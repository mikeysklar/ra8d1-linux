/*
 * telnet.c - the guest's console, carried over TCP.
 *
 * Ported from ra8d1-linux/rvlinux/src/main.c, where this same bridge is
 * running on this same board against the nommu mini-rv32ima guest. See
 * ra8d1-linux/notes/07-telnet-bridge.md for the reasoning; only the
 * differences are repeated here.
 *
 * The one structural difference is where the console is tapped. In the
 * rvlinux app the hooks had to be spliced into its mmio_load/mmio_store by
 * hand, because that emulator has no platform layer. Here every byte of guest
 * console traffic already passes through plat_putc()/plat_getc() - the 8250
 * model in emu/rv_machine.c and the SBI console in emu/rv_hostsbi.c both go
 * through them - so this file only has to provide the two hooks that
 * src/platform_zephyr.c consults, and emu/ is untouched. That also means the
 * SBI earlycon output, which the rvlinux port does not have, is carried too.
 *
 * Two hard constraints shape the code below, both inherited unchanged:
 *
 * 1. The emulator thread must never block. Any stall in it is a visible freeze
 *    of the guest, and a TCP send can stall for a retransmit timeout. So the
 *    console hooks only ever touch a ring buffer and the socket thread does
 *    all of the waiting. When the TX ring is full, guest output is dropped and
 *    counted rather than throttled.
 *
 * 2. The emulator thread must run below every networking thread, because it
 *    never yields either. rvt_emu_priority_drop() handles that.
 *
 * The rings are strict single-producer / single-consumer with one thread on
 * each side, which is what lets them run without a lock.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>

#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/dhcpv4.h>
#ifdef CONFIG_RVT_TELNET
#include <zephyr/net/socket.h>
#endif

#include <errno.h>

#include "telnet.h"

/* ------------------------------------------------------------ tiny output
 *
 * Deliberately not printk(): these lines interleave with guest output on the
 * same UART and have to cost nothing when nothing is happening.
 */

static void up(char c)
{
	plat_uart_putc(c);
}

static void us(const char *s)
{
	plat_uart_puts(s);
}

static void udec(uint32_t v)
{
	char b[12];
	int n = 0;

	if (v == 0) {
		up('0');
		return;
	}
	while (v > 0 && n < (int)sizeof(b)) {
		b[n++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (n-- > 0) {
		up(b[n]);
	}
}

/* --------------------------------------------------------- addressing */

static bool net_announced;

static void print_ipv4(const struct net_in_addr *a)
{
	for (int i = 0; i < 4; i++) {
		udec(a->s4_addr[i]);
		if (i < 3) {
			up('.');
		}
	}
}

static struct net_in_addr *net_addr_now(void)
{
	struct net_if *iface = net_if_get_default();

	if (iface == NULL) {
		return NULL;
	}
	return net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
}

/* On the UART, never on the telnet stream: this is the message that tells you
 * where to telnet to, so it has to reach the wire that already works. */
static void announce_addr(const struct net_in_addr *a)
{
	net_announced = true;

	us("\r\n***  board ip ");
	print_ipv4(a);
#ifdef CONFIG_RVT_TELNET
	us("   guest console:  telnet ");
	print_ipv4(a);
	if (CONFIG_RVT_TELNET_PORT != 23) {
		us(" ");
		udec(CONFIG_RVT_TELNET_PORT);
	}
#endif
	us("  ***\r\n\r\n");
}

static int set_static(const char *addr, const char *mask, const char *gw)
{
	struct net_if *iface = net_if_get_default();
	struct net_in_addr a, m, g;

	if (iface == NULL || net_addr_pton(NET_AF_INET, addr, &a) < 0) {
		return -EINVAL;
	}
	if (net_if_ipv4_addr_add(iface, &a, NET_ADDR_MANUAL, 0) == NULL) {
		return -ENOMEM;
	}
	if (mask != NULL && *mask != '\0' &&
	    net_addr_pton(NET_AF_INET, mask, &m) == 0) {
		(void)net_if_ipv4_set_netmask_by_addr(iface, &a, &m);
	}
	if (gw != NULL && *gw != '\0' &&
	    net_addr_pton(NET_AF_INET, gw, &g) == 0) {
		net_if_ipv4_set_gw(iface, &g);
	}
	return 0;
}

void rvt_net_bringup(void)
{
	struct net_if *iface = net_if_get_default();
	struct net_linkaddr *ll;
	struct net_in_addr *a;

	us("net: ");

	if (iface == NULL) {
		us("no interface (ethernet driver did not bind)\r\n");
		return;
	}

	ll = net_if_get_link_addr(iface);
	if (ll != NULL && ll->len == 6) {
		us("mac ");
		for (int i = 0; i < 6; i++) {
			up("0123456789abcdef"[ll->addr[i] >> 4]);
			up("0123456789abcdef"[ll->addr[i] & 0xf]);
			if (i < 5) {
				up(':');
			}
		}
		us(", ");
	}

	if (!net_if_is_admin_up(iface)) {
		(void)net_if_up(iface);
	}

#ifdef CONFIG_RVT_NET_STATIC_IP
	if (set_static(CONFIG_RVT_NET_ADDR, CONFIG_RVT_NET_NETMASK,
		       CONFIG_RVT_NET_GATEWAY) != 0) {
		us("static address rejected\r\n");
		return;
	}
	us("static ");
	print_ipv4(net_addr_now());
	us("\r\n");
#else
	us("dhcp");
	net_dhcpv4_start(iface);

	for (int i = 0; i < CONFIG_RVT_NET_DHCP_WAIT_S * 10; i++) {
		if (net_addr_now() != NULL) {
			break;
		}
		/* One dot per second: the link has to come up and autoneg has
		 * to finish before the first DISCOVER goes anywhere, so a few
		 * seconds of silence here is normal, not a fault. */
		if (i % 10 == 0) {
			up('.');
		}
		k_msleep(100);
	}

	if (net_addr_now() == NULL) {
		us(" no lease");
		if (sizeof(CONFIG_RVT_NET_FALLBACK_ADDR) > 1) {
			us(", falling back to ");
			us(CONFIG_RVT_NET_FALLBACK_ADDR);
			if (set_static(CONFIG_RVT_NET_FALLBACK_ADDR,
				       CONFIG_RVT_NET_FALLBACK_NETMASK,
				       NULL) != 0) {
				us(" (rejected)");
			}
		} else {
			us(" (dhcp still running)");
		}
	}
	us("\r\n");
#endif

	a = net_addr_now();
	if (a != NULL) {
		announce_addr(a);
	}
}

void rvt_emu_priority_drop(void)
{
	k_thread_priority_set(k_current_get(), CONFIG_RVT_EMU_PRIORITY);
}

#ifdef CONFIG_RVT_TELNET

/* ------------------------------------------------------------ ring buffers */

BUILD_ASSERT((CONFIG_RVT_TELNET_TX_RING & (CONFIG_RVT_TELNET_TX_RING - 1)) == 0,
	     "TX ring size must be a power of two");
BUILD_ASSERT((CONFIG_RVT_TELNET_RX_RING & (CONFIG_RVT_TELNET_RX_RING - 1)) == 0,
	     "RX ring size must be a power of two");

struct spsc {
	uint8_t *buf;
	uint32_t mask;
	volatile uint32_t head;   /* consumer owns */
	volatile uint32_t tail;   /* producer owns */
};

static uint8_t tn_txbuf[CONFIG_RVT_TELNET_TX_RING];
static uint8_t tn_rxbuf[CONFIG_RVT_TELNET_RX_RING];

/* tx: guest -> client, produced by the emulator thread.
 * rx: client -> guest, produced by the telnet thread. */
static struct spsc tn_tx = { .buf = tn_txbuf,
			     .mask = CONFIG_RVT_TELNET_TX_RING - 1 };
static struct spsc tn_rx = { .buf = tn_rxbuf,
			     .mask = CONFIG_RVT_TELNET_RX_RING - 1 };

static inline bool spsc_put(struct spsc *r, uint8_t b)
{
	uint32_t t = r->tail;
	uint32_t n = (t + 1) & r->mask;

	if (n == r->head) {
		return false;   /* full: caller drops, never waits */
	}
	r->buf[t] = b;
	barrier_dmem_fence_full();   /* data visible before the publishing index */
	r->tail = n;
	return true;
}

static inline int spsc_get(struct spsc *r)
{
	uint32_t h = r->head;
	uint8_t b;

	if (h == r->tail) {
		return -1;
	}
	b = r->buf[h];
	barrier_dmem_fence_full();
	r->head = (h + 1) & r->mask;
	return b;
}

/* Only ever called with no client attached, i.e. with the producer of both
 * rings quiescent. */
static void spsc_reset(struct spsc *r)
{
	r->head = 0;
	r->tail = 0;
}

/* The one flag the emulator's hot path reads. Set only after the socket is
 * fully negotiated, cleared before the socket is closed. */
static volatile bool tn_active;

static uint32_t tn_dropped;    /* guest bytes discarded on a full TX ring */
static uint32_t tn_sessions;

/* ------------------------------------------------------ telnet protocol
 *
 * Enough of RFC 854 to keep a real client's negotiation out of the guest's
 * shell, and no more. The point is character-at-a-time: the guest's tty does
 * its own echo and line editing, so the client must not.
 */

#define TN_IAC   255
#define TN_DONT  254
#define TN_DO    253
#define TN_WONT  252
#define TN_WILL  251
#define TN_SB    250
#define TN_SE    240

#define TN_OPT_ECHO   1
#define TN_OPT_SGA    3   /* suppress go-ahead */

enum tn_state { TN_S_DATA, TN_S_IAC, TN_S_OPT, TN_S_SB, TN_S_SB_IAC };

static enum tn_state tn_state;
static uint8_t tn_verb;        /* the WILL/WONT/DO/DONT awaiting its option */
static bool tn_saw_cr;         /* to swallow the NUL or LF of a CR pair */
static bool tn_said_echo;      /* announced already; do not re-announce */
static bool tn_said_sga;

/* WILL ECHO plus WILL SUPPRESS-GO-AHEAD is the classic pair that drops a
 * client out of line mode; DO SGA asks for the same in reverse so the client
 * stops waiting for a go-ahead that will never come. */
static const uint8_t tn_hello[] = {
	TN_IAC, TN_WILL, TN_OPT_ECHO,
	TN_IAC, TN_WILL, TN_OPT_SGA,
	TN_IAC, TN_DO,   TN_OPT_SGA,
};

static int tn_send_all(int s, const uint8_t *p, size_t n)
{
	while (n != 0) {
		ssize_t w = zsock_send(s, p, n, 0);

		if (w <= 0) {
			return -1;
		}
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

/*
 * Feed one received byte through the negotiation filter. Data bytes land in
 * the RX ring; protocol bytes are answered into `reply`.
 *
 * Loop avoidance is the only subtle part: a reply is sent for DO and WILL,
 * which are requests, and never for DONT or WONT, which are final. An option
 * already announced is not announced again, so a client that echoes our WILL
 * back as DO does not start a ping-pong.
 */
static void tn_rx_byte(uint8_t b, uint8_t *reply, int *rn)
{
	switch (tn_state) {
	case TN_S_DATA:
		if (b == TN_IAC) {
			tn_state = TN_S_IAC;
			return;
		}
		/* Enter arrives as CR LF or CR NUL. The guest tty maps CR to
		 * newline itself (ICRNL), so pass the CR and eat the tail. */
		if (tn_saw_cr && (b == 0x00 || b == 0x0a)) {
			tn_saw_cr = false;
			return;
		}
		tn_saw_cr = (b == 0x0d);
		if (!spsc_put(&tn_rx, b)) {
			/* Guest is not draining its console. Dropping input is
			 * the only option that does not stall this thread. */
		}
		return;

	case TN_S_IAC:
		switch (b) {
		case TN_IAC:            /* escaped literal 0xFF */
			tn_state = TN_S_DATA;
			tn_saw_cr = false;
			(void)spsc_put(&tn_rx, TN_IAC);
			return;
		case TN_WILL:
		case TN_WONT:
		case TN_DO:
		case TN_DONT:
			tn_verb = b;
			tn_state = TN_S_OPT;
			return;
		case TN_SB:
			tn_state = TN_S_SB;
			return;
		default:                /* NOP, AYT, IP, BRK, ... : ignored */
			tn_state = TN_S_DATA;
			return;
		}

	case TN_S_OPT:
		tn_state = TN_S_DATA;
		if (tn_verb == TN_DO) {
			if (b == TN_OPT_ECHO && !tn_said_echo) {
				tn_said_echo = true;
				reply[(*rn)++] = TN_IAC;
				reply[(*rn)++] = TN_WILL;
				reply[(*rn)++] = b;
			} else if (b == TN_OPT_SGA && !tn_said_sga) {
				tn_said_sga = true;
				reply[(*rn)++] = TN_IAC;
				reply[(*rn)++] = TN_WILL;
				reply[(*rn)++] = b;
			} else if (b != TN_OPT_ECHO && b != TN_OPT_SGA) {
				reply[(*rn)++] = TN_IAC;
				reply[(*rn)++] = TN_WONT;
				reply[(*rn)++] = b;
			}
		} else if (tn_verb == TN_WILL) {
			/* The client may suppress go-ahead; anything else it
			 * offers we refuse, so nothing it sends needs parsing
			 * beyond what is handled here. */
			reply[(*rn)++] = TN_IAC;
			reply[(*rn)++] = (b == TN_OPT_SGA) ? TN_DO : TN_DONT;
			reply[(*rn)++] = b;
		}
		/* WONT and DONT are confirmations. Answering them loops. */
		return;

	case TN_S_SB:
		if (b == TN_IAC) {
			tn_state = TN_S_SB_IAC;
		}
		return;

	case TN_S_SB_IAC:
		/* IAC IAC inside a subnegotiation is escaped data, not the end. */
		tn_state = (b == TN_SE) ? TN_S_DATA : TN_S_SB;
		return;
	}
}

/* Drain the TX ring to the socket. Returns -1 if the client is gone.
 *
 * Bytes are batched rather than written one at a time: a boot log at one TCP
 * segment per character would be both slow and rude to the network. 0xFF is
 * doubled here, off the emulator's hot path, so the guest can emit arbitrary
 * bytes without them reading as IAC. */
static int tn_flush(int s)
{
	uint8_t out[256];

	for (;;) {
		int n = 0, c;

		while (n < (int)sizeof(out) - 1 &&
		       (c = spsc_get(&tn_tx)) >= 0) {
			out[n++] = (uint8_t)c;
			if (c == TN_IAC) {
				out[n++] = TN_IAC;
			}
		}
		if (n == 0) {
			return 0;
		}
		if (tn_send_all(s, out, (size_t)n) != 0) {
			return -1;
		}
	}
}

static void tn_serve(int s, const struct net_in_addr *peer)
{
	struct zsock_pollfd pfd = { .fd = s, .events = ZSOCK_POLLIN };
	int one = 1;

	/* No Nagle: this is an interactive console, and the batched flush
	 * already does the coalescing Nagle would. Keepalive so a client that
	 * disappears without a FIN eventually releases the console back to the
	 * UART instead of capturing it forever. */
	(void)zsock_setsockopt(s, NET_IPPROTO_TCP, ZSOCK_TCP_NODELAY,
			       &one, sizeof(one));
	(void)zsock_setsockopt(s, ZSOCK_SOL_SOCKET, ZSOCK_SO_KEEPALIVE,
			       &one, sizeof(one));

	tn_state = TN_S_DATA;
	tn_saw_cr = false;
	tn_said_echo = false;
	tn_said_sga = false;

	/* Safe to reset: tn_active is still false, so the emulator thread is
	 * not producing into tn_tx and this thread is the only user of tn_rx. */
	spsc_reset(&tn_tx);
	spsc_reset(&tn_rx);

	if (tn_send_all(s, tn_hello, sizeof(tn_hello)) != 0) {
		return;
	}

	/* Announced only once the handshake is out on the wire, so the console
	 * is never reported as moved when it did not move. */
	tn_sessions++;
	tn_dropped = 0;
	us("telnet: session ");
	udec(tn_sessions);
	us(" from ");
	print_ipv4(peer);
	us(", console moving off uart\r\n");

	tn_active = true;

	/* Nudge the guest shell into printing a prompt, so the client sees
	 * something immediately instead of an apparently dead connection. */
	(void)spsc_put(&tn_rx, '\r');

	for (;;) {
		uint8_t in[128], reply[64];
		int rn = 0;
		ssize_t got;

		/* 5 ms is the console's worst-case output latency and the idle
		 * wakeup rate. Short enough to feel immediate, long enough that
		 * the emulator is not being preempted constantly. */
		int pr = zsock_poll(&pfd, 1, 5);

		if (pr < 0) {
			break;
		}

		if (pr > 0 && (pfd.revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP |
					      ZSOCK_POLLNVAL))) {
			break;
		}

		if (pr > 0 && (pfd.revents & ZSOCK_POLLIN)) {
			got = zsock_recv(s, in, sizeof(in), 0);
			if (got <= 0) {
				break;   /* 0 is an orderly close */
			}
			for (ssize_t i = 0; i < got; i++) {
				tn_rx_byte(in[i], reply, &rn);
				/* Worst case is 3 bytes per input byte; flush
				 * well before the buffer can overrun. */
				if (rn > (int)sizeof(reply) - 3) {
					if (tn_send_all(s, reply,
							(size_t)rn) != 0) {
						goto done;
					}
					rn = 0;
				}
			}
			if (rn != 0 && tn_send_all(s, reply, (size_t)rn) != 0) {
				break;
			}
		}

		if (tn_flush(s) != 0) {
			break;
		}
	}

done:
	/* Report before releasing the console. Once tn_active clears, the
	 * emulator starts writing the UART again and this line would be shot
	 * through with guest output. */
	us("telnet: client gone, console back on uart");
	if (tn_dropped != 0) {
		us(", ");
		udec(tn_dropped);
		us(" guest bytes dropped");
	}
	us("\r\n");

	tn_active = false;

	/* The emulator may have been mid-put when the flag cleared; discard
	 * whatever is left rather than leaking it into the next session or
	 * onto the UART out of order. */
	while (spsc_get(&tn_tx) >= 0) {
	}
}

static void tn_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	/* If the boot-time DHCP wait timed out, the lease may still be on its
	 * way. Say so on the UART when it lands, so the address is never a
	 * thing anyone has to go hunting for. */
	while (net_addr_now() == NULL) {
		k_msleep(250);
	}
	if (!net_announced) {
		announce_addr(net_addr_now());
	}

	for (;;) {
		struct net_sockaddr_in sa = {
			.sin_family = NET_AF_INET,
			.sin_port = net_htons(CONFIG_RVT_TELNET_PORT),
			.sin_addr.s_addr = 0,   /* NET_INADDR_ANY */
		};
		int ls = zsock_socket(NET_AF_INET, NET_SOCK_STREAM,
				      NET_IPPROTO_TCP);

		if (ls < 0) {
			k_sleep(K_SECONDS(2));
			continue;
		}
		if (zsock_bind(ls, (struct net_sockaddr *)&sa, sizeof(sa)) < 0 ||
		    zsock_listen(ls, 1) < 0) {
			zsock_close(ls);
			k_sleep(K_SECONDS(2));
			continue;
		}

		/* One client at a time. A second connection is accepted and
		 * dropped immediately rather than left queued, so whoever tries
		 * it gets an answer instead of a hang. */
		for (;;) {
			struct net_sockaddr_in peer;
			net_socklen_t plen = sizeof(peer);
			int cs = zsock_accept(ls, (struct net_sockaddr *)&peer,
					      &plen);

			if (cs < 0) {
				break;   /* rebuild the listener */
			}

			if (tn_active) {
				static const char busy[] =
					"\r\nguest console already in use\r\n";

				(void)tn_send_all(cs, (const uint8_t *)busy,
						  sizeof(busy) - 1);
				zsock_close(cs);
				continue;
			}

			tn_serve(cs, &peer.sin_addr);
			zsock_close(cs);
		}

		zsock_close(ls);
	}
}

K_THREAD_STACK_DEFINE(tn_stack, CONFIG_RVT_TELNET_THREAD_STACK);
static struct k_thread tn_tcb;

void rvt_telnet_start(void)
{
	k_thread_create(&tn_tcb, tn_stack, K_THREAD_STACK_SIZEOF(tn_stack),
			tn_thread, NULL, NULL, NULL,
			CONFIG_RVT_TELNET_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&tn_tcb, "telnet");
}

/* ------------------------------------- what plat_putc/plat_getc consult */

bool rvt_console_out(char c)
{
	if (!tn_active) {
		return false;
	}
	if (!spsc_put(&tn_tx, (uint8_t)c)) {
		tn_dropped++;
	}
	/* Mirroring costs one uart_poll_out() per guest character on the
	 * emulator's hot path - about 11 us of busy-wait at 921600 baud - so
	 * it is off unless someone is debugging the bridge itself. */
	return !IS_ENABLED(CONFIG_RVT_TELNET_MIRROR_UART);
}

int rvt_console_in(void)
{
	if (!tn_active) {
		return -1;
	}
	return spsc_get(&tn_rx);
}

#endif /* CONFIG_RVT_TELNET */
