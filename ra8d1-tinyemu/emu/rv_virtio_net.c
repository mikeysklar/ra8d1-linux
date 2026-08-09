/*
 * rv_virtio_net.c - virtio-mmio network device.
 *
 * A sibling of rv_virtio.c's block device, written in the same idiom for the
 * same reasons (see rv_virtio.h), rather than a refactor of it: the two
 * devices share the ring-walk shape but not state, and 100 duplicated lines
 * beat a helper layer both would have to be read through.
 *
 * Scope: modern virtio-mmio only, two split virtqueues (0 = RX, 1 = TX), no
 * indirect descriptors, no event index, no offloads of any kind. The only
 * feature offered beyond VERSION_1 is VIRTIO_NET_F_MAC, so the guest takes
 * the MAC this device reports instead of inventing one - the Zephyr side
 * filters promiscuous RX traffic by exactly this address, so the two ends
 * agreeing on it is load-bearing, not cosmetic.
 *
 * The backend is two platform hooks, keeping emu/ portable:
 *   plat_net_send(frame, len)          guest -> wire
 *   plat_net_recv(buf, cap) -> len|-1  wire -> guest, non-blocking
 * With networking off both compile to stubs and this device is simply never
 * initialised (no FDT node either, so the guest never probes it).
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "cutils.h"
#include "iomem.h"
#include "rv_machine.h"
#include "rv_platform.h"
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

#define VIRTIO_MMIO_MAGIC_VALUE 0x74726976u
#define VIRTIO_MMIO_VERSION_2   2u
#define VIRTIO_ID_NET           1u
#define VIRTIO_VENDOR_ID        0x52413844u  /* 'RA8D' */

#define VIRTIO_NET_F_MAC       (1u << 5)
#define VIRTIO_F_VERSION_1_HI  (1u << 0)   /* bit 32 */

#define VIRTIO_STATUS_DRIVER_OK 4

/* ------------------------------------------------------- split virtqueue */

#define VRING_DESC_F_NEXT     1
#define VRING_DESC_F_WRITE    2
#define VRING_DESC_F_INDIRECT 4

#define VRING_AVAIL_F_NO_INTERRUPT 1

#define VIRTIO_QUEUE_NUM_MAX 128

/* An ethernet frame is at most 1514 bytes here (no VLAN offload, no jumbo),
 * plus the 12-byte header; a TX chain is the header plus a few segments.
 * Linux's virtio-net uses 2 + MAX_SKB_FRAGS; 20 is comfortable, and the
 * bound turns ring corruption into a dropped frame rather than a hang. */
#define VIRTIO_NET_MAX_CHAIN 20

#define VIRTIO_NET_HDR_LEN 12   /* modern device: num_buffers always present */
#define NET_FRAME_MAX      1514

#define QUEUE_RX 0
#define QUEUE_TX 1

/* ------------------------------------------------------------ the device */

typedef struct {
	uint32_t num;
	uint32_t ready;
	uint64_t desc_addr;
	uint64_t avail_addr;
	uint64_t used_addr;
	uint16_t last_avail_idx;
} VQ;

typedef struct {
	int irq;
	uint8_t mac[6];

	uint32_t status;
	uint32_t dev_feat_sel;
	uint32_t drv_feat_sel;
	uint32_t drv_feat[2];
	uint32_t int_status;
	uint32_t queue_sel;

	VQ q[2];

	uint32_t tx_frames, rx_frames, rx_dropped;
	bool inited;
} RVVirtioNet;

static RVVirtioNet vnet;

/* --------------------------------------------- guest memory accessors */

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

static bool desc_read(const VQ *q, uint16_t idx, VringDesc *d)
{
	uint8_t raw[16];

	if (idx >= q->num) {
		return false;
	}
	if (!gread(q->desc_addr + (uint64_t)idx * 16, raw, sizeof(raw))) {
		return false;
	}
	memcpy(&d->addr, raw + 0, 8);
	memcpy(&d->len, raw + 8, 4);
	memcpy(&d->flags, raw + 12, 2);
	memcpy(&d->next, raw + 14, 2);
	return true;
}

static void net_set_irq(bool level)
{
	rv_plic_set_irq(vnet.irq, level);
}

static void queue_complete(VQ *q, uint16_t head, uint32_t written)
{
	uint16_t used_idx;
	uint16_t avail_flags;
	uint8_t elem[8];
	uint32_t id = head;

	if (!gread16(q->used_addr + 2, &used_idx)) {
		return;
	}
	memcpy(elem + 0, &id, 4);
	memcpy(elem + 4, &written, 4);
	if (!gwrite(q->used_addr + 4 +
			    (uint64_t)(used_idx % q->num) * 8,
		    elem, sizeof(elem))) {
		return;
	}
	used_idx++;
	if (!gwrite(q->used_addr + 2, &used_idx, 2)) {
		return;
	}

	if (gread16(q->avail_addr, &avail_flags) &&
	    (avail_flags & VRING_AVAIL_F_NO_INTERRUPT)) {
		return;
	}
	vnet.int_status |= 1;
	net_set_irq(true);
}

/* Collect a descriptor chain. Returns count, or -1 on a bad ring. */
static int chain_collect(const VQ *q, uint16_t head, VringDesc *chain)
{
	int n = 0;
	uint16_t idx = head;

	for (;;) {
		if (n >= VIRTIO_NET_MAX_CHAIN || !desc_read(q, idx, &chain[n])) {
			return -1;
		}
		if (chain[n].flags & VRING_DESC_F_INDIRECT) {
			return -1;
		}
		if (!(chain[n].flags & VRING_DESC_F_NEXT)) {
			return n + 1;
		}
		idx = chain[n].next;
		n++;
	}
}

/* ------------------------------------------------------------------- TX */

/*
 * One TX chain: a 12-byte virtio_net_hdr (ignored - no offloads were
 * offered, so a conforming driver sends it zeroed) followed by the frame in
 * one or more device-readable descriptors. Reassemble and hand to the wire.
 */
static void tx_chain(VQ *q, uint16_t head)
{
	VringDesc chain[VIRTIO_NET_MAX_CHAIN];
	uint8_t frame[NET_FRAME_MAX];
	uint32_t skip = VIRTIO_NET_HDR_LEN;
	uint32_t flen = 0;
	int n, i;

	n = chain_collect(q, head, chain);
	if (n < 0) {
		queue_complete(q, head, 0);
		return;
	}

	for (i = 0; i < n; i++) {
		uint64_t addr = chain[i].addr;
		uint32_t len = chain[i].len;
		uint32_t take;

		if (chain[i].flags & VRING_DESC_F_WRITE) {
			continue;  /* not ours to read */
		}
		if (skip >= len) {
			skip -= len;
			continue;
		}
		addr += skip;
		len -= skip;
		skip = 0;

		take = len;
		if (flen + take > sizeof(frame)) {
			take = (uint32_t)sizeof(frame) - flen;
		}
		if (take > 0 && !gread(addr, frame + flen, take)) {
			queue_complete(q, head, 0);
			return;
		}
		flen += take;
	}

	if (flen >= 14) {  /* smaller than an ethernet header is not a frame */
		plat_net_send(frame, flen);
		vnet.tx_frames++;
	}
	queue_complete(q, head, 0);
}

static void tx_notify(void)
{
	VQ *q = &vnet.q[QUEUE_TX];
	uint16_t avail_idx, head;

	if (!q->ready || q->num == 0) {
		return;
	}
	if (!gread16(q->avail_addr + 2, &avail_idx)) {
		return;
	}
	while (q->last_avail_idx != avail_idx) {
		uint64_t slot = q->avail_addr + 4 +
				(uint64_t)(q->last_avail_idx % q->num) * 2;

		if (!gread16(slot, &head)) {
			return;
		}
		q->last_avail_idx++;
		tx_chain(q, head);
	}
}

/* ------------------------------------------------------------------- RX */

/*
 * Deliver one frame into the next available RX chain: 12-byte header with
 * num_buffers = 1 (mandatory when MRG_RXBUF is not negotiated), then the
 * frame, scattered across the chain's writable descriptors.
 */
static bool rx_deliver(const uint8_t *frame, uint32_t flen)
{
	VQ *q = &vnet.q[QUEUE_RX];
	VringDesc chain[VIRTIO_NET_MAX_CHAIN];
	uint8_t hdr[VIRTIO_NET_HDR_LEN];
	uint16_t avail_idx, head;
	uint32_t written = 0;
	uint32_t src_off = 0;
	uint64_t slot;
	int n, i;

	if (!q->ready || q->num == 0) {
		return false;
	}
	if (!gread16(q->avail_addr + 2, &avail_idx)) {
		return false;
	}
	if (q->last_avail_idx == avail_idx) {
		return false;  /* no buffers; caller counts the drop */
	}

	slot = q->avail_addr + 4 + (uint64_t)(q->last_avail_idx % q->num) * 2;
	if (!gread16(slot, &head)) {
		return false;
	}

	n = chain_collect(q, head, chain);
	if (n < 0) {
		q->last_avail_idx++;
		queue_complete(q, head, 0);
		return false;
	}

	memset(hdr, 0, sizeof(hdr));
	hdr[10] = 1;  /* num_buffers, little-endian low byte */

	for (i = 0; i < n && (written < flen + VIRTIO_NET_HDR_LEN); i++) {
		uint64_t addr = chain[i].addr;
		uint32_t cap = chain[i].len;
		uint32_t off = 0;

		if (!(chain[i].flags & VRING_DESC_F_WRITE)) {
			continue;
		}
		/* Header first, then frame bytes. */
		if (written < VIRTIO_NET_HDR_LEN) {
			uint32_t take = VIRTIO_NET_HDR_LEN - written;

			if (take > cap) {
				take = cap;
			}
			if (!gwrite(addr, hdr + written, take)) {
				return false;
			}
			written += take;
			off += take;
			cap -= take;
		}
		if (cap > 0 && src_off < flen) {
			uint32_t take = flen - src_off;

			if (take > cap) {
				take = cap;
			}
			if (!gwrite(addr + off, frame + src_off, take)) {
				return false;
			}
			src_off += take;
			written += take;
		}
	}

	if (src_off < flen) {
		/* Chain too small for the frame: complete with what fit so the
		 * buffer is returned, but count it as a drop. */
		vnet.rx_dropped++;
	} else {
		vnet.rx_frames++;
	}
	q->last_avail_idx++;
	queue_complete(q, head, written);
	return true;
}

/* Pull frames from the platform into the guest. Called once per emulator
 * slice, mirroring uart_poll_input(): bounded so a flooded wire cannot
 * starve the guest of cycles. */
void rv_virtio_net_poll(void)
{
	uint8_t frame[NET_FRAME_MAX];
	int budget = 8;

	if (!vnet.inited || !(vnet.status & VIRTIO_STATUS_DRIVER_OK)) {
		return;
	}
	while (budget-- > 0) {
		int len = plat_net_recv(frame, sizeof(frame));

		if (len <= 0) {
			return;
		}
		if (!rx_deliver(frame, (uint32_t)len)) {
			vnet.rx_dropped++;
			return;  /* ring full; frame is lost, like a real NIC */
		}
	}
}

/* ------------------------------------------------------------ MMIO glue */

static uint32_t vnet_read(void *opaque, uint32_t offset, int size_log2)
{
	(void)opaque;

	if (offset >= VIRTIO_MMIO_CONFIG) {
		/* virtio-net config: 6-byte MAC. Linux reads it a byte at a
		 * time, but the spec ties access width to field width, not to
		 * driver habit - so serve 8/16/32-bit reads all coherently
		 * rather than betting on one driver's behavior. Bytes past the
		 * MAC read as zero (we do not offer VIRTIO_NET_F_STATUS, so
		 * there is no status field to misreport). */
		uint32_t off = offset - VIRTIO_MMIO_CONFIG;
		uint32_t v = 0;
		unsigned int i, n = 1u << size_log2;

		for (i = 0; i < n; i++) {
			if (off + i < 6) {
				v |= (uint32_t)vnet.mac[off + i] << (8 * i);
			}
		}
		return v;
	}
	if (size_log2 != 2) {
		return 0;
	}

	switch (offset) {
	case VIRTIO_MMIO_MAGIC:
		return VIRTIO_MMIO_MAGIC_VALUE;
	case VIRTIO_MMIO_VERSION:
		return VIRTIO_MMIO_VERSION_2;
	case VIRTIO_MMIO_DEVICE_ID:
		return VIRTIO_ID_NET;
	case VIRTIO_MMIO_VENDOR_ID:
		return VIRTIO_VENDOR_ID;
	case VIRTIO_MMIO_DEV_FEAT:
		if (vnet.dev_feat_sel == 0) {
			return VIRTIO_NET_F_MAC;
		}
		if (vnet.dev_feat_sel == 1) {
			return VIRTIO_F_VERSION_1_HI;
		}
		return 0;
	case VIRTIO_MMIO_QUEUE_NUM_MAX:
		return VIRTIO_QUEUE_NUM_MAX;
	case VIRTIO_MMIO_QUEUE_READY:
		return vnet.queue_sel < 2 ? vnet.q[vnet.queue_sel].ready : 0;
	case VIRTIO_MMIO_INT_STATUS:
		return vnet.int_status;
	case VIRTIO_MMIO_STATUS:
		return vnet.status;
	case VIRTIO_MMIO_CONFIG_GEN:
		return 0;
	default:
		return 0;
	}
}

static void vnet_write(void *opaque, uint32_t offset, uint32_t val,
		       int size_log2)
{
	VQ *q;

	(void)opaque;

	if (size_log2 != 2 || offset >= VIRTIO_MMIO_CONFIG) {
		return;
	}
	q = vnet.queue_sel < 2 ? &vnet.q[vnet.queue_sel] : NULL;

	switch (offset) {
	case VIRTIO_MMIO_DEV_FEAT_SEL:
		vnet.dev_feat_sel = val;
		break;
	case VIRTIO_MMIO_DRV_FEAT_SEL:
		vnet.drv_feat_sel = val;
		break;
	case VIRTIO_MMIO_DRV_FEAT:
		if (vnet.drv_feat_sel < 2) {
			vnet.drv_feat[vnet.drv_feat_sel] = val;
		}
		break;
	case VIRTIO_MMIO_QUEUE_SEL:
		if (val < 2) {
			vnet.queue_sel = val;
		}
		break;
	case VIRTIO_MMIO_QUEUE_NUM:
		if (q != NULL && val > 0 && val <= VIRTIO_QUEUE_NUM_MAX) {
			q->num = val;
		}
		break;
	case VIRTIO_MMIO_QUEUE_READY:
		if (q != NULL) {
			q->ready = val & 1;
			if (q->ready) {
				q->last_avail_idx = 0;
			}
		}
		break;
	case VIRTIO_MMIO_QUEUE_DESC_LO:
		if (q != NULL) {
			q->desc_addr = (q->desc_addr & ~0xffffffffull) | val;
		}
		break;
	case VIRTIO_MMIO_QUEUE_DESC_HI:
		if (q != NULL) {
			q->desc_addr = (q->desc_addr & 0xffffffffull) |
				       ((uint64_t)val << 32);
		}
		break;
	case VIRTIO_MMIO_QUEUE_AVAIL_LO:
		if (q != NULL) {
			q->avail_addr = (q->avail_addr & ~0xffffffffull) | val;
		}
		break;
	case VIRTIO_MMIO_QUEUE_AVAIL_HI:
		if (q != NULL) {
			q->avail_addr = (q->avail_addr & 0xffffffffull) |
					((uint64_t)val << 32);
		}
		break;
	case VIRTIO_MMIO_QUEUE_USED_LO:
		if (q != NULL) {
			q->used_addr = (q->used_addr & ~0xffffffffull) | val;
		}
		break;
	case VIRTIO_MMIO_QUEUE_USED_HI:
		if (q != NULL) {
			q->used_addr = (q->used_addr & 0xffffffffull) |
				       ((uint64_t)val << 32);
		}
		break;
	case VIRTIO_MMIO_QUEUE_NOTIFY:
		/* RX notifications just mean "buffers available"; the poll
		 * loop uses them next slice. TX needs service now. */
		if (val == QUEUE_TX) {
			tx_notify();
		}
		break;
	case VIRTIO_MMIO_INT_ACK:
		vnet.int_status &= ~val;
		if (vnet.int_status == 0) {
			net_set_irq(false);
		}
		break;
	case VIRTIO_MMIO_STATUS:
		vnet.status = val;
		if (val == 0) {
			memset(vnet.q, 0, sizeof(vnet.q));
			vnet.queue_sel = 0;
			vnet.int_status = 0;
			net_set_irq(false);
		}
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- setup */

int rv_virtio_net_init(PhysMemoryMap *mem_map, uint64_t base, int irq,
		       const uint8_t mac[6])
{
	memset(&vnet, 0, sizeof(vnet));
	memcpy(vnet.mac, mac, 6);
	vnet.irq = irq;
	vnet.inited = true;

	if (cpu_register_device(mem_map, base, 0x1000, &vnet,
				vnet_read, vnet_write,
				DEVIO_SIZE8 | DEVIO_SIZE16 | DEVIO_SIZE32) == NULL) {
		return -ENOMEM;
	}
	return 0;
}

uint32_t rv_virtio_net_stats(uint32_t *rx, uint32_t *dropped)
{
	if (rx != NULL) {
		*rx = vnet.rx_frames;
	}
	if (dropped != NULL) {
		*dropped = vnet.rx_dropped;
	}
	return vnet.tx_frames;
}
