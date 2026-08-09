/*
 * platform_zephyr.c - rv_platform.h on the Renesas EK-RA8D1.
 *
 * The only file in this application that knows what board it is on, and it
 * takes its facts from devicetree rather than from constants: SDRAM base and
 * size, the image partition, the console, the I2C controller and the GPIO
 * pins are all DT lookups. Porting to another Zephyr board means writing a
 * sibling of this file and a boards/<board>.overlay, and changing nothing
 * under emu/.
 */

#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

#include "rv_platform.h"
#include "telnet.h"

/* ------------------------------------------------------------- guest RAM */

#define SDRAM_NODE DT_NODELABEL(sdram1)
BUILD_ASSERT(DT_NODE_HAS_STATUS(SDRAM_NODE, okay),
	     "the guest needs external SDRAM; &sdram1 is not enabled");

#define SDRAM_BASE DT_REG_ADDR(SDRAM_NODE)
#define SDRAM_SIZE DT_REG_SIZE(SDRAM_NODE)

uint8_t *plat_guest_ram(size_t *size)
{
	/*
	 * The whole SDRAM goes to the guest. Nothing else in this application
	 * uses it: the emulator's own state is ~30 KB of .bss in internal
	 * SRAM, and there is no framebuffer in this build. The guest sees this
	 * region at 0x80000000 regardless of it living at 0x68000000 here -
	 * every guest access is resolved through a host pointer.
	 */
	*size = SDRAM_SIZE;
	return (uint8_t *)SDRAM_BASE;
}

/* ---------------------------------------------------------- image storage
 *
 * Two slots in the octo-SPI NOR, at the offsets and in the format the rvlinux
 * app on this board already uses, so an image pushed with its tested TCP
 * pusher boots here unchanged.
 *
 * Reads go through the memory-mapped window rather than flash_read(): it is
 * the same operation the driver performs internally, and for the rootfs it is
 * load-bearing rather than an optimisation, because the block device reads it
 * in place and never copies 55 MB it has nowhere to put.
 */

/* Memory-mapped window of the OSPI NOR on CS1, from the FSP BSP
 * (BSP_FEATURE_OSPI_B_DEVICE_1_START_ADDRESS). */
#define OSPI_MMAP_BASE 0x90000000UL

#define KERNEL_PART DT_NODELABEL(rvl_kernel_partition)
#define ROOTFS_PART DT_NODELABEL(rvl_rootfs_partition)

static const struct {
	uint32_t off;
	uint32_t size;
} slots[PLAT_SLOT_COUNT] = {
	[PLAT_SLOT_KERNEL] = { DT_REG_ADDR(KERNEL_PART), DT_REG_SIZE(KERNEL_PART) },
	[PLAT_SLOT_ROOTFS] = { DT_REG_ADDR(ROOTFS_PART), DT_REG_SIZE(ROOTFS_PART) },
};

BUILD_ASSERT(DT_REG_ADDR(KERNEL_PART) == 0x40000,
	     "the kernel slot must stay where the rvlinux pusher writes it");
BUILD_ASSERT(DT_REG_ADDR(ROOTFS_PART) ==
		     DT_REG_ADDR(KERNEL_PART) + DT_REG_SIZE(KERNEL_PART),
	     "the rootfs slot must follow the kernel slot with no gap");

const uint8_t *plat_slot_base(int slot, size_t *size)
{
	if (slot < 0 || slot >= PLAT_SLOT_COUNT) {
		*size = 0;
		return NULL;
	}
	*size = slots[slot].size;
	return (const uint8_t *)(OSPI_MMAP_BASE + slots[slot].off);
}

/* ------------------------------------------------------------------ time */

/*
 * The guest's timebase is 1 MHz, so the emulator wants elapsed microseconds.
 * k_cycle_get_32() wraps every ~8.9 s at 480 MHz - far longer than one
 * instruction slice, but not longer than a boot - so the difference is
 * accumulated into 64 bits on every call. Single-threaded by construction:
 * only the emulator thread calls this.
 */
static uint32_t cyc_last;
static uint64_t cyc_total;

uint64_t plat_now_us(void)
{
	uint32_t c = k_cycle_get_32();

	cyc_total += (uint32_t)(c - cyc_last);
	cyc_last = c;

	return cyc_total / (uint64_t)(sys_clock_hw_cycles_per_sec() / 1000000U);
}

/* --------------------------------------------------------------- console
 *
 * Two consoles, deliberately. plat_putc()/plat_getc() are the *guest's*
 * console - the 8250 model and the SBI debug console both go through them -
 * and they follow the telnet client when one is attached. plat_uart_putc()
 * and plat_uart_puts() are this application's own diagnostics and always go
 * to the physical UART, because the line telling you which address to telnet
 * to has to reach the wire that already works. See src/telnet.h.
 *
 * With CONFIG_RVT_TELNET off the telnet hooks compile to `return false` and
 * `return -1`, and this is the plain UART console it always was.
 */

static const struct device *const console =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
/*
 * UART receive ring, filled by the ISR and drained by the emulator.
 *
 * Without it the console loses input at any usable baud, and the failure is
 * far worse than dropped characters. The guest looks at the UART exactly once
 * per emulator slice - RV_SLICE_INSNS (10000) instructions, ~667 us at the
 * measured 15.37 MIPS - and uart_poll_in() holds a single byte between looks.
 * At 460800 a byte arrives every 22 us, so ~30 of every 31 bytes of a pasted
 * line were dropped on the floor.
 *
 * Measured before this ring existed: a 40-character line sent at wire speed
 * hung the console permanently, at 921600 *and* at 460800 - identical
 * thresholds, which is what proved the baud rate was never the problem. The
 * hang is permanent because losing the trailing newline leaves the guest
 * shell blocked in read() forever, so it looks like a dead board rather than
 * like corrupted input. That is BUILD.md gotcha 17.
 *
 * The ISR decouples the wire from the slice: bytes land in this ring at
 * interrupt time and the emulator takes them whenever it next looks.
 */
RING_BUF_DECLARE(uart_rx_rb, CONFIG_RVT_UART_RX_RING);

/* Bytes lost to a full ring. Never silently discarded: the emulator's own
 * 64-byte 8250 ring counts drops in dropped_rx and reports them nowhere,
 * which is exactly why this bug stayed unexplained for so long. */
static uint32_t uart_rx_dropped;

static void uart_rx_isr(const struct device *dev, void *user_data)
{
	uint8_t buf[32];

	ARG_UNUSED(user_data);

	/* Returns void in this Zephyr; older releases returned an int status,
	 * so do not reintroduce a test on it. */
	uart_irq_update(dev);

	while (uart_irq_rx_ready(dev) == 1) {
		int n = uart_fifo_read(dev, buf, sizeof(buf));

		if (n <= 0) {
			break;
		}
		if (ring_buf_put(&uart_rx_rb, buf, (uint32_t)n) < (uint32_t)n) {
			uart_rx_dropped++;
		}
	}
}

uint32_t plat_console_rx_dropped(void)
{
	return uart_rx_dropped;
}

void plat_console_init(void)
{
	uart_irq_rx_disable(console);
	uart_irq_tx_disable(console);
	uart_irq_callback_user_data_set(console, uart_rx_isr, NULL);
	uart_irq_rx_enable(console);
}
#else
uint32_t plat_console_rx_dropped(void)
{
	return 0;
}

void plat_console_init(void)
{
}
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */

void plat_uart_putc(char c)
{
	uart_poll_out(console, (unsigned char)c);
}

void plat_uart_puts(const char *s)
{
	while (*s != '\0') {
		plat_uart_putc(*s++);
	}
}

void plat_putc(char c)
{
	if (rvt_console_out(c)) {
		return;
	}
	plat_uart_putc(c);
}

void plat_puts(const char *s)
{
	while (*s != '\0') {
		plat_putc(*s++);
	}
}

int plat_getc(void)
{
	unsigned char c;
	int t = rvt_console_in();

	if (t >= 0) {
		return t;
	}
	/* The physical console is still read while a client is attached, so
	 * the board never goes completely deaf on the UART. */
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	if (ring_buf_get(&uart_rx_rb, &c, 1) == 1) {
		return c;
	}
#else
	if (uart_poll_in(console, &c) == 0) {
		return c;
	}
#endif
	return -1;
}

/* ------------------------------------------------------------ paravirt IO
 *
 * Same hardware and same logical pin numbering as the mini-rv32ima port, so
 * the guest driver in ra8d1-linux/guest/pv-io.c binds here unchanged. See
 * ra8d1-linux/notes/05-paravirt-io.md section 3 for why these pins and no
 * others.
 */

#if DT_NODE_HAS_STATUS(DT_NODELABEL(iic1), okay)
#define PV_HAVE_I2C 1
static const struct device *const pv_i2c[] = {
	DEVICE_DT_GET(DT_NODELABEL(iic1)),
};
#else
#define PV_HAVE_I2C 0
static const struct device *const pv_i2c[] = { NULL };
#endif

#define PV_GPIO_RAW(nodelabel, n) \
	{ .port = DEVICE_DT_GET(DT_NODELABEL(nodelabel)), .pin = (n), \
	  .dt_flags = 0 }

static const struct gpio_dt_spec pv_gpio[] = {
	GPIO_DT_SPEC_GET(DT_NODELABEL(led1), gpios),     /* 0  LED1   P600 */
	GPIO_DT_SPEC_GET(DT_NODELABEL(led2), gpios),     /* 1  LED2   P414 */
	GPIO_DT_SPEC_GET(DT_NODELABEL(led3), gpios),     /* 2  LED3   P107 */
	GPIO_DT_SPEC_GET(DT_NODELABEL(button0), gpios),  /* 3  SW1    P009 */
	GPIO_DT_SPEC_GET(DT_NODELABEL(button1), gpios),  /* 4  SW2    P008 */
	PV_GPIO_RAW(ioport0, 10),                        /* 5  mikroBUS INT */
	PV_GPIO_RAW(ioport9, 7),                         /* 6  mikroBUS PWM */
	PV_GPIO_RAW(ioport5, 7),                         /* 7  mikroBUS RST */
};

#define PV_NGPIO ARRAY_SIZE(pv_gpio)
#define PV_NI2C  (PV_HAVE_I2C ? ARRAY_SIZE(pv_i2c) : 0)

void plat_pv_caps(uint8_t *n_i2c, uint8_t *n_gpio)
{
	if (n_i2c != NULL) {
		*n_i2c = (uint8_t)PV_NI2C;
	}
	if (n_gpio != NULL) {
		*n_gpio = (uint8_t)PV_NGPIO;
	}
}

static const struct device *pv_bus(uint8_t bus)
{
	if (PV_NI2C == 0 || bus >= PV_NI2C || !device_is_ready(pv_i2c[bus])) {
		return NULL;
	}
	return pv_i2c[bus];
}

int plat_pv_i2c_write(uint8_t bus, uint8_t addr, const uint8_t *buf,
		      size_t len)
{
	const struct device *dev = pv_bus(bus);

	if (dev == NULL) {
		return -ENODEV;
	}
	return i2c_write(dev, buf, len, addr);
}

int plat_pv_i2c_read(uint8_t bus, uint8_t addr, uint8_t *buf, size_t len)
{
	const struct device *dev = pv_bus(bus);

	if (dev == NULL) {
		return -ENODEV;
	}
	return i2c_read(dev, buf, len, addr);
}

int plat_pv_i2c_write_read(uint8_t bus, uint8_t addr,
			   const uint8_t *wbuf, size_t wlen,
			   uint8_t *rbuf, size_t rlen)
{
	const struct device *dev = pv_bus(bus);

	if (dev == NULL) {
		return -ENODEV;
	}
	/*
	 * i2c_write_read() issues a repeated START rather than a STOP between
	 * the two halves. Many devices require that, and it also stops another
	 * master slipping in between the register write and the read.
	 *
	 * wbuf and rbuf are the same buffer when this comes from the bridge -
	 * the reply overwrites the register address just sent - which is fine:
	 * the driver has consumed wbuf before it starts filling rbuf.
	 */
	return i2c_write_read(dev, addr, wbuf, wlen, rbuf, rlen);
}

static gpio_flags_t pv_map_gpio_flags(uint32_t f)
{
	gpio_flags_t z = 0;

	if (f & PV_GPIO_DISCONNECT) {
		return GPIO_DISCONNECTED;
	}
	if (f & PV_GPIO_INPUT) {
		z |= GPIO_INPUT;
	}
	if (f & PV_GPIO_OUTPUT) {
		z |= GPIO_OUTPUT;
	}
	if (f & PV_GPIO_PULL_UP) {
		z |= GPIO_PULL_UP;
	}
	if (f & PV_GPIO_PULL_DOWN) {
		z |= GPIO_PULL_DOWN;
	}
	if (f & PV_GPIO_OPEN_DRAIN) {
		z |= GPIO_OPEN_DRAIN;
	}
	if (f & PV_GPIO_INIT_HIGH) {
		z |= GPIO_OUTPUT_INIT_HIGH;
	}
	if (f & PV_GPIO_INIT_LOW) {
		z |= GPIO_OUTPUT_INIT_LOW;
	}
	return z;
}

static const struct gpio_dt_spec *pv_pin(uint8_t pin)
{
	if (pin >= PV_NGPIO || !device_is_ready(pv_gpio[pin].port)) {
		return NULL;
	}
	return &pv_gpio[pin];
}

int plat_pv_gpio_config(uint8_t pin, uint32_t flags)
{
	const struct gpio_dt_spec *spec = pv_pin(pin);

	if (spec == NULL) {
		return -ENODEV;
	}
	return gpio_pin_configure_dt(spec, pv_map_gpio_flags(flags));
}

int plat_pv_gpio_set(uint8_t pin, uint32_t val)
{
	const struct gpio_dt_spec *spec = pv_pin(pin);

	if (spec == NULL) {
		return -ENODEV;
	}
	return gpio_pin_set_dt(spec, val != 0);
}

int plat_pv_gpio_get(uint8_t pin, uint32_t *val)
{
	const struct gpio_dt_spec *spec = pv_pin(pin);
	int ret;

	if (spec == NULL) {
		return -ENODEV;
	}
	/* Logical, not raw: the buttons are active-low with the polarity in
	 * devicetree, so a pressed SW1 reads 1 here. */
	ret = gpio_pin_get_dt(spec);
	if (ret < 0) {
		return ret;
	}
	*val = (uint32_t)ret;
	return 0;
}

int plat_pv_gpio_toggle(uint8_t pin)
{
	const struct gpio_dt_spec *spec = pv_pin(pin);

	if (spec == NULL) {
		return -ENODEV;
	}
	return gpio_pin_toggle_dt(spec);
}

/* ---------------------------------------------------------------- system */

/* On the UART, like main()'s post-run stats: these are the app's report of
 * what the guest did, and they must survive a telnet client going away at the
 * same moment the guest did. */

void plat_poweroff(void)
{
	plat_uart_puts("\r\n[guest powered off]\r\n");
}

void plat_reboot(void)
{
	plat_uart_puts("\r\n[guest requested reboot]\r\n");
}
