/*
 * platform.h - everything board-specific that is not video.
 *
 * Portable by construction: stdint/stdbool/stddef only, no Zephyr, no FSP, no
 * devicetree, no board constants. The app and the machine layer include this
 * to reach the hardware; the implementation is the only file allowed to know
 * what board it is on.
 *
 * Video deliberately lives in its own header, video.h, because the display
 * contract is larger and has its own lifecycle. The split is:
 *
 *   video.h     framebuffer, palette, scaling, vsync, present
 *   platform.h  ROM storage, wall clock, input
 *
 * A port to another Zephyr board should need a new implementation file and a
 * devicetree, and no change above this line.
 */
#ifndef ARCADE_PLATFORM_H_
#define ARCADE_PLATFORM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --------------------------------------------------------------- rom store */

/*
 * The ROM image, as a directly readable pointer.
 *
 * Platforms with memory-mapped flash return a pointer into that window and
 * the image is parsed and decoded in place, with no copy. Platforms without
 * it are free to read the image into RAM and return that instead; callers
 * cannot tell the difference and must not assume either.
 *
 * Returns NULL if the platform has no ROM storage at all. `size` is set to
 * the number of bytes readable from the returned pointer, which is the size
 * of the slot, not of the image; the container header carries the real
 * length.
 */
const uint8_t *plat_rom_base(size_t *size);

/*
 * Commit `len` bytes to the ROM slot, replacing whatever was there.
 *
 * Split into begin/write/end because erase granularity, write-block size and
 * alignment are all platform properties: some parts refuse writes that are
 * not whole aligned blocks. The implementation buffers as needed, so callers
 * may pass any chunk size.
 *
 * Returns 0 or a negative errno. Not all platforms implement this; -ENOTSUP
 * from plat_rom_write_begin() means "no writable ROM slot here".
 */
int plat_rom_write_begin(size_t len);
int plat_rom_write(const uint8_t *data, size_t len);
int plat_rom_write_end(void);

/* ------------------------------------------------------------------ timing */

/* Monotonic microseconds since boot. Must be cheap: the frame loop and the
 * benchmark both call it. */
uint64_t plat_now_us(void);

/* Busy-wait until plat_now_us() reaches `deadline_us`. */
void plat_sleep_until_us(uint64_t deadline_us);

/* ------------------------------------------------------------------- input */

/*
 * Abstract cabinet controls, active high (pressed = 1). The machine layer
 * maps these onto whatever its board's input ports look like; the platform
 * maps them from whatever the hardware has - GPIO, USB HID, a UART key, or
 * nothing at all.
 */
enum {
	PLAT_BTN_UP     = 1u << 0,
	PLAT_BTN_DOWN   = 1u << 1,
	PLAT_BTN_LEFT   = 1u << 2,
	PLAT_BTN_RIGHT  = 1u << 3,
	PLAT_BTN_FIRE   = 1u << 4,
	PLAT_BTN_COIN   = 1u << 5,
	PLAT_BTN_START1 = 1u << 6,
	PLAT_BTN_START2 = 1u << 7,
};

/* Current button state as a bitmask of PLAT_BTN_*. Returns 0 on a platform
 * with no input wired up, which is a valid configuration: the machine just
 * sees an idle cabinet. */
uint32_t plat_poll_input(void);

/* ----------------------------------------------------------------- console */

/* Byte-level console, used for diagnostics and the ROM loader. Kept here so
 * the app does not include a UART driver directly. */
void plat_putc(char c);
void plat_puts(const char *s);

/* Returns the next byte, or -1 if none is available. */
int plat_getc(void);

/* Blocks up to `timeout_ms` for a byte; -1 on timeout. */
int plat_getc_timeout(uint32_t timeout_ms);

#endif /* ARCADE_PLATFORM_H_ */
