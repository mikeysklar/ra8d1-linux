/*
 * Host harness for the Pac-Man machine layer.
 *
 * Loads a ROM container, runs frames, and writes the framebuffer out as
 * binary PPM so the result can be eyeballed. This is the check that the
 * video RAM mapping, the gfx bit order and the portrait rotation are right:
 * if the text in the synthetic test pattern reads correctly and the corner
 * markers land in the right corners, the orientation work is correct.
 *
 *   ./test_pacman [image.bin] [frames] [outprefix]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pacman.h"
#include "romimage.h"

static pacman_t machine;    /* 100+ KB; too big for the stack */

static uint8_t *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");

	if (!f) {
		fprintf(stderr, "cannot open %s\n", path);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	rewind(f);

	uint8_t *buf = malloc((size_t) n);

	if (!buf || fread(buf, 1, (size_t) n, f) != (size_t) n) {
		fprintf(stderr, "cannot read %s\n", path);
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*len = (size_t) n;
	return buf;
}

static int write_ppm(const char *path, const pacman_t *m)
{
	FILE *f = fopen(path, "wb");

	if (!f) {
		fprintf(stderr, "cannot write %s\n", path);
		return 1;
	}
	fprintf(f, "P6\n%d %d\n255\n", PACMAN_W, PACMAN_H);
	for (int y = 0; y < PACMAN_H; y++) {
		const uint8_t *row = m->surface + (size_t) y * m->surface_stride;

		for (int x = 0; x < PACMAN_W; x++) {
			uint8_t c = row[x] & 0x1f;
			uint8_t px[3] = { m->pal_r[c], m->pal_g[c], m->pal_b[c] };

			fwrite(px, 1, 3, f);
		}
	}
	fclose(f);
	return 0;
}

/* Coarse ASCII rendering of the framebuffer, so a run over a terminal or in
 * a log still shows whether the picture is sane. */
static void dump_ascii(const pacman_t *m)
{
	static const char ramp[] = " .:-=+*#%@";

	for (int y = 0; y < PACMAN_H; y += 8) {
		for (int x = 0; x < PACMAN_W; x += 4) {
			unsigned lum = 0, n = 0;

			for (int dy = 0; dy < 8; dy++) {
				for (int dx = 0; dx < 4; dx++) {
					uint8_t c = m->surface[(size_t) (y + dy) *
						m->surface_stride + x + dx] & 0x1f;

					lum += (unsigned) (m->pal_r[c] * 30 +
					                   m->pal_g[c] * 59 +
					                   m->pal_b[c] * 11) / 100;
					n++;
				}
			}
			lum /= n;
			putchar(ramp[lum * (sizeof(ramp) - 2) / 255]);
		}
		putchar('\n');
	}
}

int main(int argc, char **argv)
{
	const char *path = (argc > 1) ? argv[1] : "testrom.bin";
	int frames = (argc > 2) ? atoi(argv[2]) : 120;
	const char *prefix = (argc > 3) ? argv[3] : "frame";

	size_t len;
	uint8_t *img = slurp(path, &len);

	if (!img) {
		return 1;
	}

	romimg_t r;
	romimg_err_t e = romimg_parse(img, len, true, &r);

	if (e != ROMIMG_OK) {
		fprintf(stderr, "romimg_parse: %s\n", romimg_strerror(e));
		return 1;
	}
	e = romimg_check_pacman(&r);
	if (e != ROMIMG_OK) {
		fprintf(stderr, "romimg_check_pacman: %s\n", romimg_strerror(e));
		return 1;
	}
	printf("image ok: machine %u, %u bytes%s\n", r.machine, r.total_len,
	       (r.flags & ROMIMG_FLAG_SYNTHETIC) ? " (synthetic)" : "");

	if (!pacman_load(&machine, r.cpu, r.gfx, r.prom)) {
		fprintf(stderr, "pacman_load failed\n");
		return 1;
	}

	for (int i = 0; i < frames; i++) {
		pacman_run_frame(&machine);
	}

	printf("ran %u frames, %lu T-states, pc=%04X latch=%02X vec=%02X "
	       "wd=%u\n",
	       machine.frame, machine.cpu.cyc, machine.cpu.pc, machine.latch,
	       machine.irq_vector, machine.watchdog);

	char name[256];

	snprintf(name, sizeof(name), "%s.ppm", prefix);
	if (write_ppm(name, &machine) != 0) {
		return 1;
	}
	printf("wrote %s\n", name);

	dump_ascii(&machine);

	free(img);
	return 0;
}
