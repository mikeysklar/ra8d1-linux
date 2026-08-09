/*
 * Namco Pac-Man machine layer. See pacman.h for the hardware map and the
 * reference this was written from.
 *
 * Orientation note, because it drives nearly every index below: the raster
 * is 288x224 landscape and the monitor is bolted in sideways. Rather than
 * render landscape and rotate afterwards, the rotation is folded into the
 * graphics decode and the tile lookup table, so the framebuffer comes out
 * 224x288 portrait with no second pass. The transform is
 *
 *     landscape (sx, sy)  ->  portrait (px, py) = (223 - sy, sx)
 *
 * i.e. a 90-degree clockwise turn, which is MAME's ROT90 for this driver.
 */

#include "pacman.h"

#include <string.h>   /* memcpy, memset -- the only libc this file needs */

/* ------------------------------------------------------------ bus access */

/* 74LS259 latch bits at 0x5000-0x5007, data on D0. */
#define LATCH_IRQ_MASK   0x01   /* Q0 */
#define LATCH_SOUND_EN   0x02   /* Q1 */
#define LATCH_FLIPSCREEN 0x08   /* Q3 */
#define LATCH_COIN_CTR   0x80   /* Q7 */

static uint8_t bus_read(void *ud, uint16_t addr)
{
	pacman_t *m = (pacman_t *) ud;

	/* A15 is not wired at the CPU, so the whole map mirrors at +0x8000. */
	addr &= 0x7fff;

	if (addr < 0x5000) {
		/* ROM, video/colour RAM, work RAM, sprite RAM. 0x4800-0x4BFF is
		 * unmapped and floats at 0xBF; reset() prefills it with that so
		 * this stays a single index. */
		return m->mem[addr];
	}

	if (addr < 0x5100) {
		/* Only bits 6-7 of the low byte are decoded for reads. */
		switch (addr & 0xc0) {
		case 0x00: return m->in0;
		case 0x40: return m->in1;
		case 0x80: return m->dsw1;
		default:   return m->dsw2;
		}
	}

	return 0xbf;
}

static void bus_write(void *ud, uint16_t addr, uint8_t val)
{
	pacman_t *m = (pacman_t *) ud;

	addr &= 0x7fff;

	if (addr < 0x4000) {
		return;                 /* ROM */
	}

	if (addr < 0x5000) {
		if (addr >= 0x4800 && addr < 0x4c00) {
			return;         /* unmapped; keep the 0xBF fill intact */
		}
		m->mem[addr] = val;
		return;
	}

	if (addr >= 0x5100) {
		return;
	}

	/* 0x5000-0x50FF, decoded on the low byte. */
	if (addr < 0x5008) {
		/* Addressable latch: A0-A2 pick the bit, D0 is the value. */
		uint8_t bit = (uint8_t) (1u << (addr & 7));

		if (val & 1) {
			m->latch |= bit;
		} else {
			m->latch &= (uint8_t) ~bit;
		}
		return;
	}

	if (addr >= 0x5040 && addr < 0x5060) {
		/* Namco WSG. Captured so a sound stage can pick it up later;
		 * nothing here synthesises audio yet. */
		m->sound_regs[addr - 0x5040] = val;
		return;
	}

	if (addr >= 0x5060 && addr < 0x5070) {
		m->spriteram2[addr - 0x5060] = val;
		return;
	}

	if ((addr & 0xc0) == 0xc0) {
		m->watchdog = 0;        /* 0x50C0 kick */
		return;
	}

	/* 0x5008-0x503F, 0x5070-0x507F, 0x5080-0x50BF: no-ops on real hardware. */
}

static uint8_t port_in(z80 *z, uint8_t port)
{
	(void) z;
	(void) port;
	return 0xff;                /* Pac-Man decodes no input ports */
}

static void port_out(z80 *z, uint8_t port, uint8_t val)
{
	pacman_t *m = (pacman_t *) z->userdata;

	/* OUT (0),A latches the Z80 mode-2 interrupt vector. This is the only
	 * port Pac-Man decodes. */
	if (port == 0x00) {
		m->irq_vector = val;
	}
}

/* ------------------------------------------------------- graphics decode */

/* Both gfx layouts pack two bitplanes for four pixels into one byte, with
 * MAME's MSB-first bit numbering: for the k'th pixel of a group (k = x & 3),
 * plane 0 is data bit 7-k and plane 1 is data bit 3-k. */
static inline uint8_t gfx_pixel(uint8_t byte, unsigned k)
{
	return (uint8_t) (((byte >> (7u - k)) & 1u) |
	                  (((byte >> (3u - k)) & 1u) << 1));
}

/* 8x8 tiles, 16 bytes each. Landscape x 0-3 come from byte 8+y, x 4-7 from
 * byte 0+y. Stored rotated: portrait offset = lx*8 + (7 - ly). */
static void decode_tiles(pacman_t *m, const uint8_t *src)
{
	for (unsigned n = 0; n < PACMAN_NUM_TILES; n++) {
		const uint8_t *t = src + n * 16;
		uint8_t *out = m->tile_px + n * 64;

		for (unsigned ly = 0; ly < 8; ly++) {
			for (unsigned lx = 0; lx < 8; lx++) {
				uint8_t b = t[(lx < 4 ? 8u : 0u) + ly];

				out[lx * 8 + (7 - ly)] = gfx_pixel(b, lx & 3);
			}
		}
	}
}

/* 16x16 sprites, 64 bytes each. Landscape x groups 0-3/4-7/8-11/12-15 come
 * from byte bases 8/16/24/0; y 0-7 add 0-7 and y 8-15 add 32-39.
 * Stored rotated: portrait offset = lx*16 + (15 - ly). */
static void decode_sprites(pacman_t *m, const uint8_t *src)
{
	static const uint8_t xbase[4] = { 8, 16, 24, 0 };

	for (unsigned n = 0; n < PACMAN_NUM_SPRITES; n++) {
		const uint8_t *s = src + n * 64;
		uint8_t *out = m->sprite_px + n * 256;

		for (unsigned ly = 0; ly < 16; ly++) {
			unsigned ybyte = (ly < 8) ? ly : (32u + ly - 8u);

			for (unsigned lx = 0; lx < 16; lx++) {
				uint8_t b = s[xbase[lx >> 2] + ybyte];

				out[lx * 16 + (15 - ly)] = gfx_pixel(b, lx & 3);
			}
		}
	}
}

/* 82s123 palette PROM -> RGB.
 *
 * bit 7 -- 220 ohm -- BLUE      bit 2 -- 220 ohm -- GREEN
 * bit 6 -- 470 ohm -- BLUE      bit 1 -- 470 ohm -- GREEN  (bits 5..3)
 * bit 5 -- 220 ohm -- GREEN     bit 0 -- 1  kohm -- RED    (bits 2..0)
 *
 * The weights are conductance ratios normalised so that all resistors on
 * gives 255: for red/green {1k, 470, 220} -> 255/R_i divided by the sum of
 * 1/R, i.e. {33, 71, 151}; for blue only {470, 220} are fitted -> {81, 174}.
 * Both sets total 255 exactly, so no clamping is needed. */
static const uint8_t rg_weight[3] = { 33, 71, 151 };   /* 1k, 470, 220 */
static const uint8_t b_weight[2]  = { 81, 174 };       /* 470, 220     */

static void decode_palette(pacman_t *m, const uint8_t *prom)
{
	for (unsigned i = 0; i < 32; i++) {
		uint8_t p = prom[i];
		unsigned r = 0, g = 0, b = 0;

		for (unsigned k = 0; k < 3; k++) {
			if (p & (1u << k)) {
				r += rg_weight[k];
			}
			if (p & (1u << (k + 3))) {
				g += rg_weight[k];
			}
		}
		for (unsigned k = 0; k < 2; k++) {
			if (p & (1u << (k + 6))) {
				b += b_weight[k];
			}
		}

		m->pal_r[i] = (uint8_t) r;
		m->pal_g[i] = (uint8_t) g;
		m->pal_b[i] = (uint8_t) b;
	}

	/* 82s126 colour lookup PROM: 64 palettes of 4 entries, low nibble. */
	for (unsigned i = 0; i < 256; i++) {
		m->ctab[i] = (uint8_t) (prom[32 + i] & 0x0f);
	}
}

/* Portrait tile (col, row) -> video RAM offset.
 *
 * The tilemap is 36x28 in landscape. Pac-Man wires the address lines so that
 * the middle 28 columns are stored row-major in 32-byte strides, while the
 * two columns at each end -- which become the score row at the top of the
 * portrait screen and the credit row at the bottom -- are stored in the
 * leftover space at the start and end of the 1K page.
 *
 * Landscape col = portrait row, landscape row = 27 - portrait col; the rest
 * reproduces the board's decode. The signed wrap on `c` for the first two
 * columns is deliberate: c = -1 masks to 31, c = -2 to 30, which is exactly
 * where the hardware puts them. */
static void build_vram_map(pacman_t *m)
{
	for (unsigned ty = 0; ty < PACMAN_TILE_ROWS; ty++) {
		for (unsigned tx = 0; tx < PACMAN_TILE_COLS; tx++) {
			int c = (int) ty - 2;
			int r = (int) (27u - tx) + 2;
			int offs;

			if (c & 0x20) {
				offs = r + ((c & 0x1f) << 5);
			} else {
				offs = c + (r << 5);
			}

			m->vram_map[ty * PACMAN_TILE_COLS + tx] =
				(uint16_t) (offs & 0x3ff);
		}
	}
}

/* ------------------------------------------------------------------ load */

bool pacman_load(pacman_t *m, const uint8_t *rom, const uint8_t *gfx,
                 const uint8_t *prom)
{
	if (!m || !rom || !gfx || !prom) {
		return false;
	}

	memset(m, 0, sizeof(*m));

	/* Default to the built-in surface; a platform may redirect it after
	 * load with pacman_set_surface(). */
	m->surface = m->fb;
	m->surface_stride = PACMAN_W;

	memcpy(m->mem, rom, PACMAN_ROM_SIZE);
	decode_tiles(m, gfx);                       /* 5e: 0x0000-0x0FFF */
	decode_sprites(m, gfx + 0x1000);            /* 5f: 0x1000-0x1FFF */
	decode_palette(m, prom);
	build_vram_map(m);

	m->watchdog_enable = true;
	m->loaded = true;
	pacman_reset(m);
	return true;
}

void pacman_reset(pacman_t *m)
{
	/* Clear RAM but keep the ROM image and the decoded tables. */
	memset(m->mem + PACMAN_ROM_SIZE, 0, sizeof(m->mem) - PACMAN_ROM_SIZE);
	memset(m->mem + 0x4800, 0xbf, 0x400);   /* unmapped region floats high */
	memset(m->spriteram2, 0, sizeof(m->spriteram2));
	memset(m->sound_regs, 0, sizeof(m->sound_regs));

	m->latch = 0;
	m->irq_vector = 0;
	m->watchdog = 0;
	/* m->frame is deliberately not cleared: a watchdog reset mid-run should
	 * not make the frame counter lie to a benchmark. */

	m->in0 = 0xff;              /* active low: nothing pressed */
	m->in1 = 0xff;              /* bit 7 set = upright cabinet */
	m->dsw1 = PACMAN_DSW1_DEFAULT;
	m->dsw2 = 0xff;

	z80_init(&m->cpu);
	m->cpu.read_byte = bus_read;
	m->cpu.write_byte = bus_write;
	m->cpu.port_in = port_in;
	m->cpu.port_out = port_out;
	m->cpu.userdata = m;

	/* Clear whatever we are drawing into, row by row: the surface may be
	 * a window inside a wider platform framebuffer. */
	if (m->surface != NULL) {
		for (unsigned y = 0; y < PACMAN_H; y++) {
			memset(m->surface + (size_t) y * m->surface_stride, 0,
			       PACMAN_W);
		}
	}
}

/* ---------------------------------------------------------------- inputs */

void pacman_set_input(pacman_t *m, pacman_btn_t btn, bool pressed)
{
	uint8_t *port = &m->in0;
	uint8_t bit;

	switch (btn) {
	case PACMAN_BTN_UP:     bit = PACMAN_IN0_UP;       break;
	case PACMAN_BTN_LEFT:   bit = PACMAN_IN0_LEFT;     break;
	case PACMAN_BTN_RIGHT:  bit = PACMAN_IN0_RIGHT;    break;
	case PACMAN_BTN_DOWN:   bit = PACMAN_IN0_DOWN;     break;
	case PACMAN_BTN_COIN1:  bit = PACMAN_IN0_COIN1;    break;
	case PACMAN_BTN_COIN2:  bit = PACMAN_IN0_COIN2;    break;
	case PACMAN_BTN_START1: bit = PACMAN_IN1_START1; port = &m->in1; break;
	case PACMAN_BTN_START2: bit = PACMAN_IN1_START2; port = &m->in1; break;
	case PACMAN_BTN_TEST:   bit = PACMAN_IN1_SERVICE; port = &m->in1; break;
	default: return;
	}

	if (pressed) {
		*port &= (uint8_t) ~bit;    /* active low */
	} else {
		*port |= bit;
	}
}

/* ---------------------------------------------------------------- render */

static void draw_background(pacman_t *m)
{
	const uint8_t *vram = m->mem + 0x4000;
	const uint8_t *cram = m->mem + 0x4400;
	bool flip = (m->latch & LATCH_FLIPSCREEN) != 0;
	const uint32_t pitch = m->surface_stride;

	for (unsigned ty = 0; ty < PACMAN_TILE_ROWS; ty++) {
		for (unsigned tx = 0; tx < PACMAN_TILE_COLS; tx++) {
			unsigned idx = ty * PACMAN_TILE_COLS + tx;

			/* Cocktail flip mirrors the tilemap in both axes.
			 * Sprites are left alone, matching MAME. */
			if (flip) {
				idx = (PACMAN_TILE_ROWS * PACMAN_TILE_COLS - 1) - idx;
			}

			unsigned offs = m->vram_map[idx];
			const uint8_t *px = m->tile_px + (unsigned) vram[offs] * 64;
			const uint8_t *ct = m->ctab + ((cram[offs] & 0x3f) << 2);
			uint8_t *dst = m->surface + (ty * 8) * pitch + tx * 8;

			if (flip) {
				/* Mirroring the tilemap also mirrors each tile.
				 * Reversing the 64-pixel run flips both axes,
				 * which survives the portrait rotation. */
				for (unsigned iy = 0; iy < 8; iy++) {
					for (unsigned ix = 0; ix < 8; ix++) {
						dst[ix] = ct[px[63 - (iy * 8 + ix)]];
					}
					dst += pitch;
				}
			} else {
				for (unsigned iy = 0; iy < 8; iy++) {
					for (unsigned ix = 0; ix < 8; ix++) {
						dst[ix] = ct[px[ix]];
					}
					px += 8;
					dst += pitch;
				}
			}
		}
	}
}

/* Blit one 16x16 sprite. sx/sy are landscape screen coordinates; the
 * landscape clip window is x 16..271, y 0..223, which in portrait keeps
 * sprites off the score and credit rows. */
static void draw_sprite(pacman_t *m, unsigned code, unsigned color,
                        bool flipx, bool flipy, int sx, int sy)
{
	const uint8_t *px = m->sprite_px + (code & 0x3f) * 256;
	const uint8_t *ct = m->ctab + ((color & 0x3f) << 2);

	for (unsigned j = 0; j < 16; j++) {
		int ly = sy + (int) j;

		if (ly < 0 || ly > 223) {
			continue;
		}

		unsigned sj = flipy ? (15u - j) : j;
		/* Stored rotated, so the landscape row index becomes a column
		 * offset inside the sprite: 15 - sj. */
		const uint8_t *col = px + (15u - sj);
		uint8_t *dst = m->surface + (223 - ly);   /* portrait column */

		for (unsigned i = 0; i < 16; i++) {
			int lx = sx + (int) i;

			if (lx < 16 || lx > 271) {
				continue;
			}

			unsigned si = flipx ? (15u - i) : i;
			uint8_t pal = ct[col[si * 16]];

			if (pal == 0) {
				continue;       /* transparent */
			}

			/* portrait row = landscape x */
			dst[(unsigned) lx * m->surface_stride] = pal;
		}
	}
}

static void draw_sprites(pacman_t *m)
{
	const uint8_t *sr = m->mem + 0x4ff0;    /* flags+code, colour */
	const uint8_t *sr2 = m->spriteram2;     /* y, x */

	/* Priority order matters: highest sprite index draws first, and the
	 * first three sprites sit one pixel further left on Pac-Man hardware
	 * than the coordinate registers imply. */
	for (int offs = 14; offs >= 0; offs -= 2) {
		int sx = 272 - (int) sr2[offs + 1];
		int sy = (int) sr2[offs] - 31;

		if (offs <= 4) {
			sx -= 1;
		}

		unsigned code = (unsigned) (sr[offs] >> 2);
		unsigned color = (unsigned) (sr[offs + 1] & 0x1f);
		bool flipx = (sr[offs] & 1) != 0;
		bool flipy = (sr[offs] & 2) != 0;

		draw_sprite(m, code, color, flipx, flipy, sx, sy);
		/* Wraparound copy; used by the tunnel in Crush Roller and
		 * harmless here. */
		draw_sprite(m, code, color, flipx, flipy, sx - 256, sy);
	}
}

void pacman_render(pacman_t *m)
{
	draw_background(m);
	draw_sprites(m);
}

/* ------------------------------------------------------------------- run */

uint32_t pacman_run_frame(pacman_t *m)
{
	unsigned long start = m->cpu.cyc;
	unsigned long target;
	uint64_t n = 0;

	/* Active display. */
	target = start + PACMAN_CYC_TO_VBLANK;
	while (m->cpu.cyc < target) {
		z80_step(&m->cpu);
		n++;
	}

	/* VBLANK begins: raise the maskable interrupt with the latched vector.
	 * The Z80 is in IM 2, so it forms the handler address from I and this
	 * byte. */
	if (m->latch & LATCH_IRQ_MASK) {
		z80_gen_int(&m->cpu, m->irq_vector);
	}

	/* Vertical blanking interval. */
	target = start + PACMAN_CYC_PER_FRAME;
	while (m->cpu.cyc < target) {
		z80_step(&m->cpu);
		n++;
	}

	pacman_render(m);
	m->frame++;
	m->instrs += n;

	/* The board's watchdog resets the CPU after 16 frames without a kick
	 * at 0x50C0. Pac-Man's ROM relies on this during its power-on self
	 * test, so it is on by default. */
	if (m->watchdog_enable && ++m->watchdog >= 16) {
		pacman_reset(m);
	}

	return (uint32_t) (m->cpu.cyc - start);
}

/* ------------------------------------------------------------------ blit */

void pacman_set_surface(pacman_t *m, uint8_t *pixels, uint32_t stride)
{
	if (pixels == NULL) {
		m->surface = m->fb;
		m->surface_stride = PACMAN_W;
		return;
	}
	m->surface = pixels;
	m->surface_stride = stride ? stride : PACMAN_W;
}

void pacman_palette_rgb888(const pacman_t *m, uint32_t *out)
{
	for (unsigned i = 0; i < 32; i++) {
		out[i] = ((uint32_t) m->pal_r[i] << 16) |
		         ((uint32_t) m->pal_g[i] << 8) |
		         (uint32_t) m->pal_b[i];
	}
}
