/*
 * image.h - the on-flash image container, shared by the reader and the writer.
 *
 * Not ours. This is the layout the rvlinux app on this board already writes
 * and its TCP pusher already tests end to end: a 16-byte header at the slot
 * base and the payload from +4096. Keeping it byte-for-byte means an image
 * pushed with the working tool boots here unchanged.
 *
 * It lives in its own header because two files now depend on every byte of it
 * agreeing: src/main.c validates a slot at boot and src/pusher.c writes one.
 * They were duplicating the constants, which is a drift waiting to happen -
 * and the thing that would drift is the header the host tool parses.
 */
#ifndef RVT_IMAGE_H_
#define RVT_IMAGE_H_

#include <stdint.h>

#include "rv_platform.h"

#define IMG_HDR_SIZE    16U
#define IMG_PAYLOAD_OFF 4096U   /* the header gets its own aligned block */

struct img_hdr {
	char     magic[8];
	uint32_t len;
	uint32_t crc;
};

/*
 * The magic is per slot and is not redundant with the offset: it is what
 * catches a rootfs pushed into the kernel slot, which is a mistake worth eight
 * bytes of comparison to prevent.
 */
static inline const char *img_slot_magic(int slot)
{
	switch (slot) {
	case PLAT_SLOT_KERNEL:
		return "RA8LINUX";
	case PLAT_SLOT_ROOTFS:
		return "RA8ROOTF";
	default:
		return NULL;
	}
}

/*
 * Slot names as the host tool spells them. pushimage.py looks its --slot
 * argument up in the banner this produces, so these two strings are protocol,
 * not cosmetics.
 */
static inline const char *img_slot_name(int slot)
{
	switch (slot) {
	case PLAT_SLOT_KERNEL:
		return "kernel";
	case PLAT_SLOT_ROOTFS:
		return "rootfs";
	default:
		return "?";
	}
}

#endif /* RVT_IMAGE_H_ */
