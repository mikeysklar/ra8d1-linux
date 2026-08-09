/*
 * rv_virtio.c - read-only virtio-mmio block device.
 *
 * See rv_virtio.h for why this is written rather than vendored, and for the
 * deliberate limits. Register layout is the virtio 1.x MMIO transport; the
 * device is virtio-blk with VIRTIO_BLK_F_RO.
 *
 * Everything the guest hands us - descriptor table, available ring, used ring,
 * and every data buffer - is addressed by guest *physical* address and reached
 * through rv_guest_phys_ptr(), which bounds-checks against RAM. A descriptor
 * pointing outside RAM fails the request rather than the host.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "cutils.h"   /* BOOL/TRUE, which iomem.h uses but does not include */
#include "iomem.h"
#include "rv_machine.h"
#include "rv_virtio.h"

/* ------------------------------------------------------- MMIO registers */

#define VIRTIO_MMIO_MAGIC          0x000
#define VIRTIO_MMIO_VERSION        0x004
#define VIRTIO_MMIO_DEVICE_ID      0x008
#define VIRTIO_MMIO_VENDOR_ID      0x00c
#define VIRTIO_MMIO_DEV_FEAT       0x010
#define VIRTIO_MMIO_DEV_FEAT_SEL   0x014
#define VIRTIO_MMIO_DRV_FEAT       0x020
#define VIRTIO_MMIO_DRV_FEAT_SEL   0x024
#define VIRTIO_MMIO_QUEUE_SEL      0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX  0x034
#define VIRTIO_MMIO_QUEUE_NUM      0x038
#define VIRTIO_MMIO_QUEUE_READY    0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY   0x050
#define VIRTIO_MMIO_INT_STATUS     0x060
#define VIRTIO_MMIO_INT_ACK        0x064
#define VIRTIO_MMIO_STATUS         0x070
#define VIRTIO_MMIO_QUEUE_DESC_LO  0x080
#define VIRTIO_MMIO_QUEUE_DESC_HI  0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LO 0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HI 0x094
#define VIRTIO_MMIO_QUEUE_USED_LO  0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HI  0x0a4
#define VIRTIO_MMIO_CONFIG_GEN     0x0fc
#define VIRTIO_MMIO_CONFIG         0x100

#define VIRTIO_MMIO_MAGIC_VALUE 0x74726976u  /* "virt" */
#define VIRTIO_MMIO_VERSION_2   2u           /* modern; 1 would be legacy */
#define VIRTIO_ID_BLOCK         2u
#define VIRTIO_VENDOR_ID        0x52413844u  /* 'RA8D', same as the SBI impl */

/* Feature bits. Offering the smallest set that works is the whole strategy:
 * a feature not offered is a code path a conforming driver never takes. */
#define VIRTIO_BLK_F_RO        (1u << 5)   /* disk is read-only */
#define VIRTIO_F_VERSION_1_HI  (1u << 0)   /* bit 32, i.e. bit 0 of word 1 */

/* Deliberately NOT offered: VIRTIO_RING_F_INDIRECT_DESC (no indirect
 * descriptor tables to walk), VIRTIO_RING_F_EVENT_IDX (no used_event
 * suppression), VIRTIO_BLK_F_SEG_MAX / F_BLK_SIZE / F_MQ / F_DISCARD. */

#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEAT_OK   8

/* ------------------------------------------------------- split virtqueue */

#define VRING_DESC_F_NEXT     1
#define VRING_DESC_F_WRITE    2
#define VRING_DESC_F_INDIRECT 4

#define VRING_AVAIL_F_NO_INTERRUPT 1

#define VIRTIO_QUEUE_NUM_MAX 128

/* Chain length ceiling. A virtio-blk read is a header, some data segments and
 * a status byte; Linux caps its own segments well under this. The bound also
 * makes a corrupt or malicious ring a failed request rather than a hang. */
#define VIRTIO_MAX_CHAIN 260

/* ------------------------------------------------------- block requests */

#define VIRTIO_BLK_T_IN     0
#define VIRTIO_BLK_T_OUT    1
#define VIRTIO_BLK_T_FLUSH  4

#define VIRTIO_BLK_S_OK     0
#define VIRTIO_BLK_S_IOERR  1
#define VIRTIO_BLK_S_UNSUPP 2

#define SECTOR_SIZE 512

typedef struct {
	uint32_t type;
	uint32_t ioprio;
	uint64_t sector;
} BlkReqHeader;

/* ------------------------------------------------------------ the device */

typedef struct {
	const uint8_t *data;
	uint64_t nb_sectors;
	int irq;

	uint32_t status;
	uint32_t dev_feat_sel;
	uint32_t drv_feat_sel;
	uint32_t drv_feat[2];
	uint32_t int_status;

	uint32_t queue_num;
	uint32_t queue_ready;
	uint64_t desc_addr;
	uint64_t avail_addr;
	uint64_t used_addr;
	uint16_t last_avail_idx;

	uint32_t n_requests;
	bool inited;
} RVVirtioBlk;

static RVVirtioBlk vblk;

/* --------------------------------------------- guest memory accessors */

/*
 * All of these return false rather than trapping when the guest points at
 * something that is not RAM. A driver bug or a corrupt ring then shows up as
 * VIRTIO_BLK_S_IOERR, which the guest reports as an I/O error, instead of as
 * a wild host pointer.
 */
static bool gread(uint64_t addr, void *dst, size_t len)
{
	const uint8_t *p = rv_guest_phys_ptr(addr, len);

	if (p == NULL) {
		return false;
	}
	memcpy(dst, p, len);
	return true;
}

static bool gwrite(uint64_t addr, const void *src, size_t len)
{
	uint8_t *p = rv_guest_phys_ptr(addr, len);

	if (p == NULL) {
		return false;
	}
	memcpy(p, src, len);
	return true;
}

static bool gread16(uint64_t addr, uint16_t *v)
{
	return gread(addr, v, sizeof(*v));
}

/* ------------------------------------------------------------ descriptors */

typedef struct {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
} VringDesc;

static bool desc_read(uint16_t idx, VringDesc *d)
{
	uint8_t raw[16];

	if (idx >= vblk.queue_num) {
		return false;
	}
	if (!gread(vblk.desc_addr + (uint64_t)idx * 16, raw, sizeof(raw))) {
		return false;
	}
	memcpy(&d->addr, raw + 0, 8);
	memcpy(&d->len, raw + 8, 4);
	memcpy(&d->flags, raw + 12, 2);
	memcpy(&d->next, raw + 14, 2);
	return true;
}

static void blk_set_irq(bool level)
{
	rv_plic_set_irq(vblk.irq, level);
}

/* Publish a completed descriptor chain and interrupt unless the driver asked
 * us not to. */
static void queue_complete(uint16_t head, uint32_t written)
{
	uint16_t used_idx;
	uint16_t avail_flags;
	uint8_t elem[8];
	uint32_t id = head;

	if (!gread16(vblk.used_addr + 2, &used_idx)) {
		return;
	}
	memcpy(elem + 0, &id, 4);
	memcpy(elem + 4, &written, 4);
	/* used->ring[] starts at offset 4 and each element is 8 bytes. */
	if (!gwrite(vblk.used_addr + 4 +
			    (uint64_t)(used_idx % vblk.queue_num) * 8,
		    elem, sizeof(elem))) {
		return;
	}
	used_idx++;
	if (!gwrite(vblk.used_addr + 2, &used_idx, 2)) {
		return;
	}

	if (gread16(vblk.avail_addr, &avail_flags) &&
	    (avail_flags & VRING_AVAIL_F_NO_INTERRUPT)) {
		return;
	}
	vblk.int_status |= 1;  /* used buffer notification */
	blk_set_irq(true);
}

/*
 * Service one descriptor chain.
 *
 * The chain for a virtio-blk request is: one or more device-readable
 * descriptors holding the 16-byte header, then (for a read) device-writable
 * descriptors for the data, then one device-writable byte for the status.
 * We walk it once, because the status byte is by definition the last writable
 * one and we cannot know which that is until the end.
 */
static void handle_chain(uint16_t head)
{
	VringDesc chain[VIRTIO_MAX_CHAIN];
	int n = 0;
	uint16_t idx = head;
	BlkReqHeader h;
	uint8_t hdr[16];
	uint32_t hdr_got = 0;
	uint64_t offset;
	uint32_t written = 0;
	uint8_t status = VIRTIO_BLK_S_OK;
	int i;
	int status_desc = -1;

	/* Collect the chain first; bounded, and a cycle in `next` terminates. */
	for (;;) {
		if (n >= VIRTIO_MAX_CHAIN || !desc_read(idx, &chain[n])) {
			queue_complete(head, 0);
			return;
		}
		/* We do not offer VIRTIO_RING_F_INDIRECT_DESC, so a driver
		 * setting this bit is out of spec. Fail rather than guess. */
		if (chain[n].flags & VRING_DESC_F_INDIRECT) {
			queue_complete(head, 0);
			return;
		}
		if (!(chain[n].flags & VRING_DESC_F_NEXT)) {
			n++;
			break;
		}
		idx = chain[n].next;
		n++;
	}

	/* The header comes from the leading device-readable descriptors. */
	for (i = 0; i < n && hdr_got < sizeof(hdr); i++) {
		uint32_t take;

		if (chain[i].flags & VRING_DESC_F_WRITE) {
			break;
		}
		take = chain[i].len;
		if (take > sizeof(hdr) - hdr_got) {
			take = (uint32_t)sizeof(hdr) - hdr_got;
		}
		if (!gread(chain[i].addr, hdr + hdr_got, take)) {
			queue_complete(head, 0);
			return;
		}
		hdr_got += take;
	}
	if (hdr_got < sizeof(hdr)) {
		queue_complete(head, 0);
		return;
	}
	memcpy(&h.type, hdr + 0, 4);
	memcpy(&h.ioprio, hdr + 4, 4);
	memcpy(&h.sector, hdr + 8, 8);

	/* The last device-writable descriptor is the status byte. */
	for (i = n - 1; i >= 0; i--) {
		if (chain[i].flags & VRING_DESC_F_WRITE) {
			status_desc = i;
			break;
		}
	}
	if (status_desc < 0 || chain[status_desc].len < 1) {
		queue_complete(head, 0);
		return;
	}

	switch (h.type) {
	case VIRTIO_BLK_T_IN:
		offset = h.sector * SECTOR_SIZE;
		/* Every writable descriptor before the status one is data. */
		for (i = 0; i < status_desc; i++) {
			uint32_t len;

			if (!(chain[i].flags & VRING_DESC_F_WRITE)) {
				continue;
			}
			len = chain[i].len;
			/*
			 * Straight from the backing store into guest RAM. No
			 * bounce buffer: `data` is already a readable host
			 * pointer, which on the board is the memory-mapped
			 * OSPI window.
			 */
			if (offset + len > vblk.nb_sectors * SECTOR_SIZE ||
			    offset + len < offset) {
				status = VIRTIO_BLK_S_IOERR;
				break;
			}
			if (!gwrite(chain[i].addr, vblk.data + offset, len)) {
				status = VIRTIO_BLK_S_IOERR;
				break;
			}
			offset += len;
			written += len;
		}
		break;

	case VIRTIO_BLK_T_FLUSH:
		/* Nothing is cached and nothing is writable; trivially done. */
		break;

	case VIRTIO_BLK_T_OUT:
		/* Unreachable through a conforming driver: VIRTIO_BLK_F_RO is
		 * offered, so the guest mounts read-only and never issues one.
		 * Answered honestly anyway rather than silently dropped. */
		status = VIRTIO_BLK_S_IOERR;
		break;

	default:
		status = VIRTIO_BLK_S_UNSUPP;
		break;
	}

	if (status != VIRTIO_BLK_S_OK) {
		written = 0;
	}
	if (!gwrite(chain[status_desc].addr, &status, 1)) {
		queue_complete(head, 0);
		return;
	}
	vblk.n_requests++;
	queue_complete(head, written + 1);
}

/* Drain everything the driver has made available since we last looked. */
static void queue_notify(void)
{
	uint16_t avail_idx;
	uint16_t head;

	if (!vblk.queue_ready || vblk.queue_num == 0) {
		return;
	}
	if (!gread16(vblk.avail_addr + 2, &avail_idx)) {
		return;
	}
	while (vblk.last_avail_idx != avail_idx) {
		uint64_t slot = vblk.avail_addr + 4 +
				(uint64_t)(vblk.last_avail_idx %
					   vblk.queue_num) * 2;

		if (!gread16(slot, &head)) {
			return;
		}
		vblk.last_avail_idx++;
		handle_chain(head);
	}
}

/* ------------------------------------------------------------ MMIO glue */

static uint32_t virtio_read(void *opaque, uint32_t offset, int size_log2)
{
	(void)opaque;

	if (size_log2 != 2) {
		return 0;
	}

	if (offset >= VIRTIO_MMIO_CONFIG) {
		/* virtio-blk config space: capacity, in 512-byte sectors. */
		uint32_t off = offset - VIRTIO_MMIO_CONFIG;

		if (off == 0) {
			return (uint32_t)vblk.nb_sectors;
		}
		if (off == 4) {
			return (uint32_t)(vblk.nb_sectors >> 32);
		}
		return 0;
	}

	switch (offset) {
	case VIRTIO_MMIO_MAGIC:
		return VIRTIO_MMIO_MAGIC_VALUE;
	case VIRTIO_MMIO_VERSION:
		return VIRTIO_MMIO_VERSION_2;
	case VIRTIO_MMIO_DEVICE_ID:
		return VIRTIO_ID_BLOCK;
	case VIRTIO_MMIO_VENDOR_ID:
		return VIRTIO_VENDOR_ID;
	case VIRTIO_MMIO_DEV_FEAT:
		/* Word 0 is bits 0-31, word 1 is bits 32-63. VERSION_1 is bit
		 * 32 and is what makes this a modern device. */
		if (vblk.dev_feat_sel == 0) {
			return VIRTIO_BLK_F_RO;
		}
		if (vblk.dev_feat_sel == 1) {
			return VIRTIO_F_VERSION_1_HI;
		}
		return 0;
	case VIRTIO_MMIO_QUEUE_NUM_MAX:
		return VIRTIO_QUEUE_NUM_MAX;
	case VIRTIO_MMIO_QUEUE_READY:
		return vblk.queue_ready;
	case VIRTIO_MMIO_INT_STATUS:
		return vblk.int_status;
	case VIRTIO_MMIO_STATUS:
		return vblk.status;
	case VIRTIO_MMIO_CONFIG_GEN:
		/* The capacity never changes, so the generation never does. */
		return 0;
	default:
		return 0;
	}
}

static void virtio_write(void *opaque, uint32_t offset, uint32_t val,
			 int size_log2)
{
	(void)opaque;

	if (size_log2 != 2) {
		return;
	}
	if (offset >= VIRTIO_MMIO_CONFIG) {
		/* Config space is read-only on this device. */
		return;
	}

	switch (offset) {
	case VIRTIO_MMIO_DEV_FEAT_SEL:
		vblk.dev_feat_sel = val;
		break;
	case VIRTIO_MMIO_DRV_FEAT_SEL:
		vblk.drv_feat_sel = val;
		break;
	case VIRTIO_MMIO_DRV_FEAT:
		if (vblk.drv_feat_sel < 2) {
			vblk.drv_feat[vblk.drv_feat_sel] = val;
		}
		break;
	case VIRTIO_MMIO_QUEUE_SEL:
		/* One queue. A driver selecting any other sees num_max as it
		 * is, but this device has nothing else to configure. */
		break;
	case VIRTIO_MMIO_QUEUE_NUM:
		if (val > 0 && val <= VIRTIO_QUEUE_NUM_MAX) {
			vblk.queue_num = val;
		}
		break;
	case VIRTIO_MMIO_QUEUE_READY:
		vblk.queue_ready = val & 1;
		if (vblk.queue_ready) {
			vblk.last_avail_idx = 0;
		}
		break;
	case VIRTIO_MMIO_QUEUE_DESC_LO:
		vblk.desc_addr = (vblk.desc_addr & ~0xffffffffull) | val;
		break;
	case VIRTIO_MMIO_QUEUE_DESC_HI:
		vblk.desc_addr = (vblk.desc_addr & 0xffffffffull) |
				 ((uint64_t)val << 32);
		break;
	case VIRTIO_MMIO_QUEUE_AVAIL_LO:
		vblk.avail_addr = (vblk.avail_addr & ~0xffffffffull) | val;
		break;
	case VIRTIO_MMIO_QUEUE_AVAIL_HI:
		vblk.avail_addr = (vblk.avail_addr & 0xffffffffull) |
				  ((uint64_t)val << 32);
		break;
	case VIRTIO_MMIO_QUEUE_USED_LO:
		vblk.used_addr = (vblk.used_addr & ~0xffffffffull) | val;
		break;
	case VIRTIO_MMIO_QUEUE_USED_HI:
		vblk.used_addr = (vblk.used_addr & 0xffffffffull) |
				 ((uint64_t)val << 32);
		break;
	case VIRTIO_MMIO_QUEUE_NOTIFY:
		queue_notify();
		break;
	case VIRTIO_MMIO_INT_ACK:
		vblk.int_status &= ~val;
		if (vblk.int_status == 0) {
			blk_set_irq(false);
		}
		break;
	case VIRTIO_MMIO_STATUS:
		vblk.status = val;
		if (val == 0) {
			/* Reset. */
			vblk.queue_ready = 0;
			vblk.queue_num = 0;
			vblk.last_avail_idx = 0;
			vblk.int_status = 0;
			blk_set_irq(false);
		}
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- setup */

int rv_virtio_blk_init(PhysMemoryMap *mem_map, uint64_t base, int irq,
		       const uint8_t *data, uint64_t size)
{
	if (data == NULL || size < SECTOR_SIZE) {
		return -EINVAL;
	}

	memset(&vblk, 0, sizeof(vblk));
	vblk.data = data;
	vblk.nb_sectors = size / SECTOR_SIZE;
	vblk.irq = irq;
	vblk.inited = true;

	if (cpu_register_device(mem_map, base, 0x1000, &vblk,
				virtio_read, virtio_write,
				DEVIO_SIZE32) == NULL) {
		return -ENOMEM;
	}
	return 0;
}

uint64_t rv_virtio_blk_sectors(void)
{
	return vblk.inited ? vblk.nb_sectors : 0;
}

uint32_t rv_virtio_blk_requests(void)
{
	return vblk.n_requests;
}
