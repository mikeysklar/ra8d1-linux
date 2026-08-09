/*
 * rv_platform.h - everything the emulator needs from the outside world.
 *
 * Portable by construction: stdint/stdbool/stddef only. No Zephyr, no FSP, no
 * devicetree, no board constants. Everything under emu/ reaches the hardware
 * through this header and nothing else, so the machine layer can be compiled
 * and run on the Mac (host/platform_host.c) exactly as it is on the board
 * (src/platform_zephyr.c).
 *
 * This mirrors the split ra8d1-arcade already uses. A port to another Zephyr
 * board should need a new implementation file and a devicetree, and no change
 * above this line.
 */
#ifndef RV_PLATFORM_H_
#define RV_PLATFORM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------- guest RAM */

/*
 * The backing store for guest physical RAM, as a directly writable pointer.
 *
 * On the EK-RA8D1 this is the 64 MB external SDRAM at 0x68000000, handed over
 * whole. On the host harness it is a malloc'd block. The emulator never
 * assumes an address: the guest sees this region at RV_RAM_BASE regardless of
 * where the host put it, because every guest access is resolved through the
 * host pointer, never through the guest address.
 *
 * Must be at least 4-byte aligned; 8 is better on this core. *size is set to
 * the usable byte count, which the machine layer rounds down to a page.
 */
uint8_t *plat_guest_ram(size_t *size);

/* ---------------------------------------------------------- image storage
 *
 * Two independent flash slots, in the format the rvlinux app on this board
 * already writes and its TCP pusher already tests: a 16-byte header
 * (magic[8], len, crc32) at the slot base, payload from +4096.
 *
 * Adopting that format rather than inventing one means an image pushed with
 * the tested tool boots under this app unchanged, and it is why this port has
 * no container of its own. See notes/00-port.md.
 */
enum {
	PLAT_SLOT_KERNEL = 0,
	PLAT_SLOT_ROOTFS = 1,
	PLAT_SLOT_COUNT,
};

/*
 * A slot as a directly readable pointer, or NULL if this platform has no such
 * storage. `size` is set to the size of the slot, not of the image; the
 * header carries the real length.
 *
 * The rootfs slot in particular is never copied: the block device reads it in
 * place, which is the only reason a 55 MB rootfs fits on a board with 64 MB
 * of SDRAM. A platform that cannot return a readable pointer for it has no
 * rootfs, not a slow one.
 */
const uint8_t *plat_slot_base(int slot, size_t *size);

/* ------------------------------------------------------------------ timing */

/*
 * Monotonic microseconds since boot.
 *
 * This is the guest's entire notion of time: it is the CLINT's mtime, it is
 * what SBI set_timer is compared against, and it is the timebase-frequency
 * advertised in the generated devicetree (RV_TIMEBASE_HZ below). Must be
 * cheap - the run loop calls it once per instruction slice.
 */
uint64_t plat_now_us(void);

/* Guest timebase, in Hz. Fixed at 1 MHz because plat_now_us() is the clock;
 * declared here so the FDT generator and the CLINT cannot disagree. */
#define RV_TIMEBASE_HZ 1000000u

/* ----------------------------------------------------------------- console */

/* Byte-level console. Used for the emulator's own diagnostics, for the image
 * loader, and - through the 8250 model and the SBI console calls - as the
 * guest's console too. */
void plat_putc(char c);
void plat_puts(const char *s);

/* Next byte from the console, or -1 if none is available. Must not block. */
int plat_getc(void);

/* Arm the console receive path. Call once, before the guest starts. Where the
 * platform can buffer input in an ISR this is what enables it; elsewhere it is
 * a no-op and plat_getc() polls. */
void plat_console_init(void);

/* Console bytes lost because the receive buffer was full. Reported rather than
 * discarded silently: an unreported drop counter is what made BUILD.md gotcha
 * 17 look like dead hardware instead of lost input. */
uint32_t plat_console_rx_dropped(void);

/* --------------------------------------------------------- paravirtual I/O
 *
 * Real board I2C and GPIO, reached by the guest through the MMIO bridge at
 * RV_PVIO_BASE. The ABI here is deliberately not Zephyr's: the guest driver
 * (ra8d1-linux/guest/pv-io.c) is a Linux driver and must not be coupled to a
 * Zephyr header. See ra8d1-linux/notes/05-paravirt-io.md for the register
 * map, which this port keeps byte-for-byte.
 *
 * All of these return 0 on success or a negative errno. A platform with no
 * I2C or GPIO returns -ENOTSUP (or -ENODEV per-pin) and the guest sees a
 * bridge that is present but has nothing behind it.
 */

/* Capability query. Any of the out-params may be NULL. */
void plat_pv_caps(uint8_t *n_i2c, uint8_t *n_gpio);

int plat_pv_i2c_write(uint8_t bus, uint8_t addr, const uint8_t *buf, size_t len);
int plat_pv_i2c_read(uint8_t bus, uint8_t addr, uint8_t *buf, size_t len);
int plat_pv_i2c_write_read(uint8_t bus, uint8_t addr,
			   const uint8_t *wbuf, size_t wlen,
			   uint8_t *rbuf, size_t rlen);

/* GPIO flag bits, matching notes/05-paravirt-io.md section 2. */
enum {
	PV_GPIO_INPUT      = 1u << 0,
	PV_GPIO_OUTPUT     = 1u << 1,
	PV_GPIO_PULL_UP    = 1u << 2,
	PV_GPIO_PULL_DOWN  = 1u << 3,
	PV_GPIO_OPEN_DRAIN = 1u << 4,
	PV_GPIO_INIT_HIGH  = 1u << 5,
	PV_GPIO_INIT_LOW   = 1u << 6,
	PV_GPIO_DISCONNECT = 1u << 7,
};

int plat_pv_gpio_config(uint8_t pin, uint32_t flags);
int plat_pv_gpio_set(uint8_t pin, uint32_t val);
int plat_pv_gpio_get(uint8_t pin, uint32_t *val);
int plat_pv_gpio_toggle(uint8_t pin);

/* ------------------------------------------------------------------ system */

/* The guest asked to power off or reboot, through either the syscon device or
 * SBI. The platform decides what that means; returning is allowed and leaves
 * the run loop to stop. */
void plat_poweroff(void);
void plat_reboot(void);

#endif /* RV_PLATFORM_H_ */
