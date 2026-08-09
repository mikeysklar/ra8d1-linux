#include "romimage.h"
#include "pacman.h"

#include <string.h>

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
	       ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

/* CRC-32/IEEE, the same polynomial zlib uses, computed without a table.
 * The image is at most a few hundred KB and this runs once at boot, so the
 * bit-at-a-time loop is not worth a 1 KB table. */
static uint32_t crc32_ieee(const uint8_t *p, uint32_t n)
{
	uint32_t crc = 0xffffffffu;

	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++) {
			uint32_t mask = (uint32_t) -(int32_t) (crc & 1u);

			crc = (crc >> 1) ^ (0xedb88320u & mask);
		}
	}
	return ~crc;
}

romimg_err_t romimg_parse(const uint8_t *base, size_t len, bool verify_crc,
                          romimg_t *out)
{
	if (!base || !out) {
		return ROMIMG_BAD_HEADER;
	}

	if (len && len < ROMIMG_HDR_SIZE) {
		return ROMIMG_BAD_HEADER;
	}

	if (memcmp(base, ROMIMG_MAGIC, 8) != 0) {
		return ROMIMG_BAD_MAGIC;
	}

	uint32_t machine  = rd32(base + 8);
	uint32_t hdr_size = rd32(base + 12);
	uint32_t total    = rd32(base + 48);

	if (hdr_size != ROMIMG_HDR_SIZE || total < hdr_size) {
		return ROMIMG_BAD_HEADER;
	}
	if (len && total > len) {
		return ROMIMG_TRUNCATED;
	}

	memset(out, 0, sizeof(*out));
	out->machine   = machine;
	out->total_len = total;
	out->crc32     = rd32(base + 52);
	out->flags     = rd32(base + 56);

	const uint8_t **ptrs[4] = { &out->cpu, &out->gfx, &out->prom, &out->snd };
	uint32_t *lens[4] = { &out->cpu_len, &out->gfx_len, &out->prom_len,
	                      &out->snd_len };

	for (unsigned i = 0; i < 4; i++) {
		uint32_t off = rd32(base + 16 + i * 8);
		uint32_t rl  = rd32(base + 20 + i * 8);

		if (rl == 0) {
			*ptrs[i] = NULL;
			*lens[i] = 0;
			continue;
		}
		if (off < hdr_size || off > total || rl > total - off) {
			return ROMIMG_TRUNCATED;
		}
		*ptrs[i] = base + off;
		*lens[i] = rl;
	}

	if (verify_crc) {
		uint32_t calc = crc32_ieee(base + hdr_size, total - hdr_size);

		if (calc != out->crc32) {
			return ROMIMG_BAD_CRC;
		}
	}

	return ROMIMG_OK;
}

romimg_err_t romimg_check_pacman(const romimg_t *img)
{
	if (img->machine != ROMIMG_MACHINE_PACMAN) {
		return ROMIMG_BAD_MACHINE;
	}
	if (img->cpu_len != PACMAN_ROM_SIZE ||
	    img->gfx_len != PACMAN_GFX_SIZE ||
	    img->prom_len != PACMAN_PROM_SIZE) {
		return ROMIMG_BAD_REGION;
	}
	return ROMIMG_OK;
}

const char *romimg_strerror(romimg_err_t e)
{
	switch (e) {
	case ROMIMG_OK:          return "ok";
	case ROMIMG_BAD_MAGIC:   return "bad magic (not a RA8ARC01 image)";
	case ROMIMG_BAD_HEADER:  return "bad header";
	case ROMIMG_TRUNCATED:   return "region runs past end of image";
	case ROMIMG_BAD_CRC:     return "crc mismatch";
	case ROMIMG_BAD_MACHINE: return "unknown machine id";
	case ROMIMG_BAD_REGION:  return "region size wrong for machine";
	default:                 return "unknown error";
	}
}
