/*
 * netbridge.h - guest NIC to real Ethernet, via promiscuous mode.
 *
 * See netbridge.c for the design and the why-not-CONFIG_NET_ETHERNET_BRIDGE
 * reasoning. The rv_platform.h side of this interface (plat_net_mac/send/
 * recv) is declared there; this header only carries the app-facing calls,
 * with the same stub-when-disabled pattern as telnet.h so main.c stays free
 * of #ifdefs.
 */
#ifndef NETBRIDGE_H_
#define NETBRIDGE_H_

#include <stdint.h>

#ifdef CONFIG_RVT_GUEST_NET

/* Turn on promiscuous RX, start the filter thread, open the guest's path to
 * the wire. Call after rvt_net_bringup(); before the guest boots, so its
 * first DHCP DISCOVER already has a wire to go out on. */
void rvt_netbridge_start(void);

/* Frames delivered to the guest; TX count and total drops via out-params. */
uint32_t rvt_netbridge_stats(uint32_t *tx, uint32_t *drops);

/*
 * Turn promiscuous RX back off, permanently, for the life of this boot.
 *
 * Called by the image pusher once it has stopped the guest. Promiscuous mode
 * is not free on the receive path: net_if_recv_data() does a full
 * net_pkt_clone() of EVERY frame the interface sees, and a 1514-byte frame is
 * 12 of the 32 net_bufs in the RX pool, so an in-flight frame plus its clone
 * is 24 of 32. During a multi-megabyte push that pressure is what the
 * transfer is competing against - and with the guest halted, every cloned
 * frame is filtered and freed without ever reaching it. See src/pusher.c and
 * notes/pusher.md.
 *
 * There is no resume: the guest cannot run again without a reboot, so nothing
 * would consume what the bridge delivers.
 */
void rvt_netbridge_suspend(void);

#else /* !CONFIG_RVT_GUEST_NET */

static inline void rvt_netbridge_start(void)
{
}

static inline void rvt_netbridge_suspend(void)
{
}

static inline uint32_t rvt_netbridge_stats(uint32_t *tx, uint32_t *drops)
{
	if (tx != NULL) {
		*tx = 0;
	}
	if (drops != NULL) {
		*drops = 0;
	}
	return 0;
}

#endif /* CONFIG_RVT_GUEST_NET */

#endif /* NETBRIDGE_H_ */
