/*
 * Namco Pac-Man arcade machine layer.
 *
 * Freestanding C99: no libc beyond memcpy/memset, no malloc, no file I/O.
 * The caller owns all storage (see pacman_t) so this drops equally well into
 * a host test harness and into a Zephyr app on the RA8D1.
 *
 * Hardware reference: MAME's Pac-Man driver, src/mame/pacman/pacman.cpp and
 * pacman_v.cpp (BSD-3-Clause, Nicola Salmoria). Only hardware facts were
 * taken from it -- register addresses, PROM wiring, gfx bit layouts, screen
 * timing; the code below is an independent implementation. Cross-checked
 * against the Pac-Man Emulation Guide at walkofmind.com/programming/pie/.
 *
 * Board summary
 *   Z80 @ 18.432 MHz / 6 = 3.072 MHz
 *   Video 6.144 MHz dot clock, 384x264 total, 288x224 active, 60.606 Hz
 *   The monitor is rotated 90 degrees, so the cabinet sees 224x288 portrait.
 *   50688 T-states per frame, 192 per scanline. VBLANK begins at line 224,
 *   i.e. 43008 T-states into the frame.
 *
 * Z80 memory map (A15 is not connected at the CPU, so everything mirrors
 * at +0x8000)
 *   0x0000-0x3FFF  16K program ROM
 *   0x4000-0x43FF  1K  video RAM  (tile codes)
 *   0x4400-0x47FF  1K  colour RAM (tile colour attributes)
 *   0x4800-0x4BFF       unmapped, reads float
 *   0x4C00-0x4FEF  work RAM
 *   0x4FF0-0x4FFF  sprite RAM  (8 x [flags+code, colour])
 *   0x5000-0x5007  write: 74LS259 addressable latch, data on D0
 *                  read : IN0
 *   0x5040-0x505F  write: Namco WSG sound registers
 *                  read : IN1
 *   0x5060-0x506F  write: sprite coordinates (8 x [y, x]), write-only
 *   0x5080         read : DSW1
 *   0x50C0         write: watchdog kick;  read: DSW2
 * All of 0x5000-0x50FF is decoded with heavy mirroring; reads only look at
 * bits 6-7 of the low byte.
 *
 * Z80 I/O space
 *   OUT (0x00),A   latches the Z80 mode-2 interrupt vector byte.
 * The CPU runs in IM 2 and this latch supplies the vector on acknowledge.
 */
#ifndef PACMAN_H_
#define PACMAN_H_

#include <stdint.h>
#include <stdbool.h>

#include "z80.h"

/* ------------------------------------------------------------ dimensions */

/* Native cabinet (portrait) resolution. The raster is 288x224 landscape;
 * everything below works in portrait and rotates on the way in, so the
 * framebuffer needs no post-rotation. */
#define PACMAN_W            224
#define PACMAN_H            288
#define PACMAN_FB_PIXELS    (PACMAN_W * PACMAN_H)   /* 64512 */

#define PACMAN_TILE_COLS    28      /* portrait */
#define PACMAN_TILE_ROWS    36

/* ROM region sizes. These are the sizes of the MAME "pacman" regions and are
 * what pacman_load() expects. */
#define PACMAN_ROM_SIZE     0x4000  /* maincpu: 6e,6f,6h,6j @ 4K each      */
#define PACMAN_GFX_SIZE     0x2000  /* gfx1: 5e tiles 4K + 5f sprites 4K   */
#define PACMAN_PROM_SIZE    0x0120  /* 32B palette 82s123.7f + 256B 82s126.4a */
#define PACMAN_SND_SIZE     0x0200  /* 82s126.1m waveforms + 82s126.3m timing */

#define PACMAN_NUM_TILES    256
#define PACMAN_NUM_SPRITES  64

/* ------------------------------------------------------------ CPU timing */

#define PACMAN_CLOCK_HZ     3072000u
#define PACMAN_CYC_PER_LINE 192u
#define PACMAN_LINES_TOTAL  264u
#define PACMAN_LINES_ACTIVE 224u
#define PACMAN_CYC_PER_FRAME (PACMAN_CYC_PER_LINE * PACMAN_LINES_TOTAL)   /* 50688 */
#define PACMAN_CYC_TO_VBLANK (PACMAN_CYC_PER_LINE * PACMAN_LINES_ACTIVE)  /* 43008 */

/* ---------------------------------------------------------------- inputs */

/* IN0 / IN1 / DSW1 are active low: a set bit means "not pressed". The
 * pacman_t fields hold the raw port value, so they initialise to 0xFF.
 * Use pacman_set_input() rather than poking them by hand. */
enum {
	/* IN0 (0x5000) */
	PACMAN_IN0_UP        = 0x01,
	PACMAN_IN0_LEFT      = 0x02,
	PACMAN_IN0_RIGHT     = 0x04,
	PACMAN_IN0_DOWN      = 0x08,
	PACMAN_IN0_RACK_TEST = 0x10,
	PACMAN_IN0_COIN1     = 0x20,
	PACMAN_IN0_COIN2     = 0x40,
	PACMAN_IN0_SERVICE1  = 0x80,

	/* IN1 (0x5040) -- joystick bits are the cocktail player 2 controls */
	PACMAN_IN1_UP        = 0x01,
	PACMAN_IN1_LEFT      = 0x02,
	PACMAN_IN1_RIGHT     = 0x04,
	PACMAN_IN1_DOWN      = 0x08,
	PACMAN_IN1_SERVICE   = 0x10,
	PACMAN_IN1_START1    = 0x20,
	PACMAN_IN1_START2    = 0x40,
	PACMAN_IN1_UPRIGHT   = 0x80,   /* 1 = upright cabinet, 0 = cocktail */
};

/* DSW1 (0x5080) is active high and reads as a plain settings byte.
 * Factory default for a US Pac-Man cabinet: 1 coin/1 credit, 3 lives,
 * bonus at 10000, normal difficulty, normal ghost names. */
enum {
	PACMAN_DSW_COIN_2C1C   = 0x03,
	PACMAN_DSW_COIN_1C1C   = 0x01,
	PACMAN_DSW_COIN_1C2C   = 0x02,
	PACMAN_DSW_COIN_FREE   = 0x00,

	PACMAN_DSW_LIVES_1     = 0x00,
	PACMAN_DSW_LIVES_2     = 0x04,
	PACMAN_DSW_LIVES_3     = 0x08,
	PACMAN_DSW_LIVES_5     = 0x0c,

	PACMAN_DSW_BONUS_10000 = 0x00,
	PACMAN_DSW_BONUS_15000 = 0x10,
	PACMAN_DSW_BONUS_20000 = 0x20,
	PACMAN_DSW_BONUS_NONE  = 0x30,

	PACMAN_DSW_DIFF_NORMAL = 0x40,
	PACMAN_DSW_GHOST_NORMAL = 0x80,
};

#define PACMAN_DSW1_DEFAULT \
	(PACMAN_DSW_COIN_1C1C | PACMAN_DSW_LIVES_3 | PACMAN_DSW_BONUS_10000 | \
	 PACMAN_DSW_DIFF_NORMAL | PACMAN_DSW_GHOST_NORMAL)

/* Logical buttons for pacman_set_input(). */
typedef enum {
	PACMAN_BTN_UP, PACMAN_BTN_DOWN, PACMAN_BTN_LEFT, PACMAN_BTN_RIGHT,
	PACMAN_BTN_COIN1, PACMAN_BTN_COIN2,
	PACMAN_BTN_START1, PACMAN_BTN_START2,
	PACMAN_BTN_TEST,
} pacman_btn_t;

/* ----------------------------------------------------------------- state */

typedef struct {
	z80 cpu;

	/* Flat 0x0000-0x4FFF image: 16K ROM then 4K of RAM. Reads below 0x5000
	 * are a single bounds check plus an index, which is the hot path. */
	uint8_t  mem[0x5000];

	/* Write-only registers that do not live in mem[]. */
	uint8_t  spriteram2[16];   /* 0x5060-0x506F: 8 x [y, x] */
	uint8_t  latch;            /* 74LS259 outputs, one bit per Q */
	uint8_t  irq_vector;       /* set by OUT (0),A */
	uint8_t  sound_regs[32];   /* 0x5040-0x505F, captured but not synthesised */

	/* Input port images, active low; DSW1 is active high. */
	uint8_t  in0, in1, dsw1, dsw2;

	/* Decoded graphics, one byte per pixel holding the 2-bit colour index.
	 * Decoding once at load time turns the render into a byte copy. Stored
	 * already rotated into portrait orientation. 32 KB total. */
	uint8_t  tile_px[PACMAN_NUM_TILES * 8 * 8];
	uint8_t  sprite_px[PACMAN_NUM_SPRITES * 16 * 16];

	/* 32-entry RGB palette from the 82s123, expanded through the resistor
	 * ladder, plus the 256-entry colour lookup PROM. These are the values
	 * the hardware's resistor network actually produces; converting them to
	 * whatever a panel wants is the platform's business, not this layer's. */
	uint8_t  pal_r[32], pal_g[32], pal_b[32];
	uint8_t  ctab[256];

	/* Portrait tile index -> video RAM offset. Precomputed; see pacman.c. */
	uint16_t vram_map[PACMAN_TILE_COLS * PACMAN_TILE_ROWS];

	/* Default drawing surface: one byte per pixel holding a 0-31 palette
	 * index. Used unless pacman_set_surface() points the renderer at memory
	 * the platform owns, which on a real panel avoids a 64 KB copy per
	 * frame. */
	uint8_t  fb[PACMAN_FB_PIXELS];

	/* Where pacman_render() actually writes. Defaults to fb / PACMAN_W. */
	uint8_t  *surface;
	uint32_t surface_stride;

	/* Housekeeping. */
	uint32_t frame;
	uint64_t instrs;           /* Z80 instructions retired; drives the
	                            * benchmark on host and on target. */
	uint8_t  watchdog;         /* frames since the last 0x50C0 kick */
	bool     watchdog_enable;
	bool     loaded;
} pacman_t;

/* ------------------------------------------------------------------- API */

/* Decode ROMs and reset. rom/gfx/prom point at PACMAN_ROM_SIZE /
 * PACMAN_GFX_SIZE / PACMAN_PROM_SIZE bytes; they are copied or decoded, so
 * they may live in memory-mapped flash and need not stay valid afterwards.
 * Returns false only if a pointer is NULL. */
bool pacman_load(pacman_t *m, const uint8_t *rom, const uint8_t *gfx,
                 const uint8_t *prom);

/* Reset the CPU and the board latches. Keeps the decoded ROMs. */
void pacman_reset(pacman_t *m);

/* Run exactly one video frame: PACMAN_CYC_PER_FRAME T-states with a mode-2
 * interrupt raised at the start of VBLANK, then render into m->fb.
 * Returns the number of T-states actually executed (slightly over the
 * nominal figure, since the last instruction is not split). */
uint32_t pacman_run_frame(pacman_t *m);

/* Render the current video/colour/sprite RAM into the drawing surface
 * without running the CPU. pacman_run_frame() calls this for you. */
void pacman_render(pacman_t *m);

/* Point the renderer at memory somebody else owns: `pixels` must be at least
 * PACMAN_H rows of `stride` bytes, one byte per pixel of palette index. Pass
 * pixels = NULL to go back to the built-in m->fb.
 *
 * This is how the machine draws straight into a platform framebuffer with no
 * intermediate copy. The machine still knows nothing about the panel: it only
 * ever writes PACMAN_W x PACMAN_H bytes of palette index. */
void pacman_set_surface(pacman_t *m, uint8_t *pixels, uint32_t stride);

/* The 32-entry hardware palette as 0x00RRGGBB, for handing to a display
 * layer. `out` must have room for 32 entries. */
void pacman_palette_rgb888(const pacman_t *m, uint32_t *out);

/* Press or release a button. */
void pacman_set_input(pacman_t *m, pacman_btn_t btn, bool pressed);

#endif /* PACMAN_H_ */
