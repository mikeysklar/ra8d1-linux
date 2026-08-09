/*
 * Parser for the ROM container written by tools/romimage.py.
 *
 * Parses an image in place and hands back pointers into it, without copying,
 * so a platform whose flash is memory-mapped can point this straight at the
 * mapped window and let the emulator decode out of it. Where that window
 * lives is the platform's business (see src/platform.h); nothing here knows
 * or cares.
 *
 * Freestanding: no allocation, no libc beyond what pacman.c already needs.
 */
#ifndef ROMIMAGE_H_
#define ROMIMAGE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define ROMIMG_MAGIC         "RA8ARC01"
#define ROMIMG_HDR_SIZE      64u
#define ROMIMG_MACHINE_PACMAN 1u
#define ROMIMG_FLAG_SYNTHETIC 1u

typedef struct {
	uint32_t machine;
	uint32_t flags;
	uint32_t total_len;
	uint32_t crc32;

	const uint8_t *cpu;   uint32_t cpu_len;
	const uint8_t *gfx;   uint32_t gfx_len;
	const uint8_t *prom;  uint32_t prom_len;
	const uint8_t *snd;   uint32_t snd_len;
} romimg_t;

typedef enum {
	ROMIMG_OK = 0,
	ROMIMG_BAD_MAGIC,
	ROMIMG_BAD_HEADER,     /* header size or total length is not sane */
	ROMIMG_TRUNCATED,      /* a region runs past the end of the image */
	ROMIMG_BAD_CRC,
	ROMIMG_BAD_MACHINE,    /* unknown machine id */
	ROMIMG_BAD_REGION,     /* region size is wrong for this machine */
} romimg_err_t;

/* Parse `len` bytes at `base`. Pass len = 0 when the image is memory-mapped
 * and the true extent is unknown; the header's total_len is then trusted for
 * bounds. Set verify_crc = false to skip the CRC pass, which costs a full
 * read of the image over the OSPI bus.
 *
 * On success `out` points into `base`; nothing is copied. */
romimg_err_t romimg_parse(const uint8_t *base, size_t len, bool verify_crc,
                          romimg_t *out);

/* Check that the regions match what the pacman machine layer expects. */
romimg_err_t romimg_check_pacman(const romimg_t *img);

const char *romimg_strerror(romimg_err_t e);

#endif /* ROMIMAGE_H_ */
