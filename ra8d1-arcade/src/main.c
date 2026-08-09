/*
 * Namco Pac-Man arcade hardware, emulated natively on a Cortex-M85.
 *
 * This runs directly on the MCU. It is deliberately NOT hosted inside the
 * emulated RISC-V Linux that also lives on the EK-RA8D1: nesting an M85
 * emulating a RISC-V emulating a Z80 compounds to well over a thousand host
 * cycles per Z80 instruction, roughly ten times too slow for Pac-Man's
 * 3.072 MHz. Emulating the Z80 directly costs tens of cycles.
 *
 * Layering, which is the point of how this file is written:
 *
 *   emu/       portable C99, no Zephyr, no board. Compiles on the Mac for
 *              the test harnesses and on the target unchanged.
 *   src/platform.h, video.h
 *              the thin shim: ROM storage, timing, input, framebuffer.
 *   src/platform_zephyr.c, video.c
 *              the only files that know the board, and they take their facts
 *              from devicetree rather than from constants.
 *   src/main.c this file. Glue only: no addresses, no panel size, no flash
 *              offsets, no driver includes.
 *
 * ROMs: none are shipped. Push an image built by tools/mkromimage.py (your
 * own dump) or tools/mktestrom.py (synthetic, no arcade ROM involved) with
 * tools/pushrom.py. It persists across resets and app reflashes.
 */

/* No Zephyr headers here on purpose: this file is glue, and everything it
 * needs from the system comes through platform.h and video.h. That keeps it
 * compilable against any implementation of the shim. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pacman.h"
#include "romimage.h"
#include "platform.h"

/* The display stage owns video.h/video.c. Until the board has a panel - a
 * chosen zephyr,display, which CMakeLists turns into ARCADE_HAS_DISPLAY -
 * this app runs headless and never references it, so the two stages can land
 * independently. */
#if defined(ARCADE_HAS_DISPLAY)
#include "video.h"
#endif

/* ------------------------------------------------------------------- state */

/* ~118 KB. Lives in .bss, i.e. internal SRAM: the Z80's whole address space
 * is 20 KB and the decoded graphics are 32 KB, so the emulator's hot working
 * set stays on-chip and never touches external memory. That is the single
 * biggest performance decision in the project. */
static pacman_t machine;

/* ------------------------------------------------------------- console fmt */

static void us(const char *s)
{
	plat_puts(s);
}

static void udec(uint32_t v)
{
	char b[12];
	int n = 0;

	if (v == 0) {
		plat_putc('0');
		return;
	}
	while (v) {
		b[n++] = (char) ('0' + v % 10);
		v /= 10;
	}
	while (n--) {
		plat_putc(b[n]);
	}
}

/* Fixed-point decimal with two places, for rates and multiples. */
static void udec2(uint32_t whole, uint32_t hundredths)
{
	udec(whole);
	plat_putc('.');
	plat_putc((char) ('0' + (hundredths / 10) % 10));
	plat_putc((char) ('0' + hundredths % 10));
}

/* ----------------------------------------------------------------- rom load */

static bool rom_present(romimg_t *img)
{
	size_t slot;
	const uint8_t *base = plat_rom_base(&slot);

	if (base == NULL) {
		us("rom: no ROM storage on this platform\r\n");
		return false;
	}

	romimg_err_t e = romimg_parse(base, slot, true, img);

	if (e == ROMIMG_OK) {
		e = romimg_check_pacman(img);
	}
	if (e != ROMIMG_OK) {
		us("rom: ");
		us(romimg_strerror(e));
		us("\r\n");
		return false;
	}

	us("rom: ok, ");
	udec(img->total_len);
	us(" bytes");
	if (img->flags & ROMIMG_FLAG_SYNTHETIC) {
		us(" (synthetic test image)");
	}
	us("\r\n");
	return true;
}

/*
 * Receive a ROM image over the console and hand it to the platform.
 *
 * The container carries its own length and CRC, so the wire protocol is just
 * 'S', a 4-byte little-endian length, then the bytes. Erase granularity,
 * write-block size and the hold-back of the header block are all the
 * platform's problem, not this function's.
 */
static int rom_receive(void)
{
	uint32_t len = 0;
	int c;

	us("\r\nLOADER: send 'S' then <len:4 LE> then the image\r\n");

	do {
		c = plat_getc_timeout(3600u * 1000u);
		if (c < 0) {
			us("LOADER: timeout\r\n");
			return -1;
		}
	} while (c != 'S');

	for (int i = 0; i < 4; i++) {
		c = plat_getc_timeout(5000u);
		if (c < 0) {
			return -1;
		}
		len |= (uint32_t) c << (8 * i);
	}

	us("LOADER: len=");
	udec(len);
	us(", erasing\r\n");

	if (plat_rom_write_begin(len) != 0) {
		us("LOADER: cannot open ROM slot\r\n");
		return -1;
	}

	/* The erase is long enough that anything sent meanwhile is already lost
	 * in the FIFO. Drain, then say explicitly when ready. */
	while (plat_getc() >= 0) {
		/* drain */
	}
	us("\r\n<RDY>\r\n");

	for (uint32_t off = 0; off < len; off++) {
		uint8_t byte;

		c = plat_getc_timeout(10000u);
		if (c < 0) {
			us("LOADER: rx timeout at ");
			udec(off);
			us("\r\n");
			return -1;
		}
		byte = (uint8_t) c;

		if (plat_rom_write(&byte, 1) != 0) {
			us("LOADER: write failed at ");
			udec(off);
			us("\r\n");
			return -1;
		}
		if ((off & 0xFFF) == 0) {
			plat_putc('.');
		}
	}

	if (plat_rom_write_end() != 0) {
		us("\r\nLOADER: commit failed\r\n");
		return -1;
	}

	us("\r\nLOADER: done\r\n");
	return 0;
}

/* --------------------------------------------------------------- benchmark */

/*
 * How much faster than real time can this board run the machine?
 *
 * CPU and render only. The cost of getting pixels onto the panel is measured
 * by the video layer, which is where the scaling strategy lives; see
 * notes/02-video.md. Duplicating a blit here would only produce a second,
 * stale number for the same thing.
 */
static void benchmark(void)
{
	const int warm = 30;
	const int n = 300;

	machine.watchdog_enable = false;    /* a reset mid-run skews the counts */

	for (int i = 0; i < warm; i++) {
		pacman_run_frame(&machine);
	}

	uint64_t i0 = machine.instrs;
	unsigned long c0 = machine.cpu.cyc;

	uint64_t t0 = plat_now_us();

	for (int i = 0; i < n; i++) {
		pacman_run_frame(&machine);
	}
	uint64_t t_full = plat_now_us() - t0;

	uint64_t instrs = machine.instrs - i0;
	uint32_t tstates = (uint32_t) (machine.cpu.cyc - c0);

	t0 = plat_now_us();
	for (int i = 0; i < n; i++) {
		pacman_render(&machine);
	}
	uint64_t t_render = plat_now_us() - t0;

	uint64_t t_cpu = (t_full > t_render) ? (t_full - t_render) : 1;

	us("\r\n--- benchmark over ");
	udec(n);
	us(" frames ---\r\n");

	us("z80 instructions   ");
	udec((uint32_t) instrs);
	us("  (");
	udec((uint32_t) (instrs / n));
	us(" per frame)\r\n");

	us("t-states per instr ");
	udec2(tstates / (uint32_t) instrs,
	      (uint32_t) ((uint64_t) tstates * 100 / instrs) % 100);
	us("\r\n");

	us("cpu emulation      ");
	udec((uint32_t) (t_cpu / n));
	us(" us/frame, ");
	udec((uint32_t) (instrs * 1000000u / t_cpu / 1000u));
	us(" K z80-instr/s\r\n");

	us("render 224x288     ");
	udec((uint32_t) (t_render / n));
	us(" us/frame\r\n");

	uint32_t us_frame = (uint32_t) (t_full / n);
	uint32_t us_budget = 1000000u / 61u;

	us("emulation total    ");
	udec(us_frame);
	us(" us of a ");
	udec(us_budget);
	us(" us frame budget\r\n");

	if (us_frame) {
		uint32_t x100 = us_budget * 100u / us_frame;

		us("real-time multiple ");
		udec2(x100 / 100, x100 % 100);
		us("x  (emulation only, display not included)\r\n");
	}

	machine.watchdog_enable = true;
	pacman_reset(&machine);
}

/* -------------------------------------------------------------------- main */

/* Map the platform's abstract buttons onto this machine's cabinet. Which
 * physical control produces which bit is the platform's business; which
 * cabinet input it drives is this machine's. */
static void apply_input(uint32_t btns)
{
	static const struct {
		uint32_t bit;
		pacman_btn_t btn;
	} map[] = {
		{ PLAT_BTN_UP,     PACMAN_BTN_UP },
		{ PLAT_BTN_DOWN,   PACMAN_BTN_DOWN },
		{ PLAT_BTN_LEFT,   PACMAN_BTN_LEFT },
		{ PLAT_BTN_RIGHT,  PACMAN_BTN_RIGHT },
		{ PLAT_BTN_COIN,   PACMAN_BTN_COIN1 },
		{ PLAT_BTN_START1, PACMAN_BTN_START1 },
		{ PLAT_BTN_START2, PACMAN_BTN_START2 },
	};

	for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		pacman_set_input(&machine, map[i].btn,
				 (btns & map[i].bit) != 0);
	}
}

int main(void)
{
	us("\r\n\r\nra8d1-arcade: pac-man, native on cortex-m85\r\n");

	romimg_t img;

	if (!rom_present(&img)) {
		us("\r\nNo usable ROM image in flash.\r\n");
		us("Build one with tools/mktestrom.py (synthetic, no arcade\r\n");
		us("ROM needed) or tools/mkromimage.py (your own dump), then\r\n");
		us("push it with tools/pushrom.py.\r\n");

		if (rom_receive() != 0 || !rom_present(&img)) {
			us("no valid image; halting\r\n");
			return 0;
		}
	}

	if (!pacman_load(&machine, img.cpu, img.gfx, img.prom)) {
		us("pacman_load failed\r\n");
		return 0;
	}
	us("machine loaded, state ");
	udec((uint32_t) sizeof(machine) / 1024);
	us(" KB in internal SRAM\r\n");

	/*
	 * Draw straight into the platform's framebuffer when there is one.
	 * pacman_render() writes PACMAN_W x PACMAN_H bytes of palette index at
	 * whatever stride it is given, so this costs nothing and saves a 64 KB
	 * copy per frame. Without a display the machine keeps its own buffer
	 * and everything still runs.
	 */
	bool have_display = false;

#if defined(ARCADE_HAS_DISPLAY)
	if (plat_video_init(PACMAN_W, PACMAN_H) == 0) {
		uint32_t pal[32];

		pacman_set_surface(&machine, plat_get_framebuffer(),
				   plat_fb_stride());
		pacman_palette_rgb888(&machine, pal);
		plat_set_palette(pal, 0, 32);
		have_display = true;

		us("display ");
		udec(plat_fb_width() * plat_scale());
		plat_putc('x');
		udec(plat_fb_height() * plat_scale());
		us(" at ");
		udec(plat_scale());
		us("x scale\r\n");
	} else {
		us("display init failed; running headless\r\n");
	}
#else
	us("no display configured; running headless\r\n");
#endif

	benchmark();

	us("\r\nfree-running at 60.6 Hz\r\n");

	const uint64_t frame_us = 1000000u / 61u;
	uint64_t next = plat_now_us();

	for (;;) {
		apply_input(plat_poll_input());

		if (have_display) {
#if defined(ARCADE_HAS_DISPLAY)
			plat_wait_vsync();
#endif
		}

		pacman_run_frame(&machine);

		if (have_display) {
#if defined(ARCADE_HAS_DISPLAY)
			plat_present();
#endif
		}

		next += frame_us;
		plat_sleep_until_us(next);

		if (machine.frame % 303 == 0) {
			us("frame ");
			udec(machine.frame);
			us("\r\n");
		}
	}

	return 0;
}
