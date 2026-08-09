/*
 * rv_machine.c - the emulated board: memory map, devices and run loop.
 *
 * See rv_machine.h for the map and notes/00-port.md for why each piece is
 * shaped the way it is. Nothing in this file knows what host it is on; it
 * reaches the world through rv_platform.h.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "cutils.h"
#include "iomem.h"
#include "riscv_cpu.h"
#include "rv_fdt.h"
#include "rv_hostsbi.h"
#include "rv_machine.h"
#include "rv_platform.h"
#include "rv_virtio.h"

/* Declared by rv_iomem.c: attach guest RAM to memory the caller owns. */
PhysMemoryRange *rv_register_ram_ptr(PhysMemoryMap *s, uint64_t addr,
				     uint64_t size, uint8_t *host_ptr,
				     int devram_flags);

/*
 * Emulated instructions between housekeeping passes. Each pass polls the
 * console and re-evaluates the timer, so this sets the guest's worst-case
 * timer jitter: at the few MIPS this core manages on a 480 MHz M85, 10k
 * instructions is on the order of a millisecond, comfortably inside a 100 Hz
 * tick. Larger slices measurably help throughput; they also make the guest
 * clock lumpy, and a lumpy clock is much harder to debug than a slow one.
 */
#define RV_SLICE_INSNS 10000

/* Console receive ring. One slice's worth of typing at any sane baud. */
#define RV_RX_RING 64

/* Paravirt bridge data window, in bytes. Fixed by the ABI in
 * ra8d1-linux/notes/05-paravirt-io.md. */
#define PV_DATA_BYTES 256

typedef struct {
	/* 8250 */
	uint8_t ier, lcr, mcr, scr, fcr;
	uint16_t divisor;
	uint8_t rx[RV_RX_RING];
	uint8_t rx_head, rx_tail;

	/* CLINT */
	uint64_t timecmp;
	uint32_t msip;

	/* PLIC */
	uint32_t plic_pending;
	uint32_t plic_served;

	/* paravirt I/O */
	uint32_t pv_status;
	uint32_t pv_result;
	uint32_t pv_i2c_bus, pv_i2c_addr, pv_i2c_wlen, pv_i2c_rlen;
	uint32_t pv_gpio_pin, pv_gpio_val, pv_gpio_flags;
	uint8_t pv_data[PV_DATA_BYTES];

	/* machine */
	RISCVCPUState *cpu;
	PhysMemoryMap *mem_map;
	uint8_t *ram;
	uint32_t ram_size;
	int stop;               /* 0 running, 1 poweroff, 2 reboot */
	uint32_t dropped_rx;
	uint32_t fdt_addr;
	uint32_t fdt_size;
	bool has_rootfs;
} RVMachine;

static RVMachine rvm;

/* ============================================================ CLINT / time */

uint64_t rv_mtime(void)
{
	/* plat_now_us() is microseconds and RV_TIMEBASE_HZ is 1 MHz, so the
	 * machine's tick and the platform's tick are the same thing. The
	 * BUILD_ASSERT-equivalent is the #error below: if the platform clock
	 * ever stops being 1 MHz this conversion has to become explicit. */
#if RV_TIMEBASE_HZ != 1000000u
#error "rv_mtime() assumes the timebase is plat_now_us()"
#endif
	return plat_now_us();
}

void rv_set_timer(uint64_t next)
{
	rvm.timecmp = next;
	/* SBI set_timer clears a pending timer interrupt as part of arming the
	 * next one; without this the guest would take the same tick twice. */
	riscv_cpu_reset_mip(rvm.cpu, MIP_STIP);
}

void rv_set_ipi(bool level)
{
	if (level) {
		riscv_cpu_set_mip(rvm.cpu, MIP_SSIP);
	} else {
		riscv_cpu_reset_mip(rvm.cpu, MIP_SSIP);
	}
}

static void timer_update(void)
{
	if (rvm.timecmp != UINT64_MAX && rv_mtime() >= rvm.timecmp) {
		riscv_cpu_set_mip(rvm.cpu, MIP_STIP);
		/* One-shot. The guest re-arms through SBI set_timer, which is
		 * also what clears STIP again. */
		rvm.timecmp = UINT64_MAX;
	}
}

static uint32_t clint_read(void *opaque, uint32_t offset, int size_log2)
{
	RVMachine *m = opaque;
	uint64_t t;

	(void)size_log2;
	switch (offset) {
	case 0x0000:
		return m->msip;
	case 0x4000:
		return (uint32_t)m->timecmp;
	case 0x4004:
		return (uint32_t)(m->timecmp >> 32);
	case 0xbff8:
		return (uint32_t)rv_mtime();
	case 0xbffc:
		t = rv_mtime();
		return (uint32_t)(t >> 32);
	default:
		return 0;
	}
}

static void clint_write(void *opaque, uint32_t offset, uint32_t val,
			int size_log2)
{
	RVMachine *m = opaque;

	(void)size_log2;
	switch (offset) {
	case 0x0000:
		m->msip = val & 1;
		/* A hart poking its own msip is asking for a software
		 * interrupt. It arrives as SSIP because nothing here runs in
		 * M-mode to take MSIP. */
		rv_set_ipi(m->msip != 0);
		break;
	case 0x4000:
		m->timecmp = (m->timecmp & 0xffffffff00000000ull) | val;
		riscv_cpu_reset_mip(m->cpu, MIP_STIP);
		break;
	case 0x4004:
		m->timecmp = (m->timecmp & 0xffffffffull) |
			     ((uint64_t)val << 32);
		riscv_cpu_reset_mip(m->cpu, MIP_STIP);
		break;
	default:
		break;
	}
}

/* ================================================================== PLIC */

/*
 * The same minimal PLIC as TinyEMU's riscv_machine.c: priority, pending and
 * enable registers are accepted and ignored, so every source behaves as if
 * enabled at a priority above the threshold, and only claim/complete does
 * real work. Linux writes the ignored registers on the way up and then only
 * ever claims and completes, so the shortcut is invisible to it.
 *
 * One context, at the standard offset 0x200000, which is index 0 - see the
 * interrupts-extended note in rv_fdt.c.
 */
#define PLIC_HART_BASE 0x200000
#define PLIC_CLAIM     (PLIC_HART_BASE + 4)

static void plic_update(RVMachine *m)
{
	if (m->plic_pending & ~m->plic_served) {
		/* SEIP only. Raising MEIP as well - which upstream does,
		 * because BBL is there to field it - would deliver a cause-11
		 * interrupt that mideleg cannot delegate, straight to an mtvec
		 * of zero. */
		riscv_cpu_set_mip(m->cpu, MIP_SEIP);
	} else {
		riscv_cpu_reset_mip(m->cpu, MIP_SEIP);
	}
}

static void plic_set_irq(RVMachine *m, int irq, int level)
{
	uint32_t mask = 1u << (irq - 1);

	if (level) {
		m->plic_pending |= mask;
	} else {
		m->plic_pending &= ~mask;
	}
	plic_update(m);
}

/* The same line, for a device that should not know how the machine is wired. */
void rv_plic_set_irq(int irq, bool level)
{
	if (irq >= 1 && irq <= 32) {
		plic_set_irq(&rvm, irq, level ? 1 : 0);
	}
}

static uint32_t plic_read(void *opaque, uint32_t offset, int size_log2)
{
	RVMachine *m = opaque;
	uint32_t mask;
	int i;

	(void)size_log2;
	if (offset == PLIC_CLAIM) {
		mask = m->plic_pending & ~m->plic_served;
		if (mask == 0) {
			return 0;
		}
		i = ctz32(mask);
		m->plic_served |= 1u << i;
		plic_update(m);
		return (uint32_t)i + 1;
	}
	return 0;
}

static void plic_write(void *opaque, uint32_t offset, uint32_t val,
		       int size_log2)
{
	RVMachine *m = opaque;

	(void)size_log2;
	if (offset == PLIC_CLAIM) {
		if (val >= 1 && val <= 32) {
			m->plic_served &= ~(1u << (val - 1));
			plic_update(m);
		}
	}
}

/* ================================================================== 8250 */

#define UART_RBR 0
#define UART_THR 0
#define UART_IER 1
#define UART_IIR 2
#define UART_FCR 2
#define UART_LCR 3
#define UART_MCR 4
#define UART_LSR 5
#define UART_MSR 6
#define UART_SCR 7

#define UART_LCR_DLAB 0x80

#define UART_IER_RDI  0x01
#define UART_IER_THRI 0x02

#define UART_LSR_DR   0x01  /* data ready */
#define UART_LSR_THRE 0x20  /* transmit holding register empty */
#define UART_LSR_TEMT 0x40  /* transmitter empty */

static bool uart_rx_avail(const RVMachine *m)
{
	return m->rx_head != m->rx_tail;
}

static void uart_update_irq(RVMachine *m)
{
	bool active = false;

	if ((m->ier & UART_IER_RDI) && uart_rx_avail(m)) {
		active = true;
	}
	/* The transmitter is always idle here - plat_putc() has completed by
	 * the time it returns - so THRI is asserted whenever it is enabled.
	 * That is what real hardware does with an empty FIFO, and the driver
	 * masks it off again once it has nothing left to send. */
	if (m->ier & UART_IER_THRI) {
		active = true;
	}
	plic_set_irq(m, RV_UART_IRQ, active ? 1 : 0);
}

static void uart_push_rx(RVMachine *m, uint8_t c)
{
	uint8_t next = (uint8_t)((m->rx_head + 1) % RV_RX_RING);

	if (next == m->rx_tail) {
		m->dropped_rx++;
		return;
	}
	m->rx[m->rx_head] = c;
	m->rx_head = next;
	uart_update_irq(m);
}

static uint32_t uart_read(void *opaque, uint32_t offset, int size_log2)
{
	RVMachine *m = opaque;
	uint32_t val;

	(void)size_log2;
	switch (offset & 7) {
	case UART_RBR:
		if (m->lcr & UART_LCR_DLAB) {
			return m->divisor & 0xff;
		}
		if (!uart_rx_avail(m)) {
			return 0;
		}
		val = m->rx[m->rx_tail];
		m->rx_tail = (uint8_t)((m->rx_tail + 1) % RV_RX_RING);
		uart_update_irq(m);
		return val;
	case UART_IER:
		if (m->lcr & UART_LCR_DLAB) {
			return m->divisor >> 8;
		}
		return m->ier;
	case UART_IIR:
		/* Bit 0 clear means "interrupt pending". Receive-data-available
		 * outranks transmitter-empty, as on the part. */
		if ((m->ier & UART_IER_RDI) && uart_rx_avail(m)) {
			val = 0x04;
		} else if (m->ier & UART_IER_THRI) {
			val = 0x02;
		} else {
			val = 0x01;
		}
		/* Top two bits report a FIFO that is enabled. */
		return val | ((m->fcr & 1) ? 0xc0 : 0x00);
	case UART_LCR:
		return m->lcr;
	case UART_MCR:
		return m->mcr;
	case UART_LSR:
		return (uart_rx_avail(m) ? UART_LSR_DR : 0) |
		       UART_LSR_THRE | UART_LSR_TEMT;
	case UART_MSR:
		/* CTS, DSR and DCD asserted: a modem that is always ready. */
		return 0xb0;
	default:
		return m->scr;
	}
}

static void uart_write(void *opaque, uint32_t offset, uint32_t val,
		       int size_log2)
{
	RVMachine *m = opaque;

	(void)size_log2;
	val &= 0xff;
	switch (offset & 7) {
	case UART_THR:
		if (m->lcr & UART_LCR_DLAB) {
			m->divisor = (uint16_t)((m->divisor & 0xff00) | val);
			break;
		}
		plat_putc((char)val);
		uart_update_irq(m);
		break;
	case UART_IER:
		if (m->lcr & UART_LCR_DLAB) {
			m->divisor = (uint16_t)((m->divisor & 0x00ff) |
						(val << 8));
			break;
		}
		m->ier = (uint8_t)val;
		uart_update_irq(m);
		break;
	case UART_FCR:
		m->fcr = (uint8_t)val;
		break;
	case UART_LCR:
		m->lcr = (uint8_t)val;
		break;
	case UART_MCR:
		m->mcr = (uint8_t)val;
		break;
	case UART_SCR:
		m->scr = (uint8_t)val;
		break;
	default:
		break;
	}
}

/* Drain the platform console into the guest's receive ring. */
static void uart_poll_input(RVMachine *m)
{
	int c;

	while (((m->rx_head + 1) % RV_RX_RING) != m->rx_tail) {
		c = plat_getc();
		if (c < 0) {
			break;
		}
		uart_push_rx(m, (uint8_t)c);
	}
}

/* ================================================================= syscon */

static uint32_t syscon_read(void *opaque, uint32_t offset, int size_log2)
{
	(void)opaque;
	(void)offset;
	(void)size_log2;
	return 0;
}

static void syscon_write(void *opaque, uint32_t offset, uint32_t val,
			 int size_log2)
{
	RVMachine *m = opaque;

	(void)size_log2;
	if (offset != 0) {
		return;
	}
	if (val == 0x5555) {
		m->stop = 1;
	} else if (val == 0x7777) {
		m->stop = 2;
	}
}

/* ========================================================== paravirt I/O */

#define PV_REG_ID         0x000
#define PV_REG_VERSION    0x004
#define PV_REG_CAPS       0x008
#define PV_REG_CMD        0x00c
#define PV_REG_STATUS     0x010
#define PV_REG_RESULT     0x014
#define PV_REG_I2C_BUS    0x018
#define PV_REG_I2C_ADDR   0x01c
#define PV_REG_I2C_WLEN   0x020
#define PV_REG_I2C_RLEN   0x024
#define PV_REG_GPIO_PIN   0x028
#define PV_REG_GPIO_VAL   0x02c
#define PV_REG_GPIO_FLAGS 0x030
#define PV_REG_DATA       0x200

#define PV_ID      0x50564930u  /* 'PVI0' */
#define PV_VERSION 1u

#define PV_CMD_NOP             0x00
#define PV_CMD_I2C_WRITE       0x01
#define PV_CMD_I2C_READ        0x02
#define PV_CMD_I2C_WRITE_READ  0x03
#define PV_CMD_I2C_PROBE       0x04
#define PV_CMD_GPIO_CONFIG     0x10
#define PV_CMD_GPIO_SET        0x11
#define PV_CMD_GPIO_GET        0x12
#define PV_CMD_GPIO_TOGGLE     0x13

static void pv_exec(RVMachine *m, uint32_t cmd)
{
	uint8_t probe;
	uint32_t wlen = m->pv_i2c_wlen;
	uint32_t rlen = m->pv_i2c_rlen;

	m->pv_result = 0;

	if (wlen > PV_DATA_BYTES || rlen > PV_DATA_BYTES) {
		m->pv_status = (uint32_t)(-EINVAL);
		return;
	}

	/*
	 * Reject zero-length transfers explicitly.
	 *
	 * This is the difference between an I2C scan that reports one device
	 * and one that reports all 112 addresses. A zero-length read handed to
	 * a Zephyr driver can return success without ever putting the address
	 * on the wire, which the guest then reads as an ACK from every address
	 * it probes. The guest driver's I2C_AQ_NO_ZERO_LEN quirk stops the
	 * kernel i2c core from generating one, but the bridge is MMIO and
	 * anything in the guest can write these registers directly, so the
	 * check belongs on this side of it too.
	 *
	 * Note the length registers are deliberately NOT cleared between
	 * commands - they are registers, and the guest owns them. Each command
	 * below reads only the one it needs, and the guest driver writes that
	 * register immediately before firing the command, so a stale value
	 * from a previous transfer cannot be picked up.
	 */
	if ((cmd == PV_CMD_I2C_WRITE && wlen == 0) ||
	    (cmd == PV_CMD_I2C_READ && rlen == 0) ||
	    (cmd == PV_CMD_I2C_WRITE_READ && (wlen == 0 || rlen == 0))) {
		m->pv_status = (uint32_t)(-EINVAL);
		return;
	}

	switch (cmd) {
	case PV_CMD_NOP:
		m->pv_status = 0;
		break;
	case PV_CMD_I2C_WRITE:
		m->pv_status = (uint32_t)plat_pv_i2c_write(
			(uint8_t)m->pv_i2c_bus, (uint8_t)m->pv_i2c_addr,
			m->pv_data, wlen);
		break;
	case PV_CMD_I2C_READ:
		m->pv_status = (uint32_t)plat_pv_i2c_read(
			(uint8_t)m->pv_i2c_bus, (uint8_t)m->pv_i2c_addr,
			m->pv_data, rlen);
		if (m->pv_status == 0) {
			m->pv_result = rlen;
		}
		break;
	case PV_CMD_I2C_WRITE_READ:
		/*
		 * One transfer with a repeated START, not a write followed by
		 * a read. Many devices require exactly that, and splitting it
		 * lets another master in between. The reply lands at DATA[0],
		 * overwriting the bytes just written.
		 */
		m->pv_status = (uint32_t)plat_pv_i2c_write_read(
			(uint8_t)m->pv_i2c_bus, (uint8_t)m->pv_i2c_addr,
			m->pv_data, wlen, m->pv_data, rlen);
		if (m->pv_status == 0) {
			m->pv_result = rlen;
		}
		break;
	case PV_CMD_I2C_PROBE:
		m->pv_status = (uint32_t)plat_pv_i2c_read(
			(uint8_t)m->pv_i2c_bus, (uint8_t)m->pv_i2c_addr,
			&probe, 1);
		break;
	case PV_CMD_GPIO_CONFIG:
		m->pv_status = (uint32_t)plat_pv_gpio_config(
			(uint8_t)m->pv_gpio_pin, m->pv_gpio_flags);
		break;
	case PV_CMD_GPIO_SET:
		m->pv_status = (uint32_t)plat_pv_gpio_set(
			(uint8_t)m->pv_gpio_pin, m->pv_gpio_val);
		break;
	case PV_CMD_GPIO_GET:
		m->pv_status = (uint32_t)plat_pv_gpio_get(
			(uint8_t)m->pv_gpio_pin, &m->pv_gpio_val);
		break;
	case PV_CMD_GPIO_TOGGLE:
		m->pv_status = (uint32_t)plat_pv_gpio_toggle(
			(uint8_t)m->pv_gpio_pin);
		break;
	default:
		m->pv_status = (uint32_t)(-ENOTSUP);
		break;
	}
}

static uint32_t pv_read(void *opaque, uint32_t offset, int size_log2)
{
	RVMachine *m = opaque;
	uint8_t n_i2c = 0, n_gpio = 0;
	uint32_t v;

	/*
	 * 32-bit accesses only. mini-rv32ima could not enforce this - its MMIO
	 * hook is not told the width - so the ABI simply declared it. This
	 * emulator does get the width, so the rule is now checked: a byte or
	 * halfword access reads zero rather than a plausible-looking value.
	 */
	if (size_log2 != 2 || (offset & 3) != 0) {
		return 0;
	}

	if (offset >= PV_REG_DATA &&
	    offset < PV_REG_DATA + PV_DATA_BYTES) {
		memcpy(&v, m->pv_data + (offset - PV_REG_DATA), 4);
		return v;
	}

	switch (offset) {
	case PV_REG_ID:
		return PV_ID;
	case PV_REG_VERSION:
		return PV_VERSION;
	case PV_REG_CAPS:
		plat_pv_caps(&n_i2c, &n_gpio);
		return (uint32_t)n_i2c | ((uint32_t)n_gpio << 8) |
		       ((uint32_t)PV_DATA_BYTES << 16);
	case PV_REG_STATUS:
		return m->pv_status;
	case PV_REG_RESULT:
		return m->pv_result;
	case PV_REG_I2C_BUS:
		return m->pv_i2c_bus;
	case PV_REG_I2C_ADDR:
		return m->pv_i2c_addr;
	case PV_REG_I2C_WLEN:
		return m->pv_i2c_wlen;
	case PV_REG_I2C_RLEN:
		return m->pv_i2c_rlen;
	case PV_REG_GPIO_PIN:
		return m->pv_gpio_pin;
	case PV_REG_GPIO_VAL:
		return m->pv_gpio_val;
	case PV_REG_GPIO_FLAGS:
		return m->pv_gpio_flags;
	default:
		return 0;
	}
}

static void pv_write(void *opaque, uint32_t offset, uint32_t val,
		     int size_log2)
{
	RVMachine *m = opaque;

	if (size_log2 != 2 || (offset & 3) != 0) {
		return;
	}

	if (offset >= PV_REG_DATA &&
	    offset < PV_REG_DATA + PV_DATA_BYTES) {
		memcpy(m->pv_data + (offset - PV_REG_DATA), &val, 4);
		return;
	}

	switch (offset) {
	case PV_REG_CMD:
		/* Synchronous: the store does not complete until the Zephyr
		 * driver call has, so the guest may read STATUS on the very
		 * next instruction. There is no busy bit because there is
		 * nothing to poll. */
		pv_exec(m, val);
		break;
	case PV_REG_I2C_BUS:
		m->pv_i2c_bus = val;
		break;
	case PV_REG_I2C_ADDR:
		m->pv_i2c_addr = val;
		break;
	case PV_REG_I2C_WLEN:
		m->pv_i2c_wlen = val;
		break;
	case PV_REG_I2C_RLEN:
		m->pv_i2c_rlen = val;
		break;
	case PV_REG_GPIO_PIN:
		m->pv_gpio_pin = val;
		break;
	case PV_REG_GPIO_VAL:
		m->pv_gpio_val = val;
		break;
	case PV_REG_GPIO_FLAGS:
		m->pv_gpio_flags = val;
		break;
	default:
		/* ID, VERSION, CAPS, STATUS and RESULT are read-only; a write
		 * is dropped rather than aliased onto a neighbour. */
		break;
	}
}

/* ================================================================= machine */

static void machine_flush_tlb_write_range(void *opaque, uint8_t *ram_addr,
					  size_t ram_size)
{
	RVMachine *m = opaque;

	riscv_cpu_flush_tlb_write_range_ram(m->cpu, ram_addr, ram_size);
}

uint8_t *rv_guest_phys_ptr(uint64_t paddr, size_t len)
{
	uint64_t base = RV_RAM_BASE;

	if (paddr < base || len > rvm.ram_size) {
		return NULL;
	}
	if (paddr - base > (uint64_t)rvm.ram_size - len) {
		return NULL;
	}
	return rvm.ram + (paddr - base);
}

void rv_request_poweroff(void)
{
	rvm.stop = 1;
}

void rv_request_reboot(void)
{
	rvm.stop = 2;
}

uint64_t rv_machine_insns(void)
{
	return rvm.cpu ? riscv_cpu_get_cycles(rvm.cpu) : 0;
}

uint32_t rv_machine_fdt(uint32_t *size)
{
	if (size != NULL) {
		*size = rvm.fdt_size;
	}
	return rvm.fdt_addr;
}

/*
 * How much RAM the kernel occupies once it is running, which is NOT the size
 * of the file.
 *
 * A RISC-V Linux Image carries a 64-byte header (arch/riscv/include/asm/
 * image.h) whose image_size field covers the kernel's .bss as well as its
 * loaded bytes. Placing anything at file-end lands it inside that .bss: the
 * kernel reserves [_start, _end] from memblock and then rejects whatever is
 * sitting there. Observed exactly once, and the kernel says so plainly -
 *
 *   INITRD: 0x818b9000+0x002eb000 overlaps in-use memory region
 *    - disabling initrd
 *
 * - which is a 25.9 MB file against a 26.2 MB image_size.
 *
 * Falls back to the file size for a payload with no header, which is what the
 * host/tests payloads are; their .bss is inside the region the caller zeroes
 * anyway.
 */
#define RV_IMAGE_MAGIC2 0x05435352u  /* "RSC\x05" */

static size_t rv_kernel_footprint(const uint8_t *kernel, size_t file_size)
{
	uint32_t magic2;
	uint64_t image_size;

	if (file_size < 64) {
		return file_size;
	}
	memcpy(&magic2, kernel + 56, sizeof(magic2));
	if (magic2 != RV_IMAGE_MAGIC2) {
		return file_size;
	}
	memcpy(&image_size, kernel + 16, sizeof(image_size));

	/*
	 * text_offset (at +8) is deliberately NOT honoured. It is 4 MB for
	 * rv32, and it exists to leave room for the M-mode firmware this
	 * machine does not have. Linux computes its load address at runtime in
	 * setup_vm() and only requires 4 MB alignment, which RV_RAM_BASE has;
	 * obeying the offset would spend 4 MB of a 64 MB guest on nothing.
	 * Verified by booting a 6.1.44 rv32 Image at RAM_BASE.
	 */
	if (image_size > file_size && image_size < 0xffffffffu) {
		return (size_t)image_size;
	}
	return file_size;
}

/*
 * "rv32" followed by the single-letter extensions misa advertises, which is
 * how TinyEMU's own FDT builder does it. Deriving it means the string cannot
 * drift from what the interpreter was compiled to execute: build with FLEN=64
 * and it says rv32imafd, build with FLEN=0 and it says rv32ima, with no second
 * place to remember to edit.
 */
static const char *rv_isa_string(uint32_t misa, char *buf, size_t buf_size)
{
	size_t n = 0;
	int i;

	if (buf_size < 5) {
		return "rv32ima";
	}
	buf[n++] = 'r';
	buf[n++] = 'v';
	buf[n++] = '3';
	buf[n++] = '2';
	for (i = 0; i < 26 && n < buf_size - 1; i++) {
		if (misa & (1u << i)) {
			buf[n++] = (char)('a' + i);
		}
	}
	buf[n] = '\0';
	return buf;
}

int rv_machine_init(const RVBootImage *img)
{
	RVMachine *m = &rvm;
	size_t ram_size;
	uint8_t *ram;
	uint32_t initrd_start = 0, initrd_end = 0;
	uint32_t fdt_addr;
	size_t fdt_size;
	size_t used;
	size_t top;
	char isa_buf[40];
	RVFdtParams fp;

	if (img == NULL || img->kernel == NULL || img->kernel_size == 0) {
		return -EINVAL;
	}

	ram = plat_guest_ram(&ram_size);
	if (ram == NULL || ram_size < (2u << 20)) {
		return -ENOMEM;
	}
	/* Round down to a page: iomem insists on a whole number of them, and
	 * a partial page at the top is not worth the special case. */
	ram_size &= ~(size_t)(DEVRAM_PAGE_SIZE - 1);
	if (ram_size > 0x40000000u) {
		/* Sv32's linear map is 1 GB. More would not be addressable. */
		ram_size = 0x40000000u;
	}

	memset(m, 0, sizeof(*m));
	m->ram = ram;
	m->ram_size = (uint32_t)ram_size;
	m->timecmp = UINT64_MAX;

	/*
	 * Layout inside guest RAM:
	 *
	 *   RAM_BASE                kernel Image
	 *   (4 KB aligned)          initrd, if any
	 *   RAM_BASE + size - 1 MB  devicetree
	 *
	 * The kernel sits at the very base rather than at +2 MB: that offset
	 * exists on other machines to leave room for the M-mode firmware, and
	 * there is none here. RAM_BASE is 4 MB aligned, which is what an Sv32
	 * kernel needs for its megapage-mapped text.
	 *
	 * The devicetree goes near the top, out of the way of anything the
	 * kernel decompresses or relocates over itself early on, and the initrd
	 * goes just below it - NOT immediately after the kernel image. See
	 * rv_kernel_footprint() for why that distinction is load-bearing.
	 */
	memcpy(ram, img->kernel, img->kernel_size);
	used = rv_kernel_footprint(img->kernel, img->kernel_size);
	if (used > ram_size) {
		return -ENOMEM;
	}

	fdt_addr = (uint32_t)(RV_RAM_BASE + ram_size - (1u << 20));
	top = ram_size - (1u << 20);

	if (img->initrd != NULL && img->initrd_size > 0) {
		size_t off = (top - img->initrd_size) & ~(size_t)0xfff;

		if (img->initrd_size > top || off < used) {
			return -ENOMEM;
		}
		memcpy(ram + off, img->initrd, img->initrd_size);
		initrd_start = (uint32_t)(RV_RAM_BASE + off);
		initrd_end = (uint32_t)(initrd_start + img->initrd_size);
		top = off;
	}

	if (used > top) {
		return -ENOMEM;
	}
	/* Everything between the kernel and whatever was placed high is zeroed,
	 * so a kernel that reads its own uninitialised .bss sees zeros rather
	 * than whatever the last boot left in SDRAM. */
	memset(ram + used, 0, top - used);

	/* -------------------------------------------------------- devices */
	m->mem_map = phys_mem_map_init();
	m->mem_map->opaque = m;
	m->mem_map->flush_tlb_write_range = machine_flush_tlb_write_range;

	if (rv_register_ram_ptr(m->mem_map, RV_RAM_BASE, ram_size, ram, 0) ==
	    NULL) {
		return -ENOMEM;
	}

	cpu_register_device(m->mem_map, RV_UART_BASE, RV_UART_SIZE, m,
			    uart_read, uart_write,
			    DEVIO_SIZE8 | DEVIO_SIZE32);
	cpu_register_device(m->mem_map, RV_CLINT_BASE, RV_CLINT_SIZE, m,
			    clint_read, clint_write, DEVIO_SIZE32);
	cpu_register_device(m->mem_map, RV_SYSCON_BASE, RV_SYSCON_SIZE, m,
			    syscon_read, syscon_write, DEVIO_SIZE32);
	cpu_register_device(m->mem_map, RV_PVIO_BASE, RV_PVIO_SIZE, m,
			    pv_read, pv_write, DEVIO_SIZE32);
	cpu_register_device(m->mem_map, RV_PLIC_BASE, RV_PLIC_SIZE, m,
			    plic_read, plic_write, DEVIO_SIZE32);

	/*
	 * The root filesystem, if there is one. Registered last so the whole
	 * memory map exists before the device can be poked, and skipped
	 * entirely when the platform has no rootfs - the guest then simply
	 * finds no block device, which is what a devicetree without the node
	 * tells it anyway.
	 */
	if (img->rootfs != NULL && img->rootfs_size >= 512) {
		int ret = rv_virtio_blk_init(m->mem_map, RV_VIRTIO_BASE,
					     RV_VIRTIO_IRQ, img->rootfs,
					     img->rootfs_size);

		if (ret != 0) {
			return ret;
		}
		m->has_rootfs = true;
	}

	m->cpu = riscv_cpu_init(m->mem_map, 32);
	if (m->cpu == NULL) {
		return -ENOMEM;
	}

	/*
	 * The devicetree is built after the CPU exists so `riscv,isa` can be
	 * derived from misa rather than asserted. The kernel decides whether to
	 * save and restore FP context from that string; a build with FLEN
	 * enabled and an isa string that forgot to say so would run F and D
	 * instructions correctly and corrupt them across every context switch.
	 */
	memset(&fp, 0, sizeof(fp));
	fp.ram_base = RV_RAM_BASE;
	fp.ram_size = m->ram_size;
	fp.timebase_hz = RV_TIMEBASE_HZ;
	fp.isa = rv_isa_string(riscv_cpu_get_misa(m->cpu), isa_buf,
			       sizeof(isa_buf));
	fp.mmu_type = "riscv,sv32";
	fp.cmdline = img->cmdline;
	fp.initrd_start = initrd_start;
	fp.initrd_end = initrd_end;
	fp.has_virtio_blk = m->has_rootfs;

	fdt_size = rv_build_fdt(ram + (fdt_addr - RV_RAM_BASE), (1u << 20), &fp);
	if (fdt_size == 0) {
		return -ENOMEM;
	}
	m->fdt_addr = fdt_addr;
	m->fdt_size = (uint32_t)fdt_size;

	riscv_cpu_boot_smode(m->cpu, RV_RAM_BASE, 0, fdt_addr);
	return 0;
}

int rv_machine_run_bounded(uint64_t insn_limit, uint64_t us_limit)
{
	RVMachine *m = &rvm;
	uint64_t deadline = us_limit ? rv_mtime() + us_limit : 0;

	while (m->stop == 0) {
		timer_update();
		uart_poll_input(m);
		/*
		 * riscv_cpu_interp() returns early on a WFI, so a guest that
		 * idles spins here rather than blocking. That is the same
		 * bargain the mini-rv32ima port made: this thread runs at a
		 * priority below everything else on the board, so the spin
		 * costs only cycles nothing else wanted.
		 */
		riscv_cpu_interp(m->cpu, RV_SLICE_INSNS);

		if (insn_limit != 0 &&
		    riscv_cpu_get_cycles(m->cpu) >= insn_limit) {
			return 2;
		}
		if (deadline != 0 && rv_mtime() >= deadline) {
			return 2;
		}
	}
	return m->stop == 2 ? 1 : 0;
}

int rv_machine_run(void)
{
	return rv_machine_run_bounded(0, 0);
}
