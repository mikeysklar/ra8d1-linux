/*
 * netbridge.c - the guest NIC's path to the real Ethernet.
 *
 * Implements rv_platform.h's plat_net_mac()/plat_net_send()/plat_net_recv()
 * by putting eth0 into promiscuous mode and filtering the segment's traffic
 * for the guest, rather than by joining Zephyr's CONFIG_NET_ETHERNET_BRIDGE.
 * The bridge subsystem wants a second ethernet net_if for the guest side,
 * which means writing a dummy ethernet driver whose only job is shuttling
 * frames to the emulator - three layers of machinery to move a frame between
 * two buffers. This file does it with two rings and one thread, the same
 * shape as src/telnet.c, and the driver patch that promiscuous mode needed
 * (zephyr fork 8ca5eb88530) was required under either design.
 *
 * Traffic model:
 *   wire -> guest: net_promisc_mode_wait_data() delivers every frame the
 *     driver accepts; keep those addressed to the guest MAC, broadcast, or
 *     multicast, and drop the board's own unicast (the host stack already
 *     has it - a copy would loop DHCP replies meant for Zephyr into the
 *     guest). Frames land in an SPSC ring the emulator drains once per
 *     instruction slice through plat_net_recv().
 *   guest -> wire: plat_net_send() copies the frame into a net_pkt of
 *     family AF_PACKET - which ethernet L2 transmits verbatim, no header
 *     prepended - and queues it on eth0. The emulator thread never blocks:
 *     no buffer means the frame is dropped, as on a congested real wire.
 *
 * The guest's MAC is the board's with the locally-administered bit set and
 * the low byte inverted: recognisably related on a packet capture, distinct
 * on the segment, stable across boots (so DHCP leases stick), and never
 * colliding with the board's own address.
 */

#include <zephyr/kernel.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/promiscuous.h>
#include <zephyr/sys/ring_buffer.h>

#include <string.h>

#include "rv_platform.h"
#include "netbridge.h"
#include "telnet.h"   /* plat_uart_puts lives with the console split */

#define NET_FRAME_MAX 1514

/*
 * wire -> guest ring. Frames are stored length-prefixed (u16), so the ring
 * holds whole frames and a partial get can never split one. 8 KB is five
 * full-size frames or dozens of ACK-sized ones; the emulator drains every
 * ~667 us, so depth buys burst tolerance, not throughput.
 */
RING_BUF_DECLARE(nb_rx_ring, CONFIG_RVT_NET_GUEST_RX_RING);

static struct {
	uint8_t mac[6];
	bool up;
	uint32_t rx_kept, rx_filtered, rx_dropped, tx_sent, tx_dropped;
} nb;

/* ------------------------------------------------------- rv_platform hooks */

bool plat_net_mac(uint8_t mac[6])
{
	struct net_if *iface = net_if_get_default();
	struct net_linkaddr *ll;

	if (!IS_ENABLED(CONFIG_RVT_GUEST_NET) || iface == NULL) {
		return false;
	}
	ll = net_if_get_link_addr(iface);
	if (ll == NULL || ll->len != 6) {
		return false;
	}
	memcpy(mac, ll->addr, 6);
	mac[0] |= 0x02;         /* locally administered */
	mac[5] = (uint8_t)~mac[5];
	memcpy(nb.mac, mac, 6);
	return true;
}

void plat_net_send(const uint8_t *frame, uint32_t len)
{
	struct net_if *iface = net_if_get_default();
	struct net_pkt *pkt;

	if (!nb.up || iface == NULL || len < 14 || len > NET_FRAME_MAX) {
		nb.tx_dropped++;
		return;
	}

	/*
	 * NET_AF_PACKET is the whole trick: ethernet_send() transmits such a
	 * packet as-is when it carries no socket context - no L2 header
	 * prepended, no neighbor lookup (ethernet.c:736 "Raw packet, just
	 * send it"; gated on CONFIG_NET_SOCKETS_PACKET, which prj.conf sets
	 * for exactly this line). K_NO_WAIT because this is called from the
	 * emulator thread - blocking here stops the guest's clock.
	 */
	pkt = net_pkt_alloc_with_buffer(iface, len, NET_AF_PACKET, 0,
					K_NO_WAIT);
	if (pkt == NULL) {
		nb.tx_dropped++;
		return;
	}
	if (net_pkt_write(pkt, frame, len) != 0) {
		net_pkt_unref(pkt);
		nb.tx_dropped++;
		return;
	}

	/*
	 * Rewind before queueing. This is not housekeeping, it is the whole
	 * frame: net_pkt_write() leaves the cursor at the end of what it
	 * wrote, and the driver's tx does net_pkt_read() FROM THE CURSOR. The
	 * normal ethernet_send() path calls net_pkt_cursor_init() for you -
	 * but it does so on the line before the `send:` label, and the raw
	 * AF_PACKET path arrives by `goto send`, jumping over it
	 * (ethernet.c:735-742 vs 769-772). So a raw sender has to do it
	 * itself.
	 *
	 * Cost of omitting it, measured on hardware: every frame queues and
	 * transmits nothing. The guest's own tx_packets counter still climbs -
	 * it counts what it handed the virtio device - so the guest looks
	 * healthy while the wire stays silent, and the Mac's ARP entry for it
	 * sits at "(incomplete)" forever.
	 */
	net_pkt_cursor_init(pkt);

	/* Returns void; a full TX queue surfaces as the driver's own
	 * backpressure (the cherry-picked K_MSEC wait), not here. */
	net_if_queue_tx(iface, pkt);
	nb.tx_sent++;
}

int plat_net_recv(uint8_t *buf, uint32_t cap)
{
	uint16_t len;

	if (ring_buf_peek(&nb_rx_ring, (uint8_t *)&len, 2) < 2) {
		return -1;
	}
	if (len > cap) {
		/* Too big for the caller: discard the frame to keep the ring
		 * parseable. Cannot happen while both sides use 1514. */
		ring_buf_get(&nb_rx_ring, NULL, 2u + len);
		nb.rx_dropped++;
		return -1;
	}
	ring_buf_get(&nb_rx_ring, (uint8_t *)&len, 2);
	ring_buf_get(&nb_rx_ring, buf, len);
	return (int)len;
}

/* ------------------------------------------------------------ RX thread */

static bool frame_is_for_guest(const uint8_t *dst)
{
	static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

	if (memcmp(dst, nb.mac, 6) == 0) {
		return true;
	}
	if (memcmp(dst, bcast, 6) == 0) {
		return true;
	}
	/* Multicast: group bit set. The guest's own filter narrows further. */
	return (dst[0] & 0x01) != 0;
}

static void nb_rx_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	for (;;) {
		struct net_pkt *pkt =
			net_promisc_mode_wait_data(K_FOREVER);
		uint8_t hdr[14];
		size_t flen;

		if (pkt == NULL) {
			continue;
		}
		flen = net_pkt_get_len(pkt);

		net_pkt_cursor_init(pkt);
		if (flen < 14 || flen > NET_FRAME_MAX ||
		    net_pkt_read(pkt, hdr, 14) != 0 ||
		    !frame_is_for_guest(hdr)) {
			nb.rx_filtered++;
			net_pkt_unref(pkt);
			continue;
		}

		/* Whole frame or nothing: length prefix plus payload must fit
		 * the ring in one piece. */
		if (ring_buf_space_get(&nb_rx_ring) < 2u + flen) {
			nb.rx_dropped++;
			net_pkt_unref(pkt);
			continue;
		}
		{
			uint16_t len16 = (uint16_t)flen;
			uint8_t chunk[64];
			size_t off = 0;

			ring_buf_put(&nb_rx_ring, (uint8_t *)&len16, 2);
			net_pkt_cursor_init(pkt);
			while (off < flen) {
				size_t take = MIN(sizeof(chunk), flen - off);

				if (net_pkt_read(pkt, chunk, take) != 0) {
					break;
				}
				ring_buf_put(&nb_rx_ring, chunk, take);
				off += take;
			}
		}
		nb.rx_kept++;
		net_pkt_unref(pkt);
	}
}

K_THREAD_STACK_DEFINE(nb_stack, CONFIG_RVT_NET_GUEST_THREAD_STACK);
static struct k_thread nb_tcb;

/* ----------------------------------------------------------------- start */

void rvt_netbridge_start(void)
{
	struct net_if *iface = net_if_get_default();
	int ret;

	if (iface == NULL) {
		plat_uart_puts("netbridge: no interface\r\n");
		return;
	}

	ret = net_promisc_mode_on(iface);
	if (ret != 0) {
		/* -EALREADY is fine - someone else turned it on. Anything
		 * else means the driver refused, and the guest NIC would be
		 * deaf; say so rather than boot a guest that DHCPs forever. */
		if (ret != -EALREADY) {
			plat_uart_puts("netbridge: promiscuous mode REFUSED; "
				       "guest NIC will be deaf\r\n");
			return;
		}
	}

	k_thread_create(&nb_tcb, nb_stack, K_THREAD_STACK_SIZEOF(nb_stack),
			nb_rx_thread, NULL, NULL, NULL,
			CONFIG_RVT_NET_GUEST_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&nb_tcb, "guestnet");
	nb.up = true;
	plat_uart_puts("guest NIC bridged to eth0 (promiscuous)\r\n");
}

uint32_t rvt_netbridge_stats(uint32_t *tx, uint32_t *drops)
{
	if (tx != NULL) {
		*tx = nb.tx_sent;
	}
	if (drops != NULL) {
		*drops = nb.rx_dropped + nb.tx_dropped;
	}
	return nb.rx_kept;
}
