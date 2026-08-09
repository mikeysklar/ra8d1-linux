/*
 * rv_virtio.h - a read-only virtio-mmio block device.
 *
 * Written rather than vendored from TinyEMU's virtio.c, which was the other
 * option. The reasoning, since it goes against the obvious choice:
 *
 *   - virtio.h pulls <sys/select.h>, pci.h and fs.h. The first does not exist
 *     on bare metal and the other two drag in the PCI transport and the whole
 *     9p filesystem client, none of which this machine has or wants.
 *   - We need about 15% of its 2,650 lines: one transport, one device, one
 *     direction.
 *   - Both of the problems found in that code are structural here rather than
 *     patched. `virtio_block_init` (virtio.c:1117) never sets device_features,
 *     so the guest sees a writable disk; this device offers VIRTIO_BLK_F_RO
 *     and has no write path to forget to disable. The per-request malloc
 *     bounce buffer (virtio.c:1084) does not exist, because the backing store
 *     is already a readable host pointer - the memory-mapped OSPI window - so
 *     data goes straight from flash into the guest's descriptor buffers with
 *     no intermediate copy at all. There is no allocation to make static.
 *
 * Scope, stated so the limits are not discovered later: modern virtio-mmio
 * only (version 2, VIRTIO_F_VERSION_1), one virtqueue, split rings, no
 * indirect descriptors, no event index, no write path. Every one of those is
 * a feature we simply do not offer, so a conforming driver never uses it.
 */
#ifndef RV_VIRTIO_H_
#define RV_VIRTIO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "iomem.h"

/*
 * Attach a read-only block device at `base`, backed by `size` bytes readable
 * at `data`. The pointer must stay valid for the life of the machine; on the
 * board it points into the OSPI window and is never copied, which is the
 * entire point - a 55 MB rootfs costs no SDRAM.
 *
 * `size` is truncated down to a whole 512-byte sector. Returns 0, or -EINVAL
 * for a backing store smaller than one sector.
 */
int rv_virtio_blk_init(PhysMemoryMap *mem_map, uint64_t base, int irq,
		       const uint8_t *data, uint64_t size);

/* Sectors the device reports, or 0 if it was never initialised. Used by the
 * banner and by the tests. */
uint64_t rv_virtio_blk_sectors(void);

/* Requests completed, for the console statistics line. */
uint32_t rv_virtio_blk_requests(void);

/* ------------------------------------------------------------------ net
 *
 * Sibling network device in rv_virtio_net.c, same idiom and the same
 * deliberate limits (modern MMIO, split rings, no indirect descriptors) plus
 * its own: two queues, no offloads, VIRTIO_NET_F_MAC as the only feature.
 * Backend is plat_net_send()/plat_net_recv() in rv_platform.h.
 */

/* Attach the network device. `mac` is the address the guest will see in
 * config space and source frames from; the platform chose it, so the
 * platform can filter for it. */
int rv_virtio_net_init(PhysMemoryMap *mem_map, uint64_t base, int irq,
		       const uint8_t mac[6]);

/* Pull received frames from the platform into the guest's RX ring. Called
 * once per emulator slice, like uart_poll_input(); bounded per call. Safe to
 * call when the device was never initialised. */
void rv_virtio_net_poll(void);

/* TX frame count; RX delivered and RX dropped through the out-params. */
uint32_t rv_virtio_net_stats(uint32_t *rx, uint32_t *dropped);

#endif /* RV_VIRTIO_H_ */
