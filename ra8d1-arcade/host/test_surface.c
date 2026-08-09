/*
 * Guards the contract between the machine layer and the display layer.
 *
 * video.h hands back an 8bpp indexed framebuffer at some stride and expects
 * the machine to draw a PACMAN_W x PACMAN_H window into it. That lets the
 * machine render straight into the display buffer with no intermediate copy,
 * which is worth 64 KB of memcpy per frame - but only if the stride plumbing
 * is exactly right.
 *
 * So: render once to the built-in buffer, once into a deliberately wider
 * external buffer, and require that the window is byte-identical and that
 * nothing outside it was touched.
 *
 *   ./test_surface [image.bin]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pacman.h"
#include "romimage.h"

#define EXT_STRIDE 400
#define EXT_ROWS   (PACMAN_H + 8)
#define GUARD      0xAA

static pacman_t m;
static uint8_t ext[EXT_STRIDE * EXT_ROWS];
static uint8_t ref[PACMAN_FB_PIXELS];

int main(int argc, char **argv)
{
	const char *path = (argc > 1) ? argv[1] : "testrom.bin";
	FILE *f = fopen(path, "rb");

	if (!f) {
		fprintf(stderr, "cannot open %s\n", path);
		return 1;
	}

	static uint8_t img[1 << 20];
	size_t n = fread(img, 1, sizeof(img), f);

	fclose(f);

	romimg_t r;

	if (romimg_parse(img, n, true, &r) != ROMIMG_OK ||
	    romimg_check_pacman(&r) != ROMIMG_OK) {
		fprintf(stderr, "bad rom image\n");
		return 1;
	}
	if (!pacman_load(&m, r.cpu, r.gfx, r.prom)) {
		return 1;
	}

	/* Run far enough in that sprites are on screen and moving. */
	for (int i = 0; i < 100; i++) {
		pacman_run_frame(&m);
	}
	memcpy(ref, m.fb, sizeof(ref));

	/* Same state, rendered into somebody else's wider buffer. */
	memset(ext, GUARD, sizeof(ext));
	pacman_set_surface(&m, ext, EXT_STRIDE);
	pacman_render(&m);

	for (int y = 0; y < PACMAN_H; y++) {
		if (memcmp(ext + (size_t) y * EXT_STRIDE,
			   ref + (size_t) y * PACMAN_W, PACMAN_W) != 0) {
			fprintf(stderr, "FAIL: row %d differs\n", y);
			return 1;
		}
	}

	for (int y = 0; y < EXT_ROWS; y++) {
		for (int x = 0; x < EXT_STRIDE; x++) {
			bool inside = (y < PACMAN_H) && (x < PACMAN_W);

			if (!inside && ext[(size_t) y * EXT_STRIDE + x] != GUARD) {
				fprintf(stderr, "FAIL: wrote outside the window "
					"at %d,%d\n", x, y);
				return 1;
			}
		}
	}

	/* And back to the built-in buffer. */
	pacman_set_surface(&m, NULL, 0);
	if (m.surface != m.fb || m.surface_stride != PACMAN_W) {
		fprintf(stderr, "FAIL: did not revert to the built-in surface\n");
		return 1;
	}
	pacman_render(&m);
	if (memcmp(m.fb, ref, sizeof(ref)) != 0) {
		fprintf(stderr, "FAIL: built-in render differs after revert\n");
		return 1;
	}

	printf("surface binding OK: %dx%d window identical at stride %d, "
	       "no writes outside it, revert clean\n",
	       PACMAN_W, PACMAN_H, EXT_STRIDE);
	return 0;
}
