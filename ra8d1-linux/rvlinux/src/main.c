/*
 * Emulated RISC-V Linux on the Renesas EK-RA8D1 (Cortex-M85 @ 480 MHz).
 *
 * Runs cnlohr's mini-rv32ima interpreter over the board's 64 MB external
 * SDRAM, booting a rv32ima nommu Linux kernel with an embedded initramfs.
 *
 * Why this shape:
 *   - Cortex-M85 has no MMU, and upstream Linux arch/arm NOMMU supports only
 *     ARMv7-M, so native Linux is impossible here. Emulating a RISC-V lets a
 *     stock, upstream-supported kernel run unmodified.
 *
 *     NOTE: this does NOT give the guest an MMU. mini-rv32ima is machine-mode
 *     only (no satp, no supervisor CSRs, no page-table walk) and the kernel is
 *     a nommu build -- the guest reports "mmu : none". Every process still
 *     needs a physically contiguous allocation, so the fragmentation failure
 *     mode of no-MMU Linux is present here too. What helps is 64 MB instead of
 *     8, plus CONFIG_ARCH_FORCE_MAX_ORDER=13 (4 MB -> 32 MB max contiguous).
 *   - The kernel image is 3.4 MB, larger than the 2 MB internal flash, so it
 *     lives in the 64 MB octo-SPI NOR at CS1. That NOR is memory-mapped at
 *     0x90000000, so loading the guest is a plain memcpy with no driver in
 *     the path.
 *
 * Memory map:
 *   0x68000000  64 MB SDRAM   guest RAM (guest sees it as 0x80000000)
 *   0x90000000  64 MB OSPI    0x00000-0x1FFFF CIRCUITPY fs, 0x2000 is the
 *                             OSPI driver's autocalibration sector.
 *                             0x40000 kernel slot, then the rootfs slot.
 *
 * Images live in two independent slots, each with its own magic, length and
 * CRC, and both survive resets and app reflashes. Three ways to load one:
 *
 *   - TCP, the normal one. A listener runs alongside the telnet console for
 *     as long as the board is up, so a push lands while the guest keeps
 *     running and takes effect at the next reset.
 *   - The boot-time UART loader, entered by pressing 'L' at the prompt. This
 *     is the recovery path: it is the only one that exists before the network
 *     stack does.
 *   - The same UART loader, entered automatically when the kernel slot holds
 *     nothing valid, so a blank board is never stuck.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/cache.h>
#include <string.h>

#ifdef CONFIG_NETWORKING
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/dhcpv4.h>
#endif
#if defined(CONFIG_RVL_TELNET) || defined(CONFIG_RVL_NETLOAD)
#include <zephyr/net/socket.h>
#endif

/* ------------------------------------------------------------------ layout */

#define SDRAM_BASE      DT_REG_ADDR(DT_NODELABEL(sdram1))
#define SDRAM_SIZE      DT_REG_SIZE(DT_NODELABEL(sdram1))

/* Guest RAM. The whole SDRAM is handed to the guest; the DTB and the
 * emulator's own state live in the top of it, exactly as the reference
 * implementation arranges them. */
#define GUEST_RAM_SIZE  SDRAM_SIZE

/* Memory-mapped window of the OSPI NOR on CS1, from the FSP BSP:
 * BSP_FEATURE_OSPI_B_DEVICE_1_START_ADDRESS. Reads through here are the
 * same operation Zephyr's flash_read() performs internally. */
#define OSPI_MMAP_BASE  0x90000000UL

#define IMG_HDR_SIZE    16U
#define IMG_PAYLOAD_OFF 4096U   /* header gets its own aligned block */
#define OSPI_ERASE_SZ   (256U * 1024U)
#define OSPI_SIZE       (64U * 1024U * 1024U)

/* The last 256 KB block of the chip is never used. Zephyr's flash_erase() asks
 * the page layout for the page at `offset + len`, and flash_get_page_info()
 * returns -EINVAL for an offset one past the final page, so an erase that ends
 * exactly at the top of the chip fails outright. Keeping one block in hand
 * costs nothing at any image size we care about and removes the edge. */
#define OSPI_USABLE_END (OSPI_SIZE - OSPI_ERASE_SZ)

/* Two independent slots rather than one image.
 *
 * The guest is moving to a gzipped kernel plus a separate read-only rootfs, so
 * there are now two artifacts to store. Two self-describing slots beat one
 * container header listing regions, for three reasons:
 *
 *   - Each slot carries its own magic, length and CRC, which is exactly the
 *     check that already existed. There is no new format to get wrong, and a
 *     slot validates without reference to anything outside itself.
 *   - Pushing one artifact cannot disturb the other. A container index has to
 *     be rewritten whenever either region changes, so a failed push takes out
 *     both. Slots are aligned to 256 KB and therefore never share an erase
 *     block, so the hardware enforces the isolation rather than the code.
 *   - Recovery is per-slot: a bad rootfs still leaves a bootable kernel.
 *
 * Slot 0 starts at 0x40000, where the single image always lived -- past both
 * the CIRCUITPY filesystem and the driver's autocalibration sector, and on the
 * boundary where uniform 256 KB erase blocks begin. An image pushed before
 * slots existed still validates and still boots.
 *
 * The distinct magic per slot is not redundant with the offset: it is what
 * catches a rootfs pushed into the kernel slot, which is a mistake worth one
 * byte of comparison to prevent. */
#define SLOT_KERNEL_OFF  0x40000UL
#define SLOT_KERNEL_SZ   ((uint32_t)CONFIG_RVL_KERNEL_SLOT_KB * 1024U)
#define SLOT_ROOTFS_OFF  (SLOT_KERNEL_OFF + SLOT_KERNEL_SZ)
#define SLOT_ROOTFS_SZ   (OSPI_USABLE_END - SLOT_ROOTFS_OFF)

BUILD_ASSERT(SLOT_KERNEL_OFF % OSPI_ERASE_SZ == 0, "kernel slot must be erase-aligned");
BUILD_ASSERT(SLOT_KERNEL_SZ % OSPI_ERASE_SZ == 0, "kernel slot must be a whole number of blocks");
BUILD_ASSERT(SLOT_KERNEL_SZ > IMG_PAYLOAD_OFF, "kernel slot is smaller than its own header");
BUILD_ASSERT(SLOT_ROOTFS_OFF + SLOT_ROOTFS_SZ <= OSPI_USABLE_END, "slots overrun the chip");

struct img_hdr {
	char     magic[8];
	uint32_t len;
	uint32_t crc;
};

struct img_slot {
	const char *name;
	uint32_t    off;
	uint32_t    size;
	const char *magic;      /* exactly 8 bytes */
};

enum { SLOT_KERNEL = 0, SLOT_ROOTFS = 1 };

static const struct img_slot img_slots[] = {
	{ "kernel", SLOT_KERNEL_OFF, SLOT_KERNEL_SZ, "RA8LINUX" },
	{ "rootfs", SLOT_ROOTFS_OFF, SLOT_ROOTFS_SZ, "RA8ROOTF" },
};

#define IMG_NSLOTS ARRAY_SIZE(img_slots)

#define SLOT_MMAP(s)   (OSPI_MMAP_BASE + (s)->off)

/* --------------------------------------------------------------- emulator */

#define MINI_RV32_RAM_SIZE   GUEST_RAM_SIZE
#define MINIRV32_IMPLEMENTATION

static uint32_t mmio_store(uint32_t addy, uint32_t val);
static uint32_t mmio_load(uint32_t addy);

/* The syscon path signals poweroff/reboot by returning the written value up
 * through MiniRV32IMAStep. That `return val` is load-bearing. */
#define MINIRV32_HANDLE_MEM_STORE_CONTROL(addy, val) \
	if (mmio_store(addy, val)) return val;
#define MINIRV32_HANDLE_MEM_LOAD_CONTROL(addy, rval) \
	rval = mmio_load(addy);

#include "mini-rv32ima.h"
#include "default64mbdtc.h"

static struct MiniRV32IMAState *core;

/* Largest payload guest_prepare() can place. It copies the image to guest
 * offset 0 and puts the DTB and the emulator state at the very top, so an
 * image longer than this would be memcpy'd straight over them. Stated here
 * rather than left implicit because the MMU guest images are an order of
 * magnitude larger than the nommu one this was written for. */
#define IMG_MAX_RAM_LEN (GUEST_RAM_SIZE - sizeof(default64mbdtb) - \
			 sizeof(struct MiniRV32IMAState))

/* ------------------------------------------------------------------- uart */

static const struct device *const uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static inline void up(char c)
{
	uart_poll_out(uart, c);
}

static void us(const char *s)
{
	while (*s) {
		up(*s++);
	}
}

static void uhex(uint32_t v)
{
	static const char d[] = "0123456789abcdef";
	for (int i = 28; i >= 0; i -= 4) {
		up(d[(v >> i) & 0xf]);
	}
}

static void udec(uint32_t v)
{
	char b[11];
	int n = 0;
	if (!v) {
		up('0');
		return;
	}
	while (v) {
		b[n++] = '0' + (v % 10);
		v /= 10;
	}
	while (n--) {
		up(b[n]);
	}
}

/* One-byte lookahead. The guest polls the 8250 LSR far more often than a
 * byte actually arrives, so keep the peeked byte here rather than dropping
 * it between the LSR read and the data read.
 *
 * Only ever touched from the emulator thread: the guest console path below and
 * the image loader both run there, and the telnet thread reaches the guest
 * through a ring instead. */
static int rx_have = -1;

static int uart_rx_poll(void)
{
	unsigned char c;

	if (rx_have >= 0) {
		return rx_have;
	}
	if (uart_poll_in(uart, &c) == 0) {
		rx_have = c;
	}
	return rx_have;
}

static int uart_rx_take(void)
{
	int c = uart_rx_poll();

	rx_have = -1;
	return c;
}

/* Blocking read used only by the loader, never on the guest hot path.
 *
 * Deliberately UART-only. A telnet client that connects mid-upload must not be
 * able to inject bytes into the image stream. */
static int rx_block(k_timeout_t to)
{
	int64_t end = k_uptime_get() + k_ticks_to_ms_floor64(to.ticks);
	int c;

	do {
		c = uart_rx_take();
		if (c >= 0) {
			return c;
		}
	} while (k_uptime_get() < end);

	return -1;
}

/* ------------------------------------------------- guest console over TCP */

/*
 * The guest kernel is built CONFIG_NET=n and its BusyBox has no telnetd, so it
 * cannot serve a shell itself. It does not have to: the shell it already runs
 * on its emulated 8250 has *both* ends inside this app. mmio_store(0x10000000)
 * is the guest's stdout and mmio_load(0x10000000/0x10000005) is its stdin, so
 * a telnet server here is just a matter of re-pointing those two ends at a
 * socket. Nothing in the guest changes, and nothing in the guest can tell.
 *
 * Two hard constraints shape the code below.
 *
 * 1. The emulator step loop must never block. It is the thread executing guest
 *    instructions; any stall in it is a visible freeze of the guest, and a TCP
 *    send can stall for as long as a retransmit timeout. So the mmio hooks only
 *    ever touch a ring buffer, and the socket thread does all the waiting.
 *
 * 2. The step loop also never *yields*. Zephyr schedules strictly by priority
 *    among preemptible threads, so a loop that neither sleeps nor blocks starves
 *    everything numerically below it -- including the Ethernet RX thread at
 *    priority 2. main() therefore drops itself to CONFIG_RVL_EMU_PRIORITY (10)
 *    before entering the loop. Interrupts and every networking thread then
 *    preempt the guest, which is the correct ordering: a few microseconds of
 *    stolen guest time per packet is unmeasurable, a dropped packet is not.
 *
 * The rings are strict single-producer / single-consumer, one thread on each
 * side, which is what lets them run without a lock. That matters because
 * spsc_empty() sits on the hot path: Linux polls the 8250 LSR far more often
 * than a byte actually moves, so the common case must cost two loads and a
 * compare, not an interrupt lock.
 */

#ifdef CONFIG_NETWORKING
/* Defined with the rest of the addressing code below; the telnet thread needs
 * them to announce a lease that arrives after the boot-time wait gave up. */
static struct net_in_addr *net_addr_now(void);
static void announce_addr(const struct net_in_addr *a);
static void print_ipv4(const struct net_in_addr *a);
static bool net_announced;
#endif

#ifdef CONFIG_RVL_TELNET

BUILD_ASSERT((CONFIG_RVL_TELNET_TX_RING & (CONFIG_RVL_TELNET_TX_RING - 1)) == 0,
	     "TX ring size must be a power of two");
BUILD_ASSERT((CONFIG_RVL_TELNET_RX_RING & (CONFIG_RVL_TELNET_RX_RING - 1)) == 0,
	     "RX ring size must be a power of two");

struct spsc {
	uint8_t *buf;
	uint32_t mask;
	volatile uint32_t head;   /* consumer owns */
	volatile uint32_t tail;   /* producer owns */
};

static uint8_t tn_txbuf[CONFIG_RVL_TELNET_TX_RING];
static uint8_t tn_rxbuf[CONFIG_RVL_TELNET_RX_RING];

/* tx: guest -> client, produced by the emulator thread.
 * rx: client -> guest, produced by the telnet thread. */
static struct spsc tn_tx = { .buf = tn_txbuf, .mask = CONFIG_RVL_TELNET_TX_RING - 1 };
static struct spsc tn_rx = { .buf = tn_rxbuf, .mask = CONFIG_RVL_TELNET_RX_RING - 1 };

static inline bool spsc_empty(struct spsc *r)
{
	return r->head == r->tail;
}

static inline bool spsc_put(struct spsc *r, uint8_t b)
{
	uint32_t t = r->tail;
	uint32_t n = (t + 1) & r->mask;

	if (n == r->head) {
		return false;   /* full: caller drops, never waits */
	}
	r->buf[t] = b;
	barrier_dmem_fence_full();   /* data visible before the index that publishes it */
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

/* Reset is only ever called with no client attached, i.e. with the producer of
 * both rings quiescent. */
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

/* ---- telnet protocol ----
 *
 * Enough of RFC 854 to keep a real client's negotiation out of the guest's
 * shell, and no more. The whole point is character-at-a-time: the guest's tty
 * does its own echo and line editing, so the client must not.
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

/* Opening offer. WILL ECHO plus WILL SUPPRESS-GO-AHEAD is the classic pair
 * that drops a client out of line mode; DO SGA asks for the same in reverse so
 * the client stops waiting for a go-ahead that will never come. */
static const uint8_t tn_hello[] = {
	TN_IAC, TN_WILL, TN_OPT_ECHO,
	TN_IAC, TN_WILL, TN_OPT_SGA,
	TN_IAC, TN_DO,   TN_OPT_SGA,
};

static int tn_send_all(int s, const uint8_t *p, size_t n)
{
	while (n) {
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
 * we have already announced is not announced again, so a client that echoes
 * our WILL back as DO does not start a ping-pong.
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
			 * offers, we refuse, so nothing it sends needs parsing
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

		while (n < (int)sizeof(out) - 1 && (c = spsc_get(&tn_tx)) >= 0) {
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

	/* No Nagle: this is an interactive console, and a batched flush already
	 * does the coalescing that Nagle would. Keepalive so a client that
	 * disappears without a FIN eventually releases the console back to the
	 * UART instead of leaving it captured forever. */
	(void)zsock_setsockopt(s, NET_IPPROTO_TCP, ZSOCK_TCP_NODELAY, &one, sizeof(one));
	(void)zsock_setsockopt(s, ZSOCK_SOL_SOCKET, ZSOCK_SO_KEEPALIVE, &one, sizeof(one));

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

	/* Announced only once the handshake is actually out on the wire, so the
	 * console is never reported as moved when it did not move. */
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

		if (pr > 0 && (pfd.revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL))) {
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
					if (tn_send_all(s, reply, (size_t)rn) != 0) {
						goto done;
					}
					rn = 0;
				}
			}
			if (rn && tn_send_all(s, reply, (size_t)rn) != 0) {
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
	if (tn_dropped) {
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
	 * thing the user has to go hunting for. */
	while (net_addr_now() == NULL) {
		k_msleep(250);
	}
	if (!net_announced) {
		announce_addr(net_addr_now());
	}

	for (;;) {
		struct net_sockaddr_in sa = {
			.sin_family = NET_AF_INET,
			.sin_port = net_htons(CONFIG_RVL_TELNET_PORT),
			.sin_addr.s_addr = 0,   /* NET_INADDR_ANY */
		};
		int ls = zsock_socket(NET_AF_INET, NET_SOCK_STREAM, NET_IPPROTO_TCP);

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
		 * dropped immediately rather than left queued, so the person
		 * trying it gets an answer instead of a hang. */
		for (;;) {
			struct net_sockaddr_in peer;
			net_socklen_t plen = sizeof(peer);
			int cs = zsock_accept(ls, (struct net_sockaddr *)&peer, &plen);

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

K_THREAD_STACK_DEFINE(tn_stack, CONFIG_RVL_TELNET_THREAD_STACK);
static struct k_thread tn_tcb;

#endif /* CONFIG_RVL_TELNET */

/* ---- what the emulated 8250 actually calls ---- */

/* Guest stdout. Falls back to the UART whenever no client is attached, which
 * is what keeps the serial console working exactly as before. */
static inline void guest_out(char c)
{
#ifdef CONFIG_RVL_TELNET
	if (tn_active) {
		if (!spsc_put(&tn_tx, (uint8_t)c)) {
			tn_dropped++;
		}
		if (IS_ENABLED(CONFIG_RVL_TELNET_MIRROR_UART)) {
			up(c);
		}
		return;
	}
#endif
	up(c);
}

/* Guest stdin. The socket wins when attached, but the UART is still read, so
 * the physical console never goes completely deaf. */
static int guest_rx_poll(void)
{
	if (rx_have >= 0) {
		return rx_have;
	}
#ifdef CONFIG_RVL_TELNET
	if (tn_active) {
		int c = spsc_get(&tn_rx);

		if (c >= 0) {
			rx_have = c;
			return rx_have;
		}
	}
#endif
	return uart_rx_poll();
}

static int guest_rx_take(void)
{
	int c = guest_rx_poll();

	rx_have = -1;
	return c;
}

/* ------------------------------------------------- ethernet and addressing */

#ifdef CONFIG_NETWORKING

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

/* Printed on the UART, never on the telnet stream: this is the message that
 * tells you where to telnet to, so it has to reach the wire that already
 * works. */
static void announce_addr(const struct net_in_addr *a)
{
	net_announced = true;

	us("\r\n***  board ip ");
	print_ipv4(a);
#ifdef CONFIG_RVL_TELNET
	us("   guest console:  telnet ");
	print_ipv4(a);
	if (CONFIG_RVL_TELNET_PORT != 23) {
		us(" ");
		udec(CONFIG_RVL_TELNET_PORT);
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
	if (mask && *mask && net_addr_pton(NET_AF_INET, mask, &m) == 0) {
		(void)net_if_ipv4_set_netmask_by_addr(iface, &a, &m);
	}
	if (gw && *gw && net_addr_pton(NET_AF_INET, gw, &g) == 0) {
		net_if_ipv4_set_gw(iface, &g);
	}
	return 0;
}

/*
 * Bring the interface up and get an address.
 *
 * This blocks the boot for up to CONFIG_RVL_NET_DHCP_WAIT_S, purely so the
 * address can be printed before the guest's own output starts scrolling. On
 * timeout the guest boots anyway; DHCP is left running and the address is
 * announced whenever it turns up.
 */
static void net_bringup(void)
{
	struct net_if *iface = net_if_get_default();

	us("net: ");

	if (iface == NULL) {
		us("no interface (ethernet driver did not bind)\r\n");
		return;
	}

	struct net_linkaddr *ll = net_if_get_link_addr(iface);

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

#ifdef CONFIG_RVL_NET_STATIC_IP
	if (set_static(CONFIG_RVL_NET_ADDR, CONFIG_RVL_NET_NETMASK,
		       CONFIG_RVL_NET_GATEWAY) != 0) {
		us("static address rejected\r\n");
		return;
	}
	us("static ");
	print_ipv4(net_addr_now());
	us("\r\n");
#else
	us("dhcp");
	net_dhcpv4_start(iface);

	for (int i = 0; i < CONFIG_RVL_NET_DHCP_WAIT_S * 10; i++) {
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
		if (sizeof(CONFIG_RVL_NET_FALLBACK_ADDR) > 1) {
			us(", falling back to ");
			us(CONFIG_RVL_NET_FALLBACK_ADDR);
			if (set_static(CONFIG_RVL_NET_FALLBACK_ADDR,
				       CONFIG_RVL_NET_FALLBACK_NETMASK, NULL) != 0) {
				us(" (rejected)");
			}
		} else {
			us(" (dhcp still running)");
		}
	}
	us("\r\n");
#endif

	struct net_in_addr *a = net_addr_now();

	if (a != NULL) {
		announce_addr(a);
	}
}

#endif /* CONFIG_NETWORKING */

/* ------------------------------------------------ paravirtual I/O device */

/*
 * A register-mapped bridge that lets the guest drive the board's *real* I2C
 * bus and GPIO pins through Zephyr's drivers. This is the piece that makes the
 * emulated guest useful for anything other than printing to a console.
 *
 * It sits at 0x11200000, inside MINIRV32_MMIO_RANGE (0x10000000-0x11ffffff)
 * but clear of the UART/CLINT/syscon that the guest DTB already describes, so
 * adding it needs no DTB change: an unmodified guest simply never touches it.
 *
 * ALL GUEST ACCESSES MUST BE 32-BIT AND WORD-ALIGNED. mini-rv32ima hands the
 * store hook a value but not the access width, so a byte store is
 * indistinguishable from a word store carrying the same low byte. The 8250
 * emulation above gets away with that because it only ever looks at bits 7:0;
 * a data window cannot. The DATA window is therefore an array of words, each
 * packing four payload bytes little-endian. Host and guest are both
 * little-endian, so the word array *is* the byte buffer and no swapping is
 * needed on either side.
 *
 * Register map, offsets from PV_BASE:
 *
 *   0x000  ID          R   0x50564930, 'PVI0'
 *   0x004  VERSION     R   PV_VERSION
 *   0x008  CAPS        R   [7:0]   I2C bus count
 *                          [15:8]  GPIO pin count
 *                          [31:16] DATA window size in bytes
 *   0x00c  CMD         W   write a PV_CMD_* code to run it; blocks until done
 *   0x010  STATUS      R   0 on success, else a negative Linux errno
 *   0x014  RESULT      R   command-specific; byte count for I2C reads
 *   0x018  I2C_BUS     RW  bus index, 0 .. buses-1
 *   0x01c  I2C_ADDR    RW  7-bit target address
 *   0x020  I2C_WLEN    RW  bytes to send, taken from DATA
 *   0x024  I2C_RLEN    RW  bytes to receive, placed in DATA
 *   0x028  GPIO_PIN    RW  logical pin index, 0 .. pins-1
 *   0x02c  GPIO_VAL    RW  value to write; GPIO_GET leaves the read value here
 *   0x030  GPIO_FLAGS  RW  PV_GPIO_* bits, consumed by GPIO_CONFIG
 *   0x200  DATA[0..63] RW  64 words = 256 bytes
 *
 * Commands are synchronous: the store that writes CMD does not return until
 * the Zephyr call has finished, so the guest can read STATUS on the very next
 * instruction and never needs to poll a busy bit. That does block the emulator
 * thread for the length of a real bus transaction, which is the intended
 * trade for a bring-up bridge: no queue, no interrupts, no completion race.
 *
 * Cost on the emulator's hot path is nil. MINIRV32_HANDLE_MEM_*_CONTROL only
 * runs for addresses already outside guest RAM, and the PV decode is the last
 * arm of the existing if-chain, so ordinary loads and stores never see it.
 */

#define PV_BASE          0x11200000u
#define PV_MASK          0xfffff000u  /* one guest page */
#define PV_VERSION       1u

#define PV_REG_ID        0x000
#define PV_REG_VERSION   0x004
#define PV_REG_CAPS      0x008
#define PV_REG_CMD       0x00c
#define PV_REG_STATUS    0x010
#define PV_REG_RESULT    0x014
#define PV_REG_I2C_BUS   0x018
#define PV_REG_I2C_ADDR  0x01c
#define PV_REG_I2C_WLEN  0x020
#define PV_REG_I2C_RLEN  0x024
#define PV_REG_GPIO_PIN  0x028
#define PV_REG_GPIO_VAL  0x02c
#define PV_REG_GPIO_FLAG 0x030
#define PV_REG_DATA      0x200

#define PV_ID_MAGIC      0x50564930u

#define PV_DATA_WORDS    64
#define PV_DATA_BYTES    (PV_DATA_WORDS * 4)

/* Commands. */
#define PV_CMD_NOP           0x00
#define PV_CMD_I2C_WRITE     0x01
#define PV_CMD_I2C_READ      0x02
#define PV_CMD_I2C_WRITE_READ 0x03
#define PV_CMD_I2C_PROBE     0x04
#define PV_CMD_GPIO_CONFIG   0x10
#define PV_CMD_GPIO_SET      0x11
#define PV_CMD_GPIO_GET      0x12
#define PV_CMD_GPIO_TOGGLE   0x13

/* GPIO_FLAGS bits. Deliberately our own small ABI rather than Zephyr's
 * gpio_flags_t, so the guest side is not coupled to a Zephyr header. */
#define PV_GPIO_INPUT        BIT(0)
#define PV_GPIO_OUTPUT       BIT(1)
#define PV_GPIO_PULL_UP      BIT(2)
#define PV_GPIO_PULL_DOWN    BIT(3)
#define PV_GPIO_OPEN_DRAIN   BIT(4)
#define PV_GPIO_INIT_HIGH    BIT(5)
#define PV_GPIO_INIT_LOW     BIT(6)
#define PV_GPIO_DISCONNECT   BIT(7)

/* ---- host resources behind the device ---- */

/* iic1 is the RIIC controller on P512/P511. On the EK-RA8D1 those pins come
 * out on the MIPI graphics expansion header (J58) and the DVP camera
 * connector (J13); there is no Qwiic/Grove connector on this board, so an
 * external device needs flying leads and its own pull-ups. app.overlay marks
 * the node okay, following the same pattern the Renesas LCD shields use. */
#if DT_NODE_HAS_STATUS(DT_NODELABEL(iic1), okay)
#define PV_HAVE_I2C 1
static const struct device *const pv_i2c[] = {
	DEVICE_DT_GET(DT_NODELABEL(iic1)),
};
#else
#define PV_HAVE_I2C 0
static const struct device *const pv_i2c[] = { NULL };
#endif

/* Logical GPIO pins. The first five are the board's own LEDs and buttons, so
 * the bridge can be exercised with no wiring at all; the last three are the
 * uncommitted mikroBUS signals, which are the only header pins on this board
 * not already claimed by an enabled pinctrl group. To add a pin, append a row
 * here and to pv_gpio_name[] -- nothing else needs to change. */
#define PV_GPIO_RAW(nodelabel, n) \
	{ .port = DEVICE_DT_GET(DT_NODELABEL(nodelabel)), .pin = (n), .dt_flags = 0 }

static const struct gpio_dt_spec pv_gpio[] = {
	GPIO_DT_SPEC_GET(DT_NODELABEL(led1), gpios),     /* 0  LED1   P600 */
	GPIO_DT_SPEC_GET(DT_NODELABEL(led2), gpios),     /* 1  LED2   P414 */
	GPIO_DT_SPEC_GET(DT_NODELABEL(led3), gpios),     /* 2  LED3   P107 */
	GPIO_DT_SPEC_GET(DT_NODELABEL(button0), gpios),  /* 3  SW1    P009 */
	GPIO_DT_SPEC_GET(DT_NODELABEL(button1), gpios),  /* 4  SW2    P008 */
	PV_GPIO_RAW(ioport0, 10),                        /* 5  mikroBUS INT */
	PV_GPIO_RAW(ioport9, 7),                         /* 6  mikroBUS PWM */
	PV_GPIO_RAW(ioport5, 7),                         /* 7  mikroBUS RST */
};

static const char *const pv_gpio_name[] = {
	"LED1", "LED2", "LED3", "SW1", "SW2", "MB_INT", "MB_PWM", "MB_RST",
};

#define PV_NGPIO  ARRAY_SIZE(pv_gpio)
#define PV_NI2C   (PV_HAVE_I2C ? ARRAY_SIZE(pv_i2c) : 0)

/* ---- device state ---- */

static uint32_t pv_status;
static uint32_t pv_result;
static uint32_t pv_i2c_bus;
static uint32_t pv_i2c_addr;
static uint32_t pv_i2c_wlen;
static uint32_t pv_i2c_rlen;
static uint32_t pv_gpio_pin;
static uint32_t pv_gpio_val;
static uint32_t pv_gpio_flags;
static uint32_t pv_data[PV_DATA_WORDS];

static gpio_flags_t pv_map_gpio_flags(uint32_t f)
{
	gpio_flags_t z = 0;

	if (f & PV_GPIO_DISCONNECT) {
		return GPIO_DISCONNECTED;
	}
	if (f & PV_GPIO_INPUT) {
		z |= GPIO_INPUT;
	}
	if (f & PV_GPIO_OUTPUT) {
		z |= GPIO_OUTPUT;
	}
	if (f & PV_GPIO_PULL_UP) {
		z |= GPIO_PULL_UP;
	}
	if (f & PV_GPIO_PULL_DOWN) {
		z |= GPIO_PULL_DOWN;
	}
	if (f & PV_GPIO_OPEN_DRAIN) {
		z |= GPIO_OPEN_DRAIN;
	}
	if (f & PV_GPIO_INIT_HIGH) {
		z |= GPIO_OUTPUT_INIT_HIGH;
	}
	if (f & PV_GPIO_INIT_LOW) {
		z |= GPIO_OUTPUT_INIT_LOW;
	}

	return z;
}

/* Run one command. Returns a Zephyr-style errno, which lands in STATUS. */
static int pv_exec(uint32_t cmd)
{
	uint8_t *buf = (uint8_t *)pv_data;

	pv_result = 0;

	switch (cmd) {
	case PV_CMD_NOP:
		return 0;

	case PV_CMD_I2C_WRITE:
	case PV_CMD_I2C_READ:
	case PV_CMD_I2C_WRITE_READ:
	case PV_CMD_I2C_PROBE: {
		if (PV_NI2C == 0 || pv_i2c_bus >= PV_NI2C) {
			return -ENODEV;
		}

		const struct device *dev = pv_i2c[pv_i2c_bus];

		if (dev == NULL || !device_is_ready(dev)) {
			return -ENODEV;
		}
		if (pv_i2c_addr > 0x7f) {
			return -EINVAL;
		}

		uint16_t addr = (uint16_t)pv_i2c_addr;

		if (cmd == PV_CMD_I2C_PROBE) {
			/* Same shape as Zephyr's own `i2c scan`: a 1-byte read.
			 * A missing target NAKs its address and the driver
			 * reports -EIO, which is the answer we want. */
			uint8_t discard;

			return i2c_read(dev, &discard, 1, addr);
		}

		/* The write payload and the read payload share the DATA
		 * window. For WRITE_READ the read must not start before the
		 * write has been consumed, and i2c_write_read() does exactly
		 * that, so overlapping them in one buffer is safe -- but only
		 * if together they fit. */
		if (pv_i2c_wlen > PV_DATA_BYTES || pv_i2c_rlen > PV_DATA_BYTES) {
			return -EINVAL;
		}

		if (cmd == PV_CMD_I2C_WRITE) {
			int rc = i2c_write(dev, buf, pv_i2c_wlen, addr);

			if (rc == 0) {
				pv_result = pv_i2c_wlen;
			}
			return rc;
		}
		if (cmd == PV_CMD_I2C_READ) {
			int rc = i2c_read(dev, buf, pv_i2c_rlen, addr);

			if (rc == 0) {
				pv_result = pv_i2c_rlen;
			}
			return rc;
		}

		/* WRITE_READ: the read lands at the start of DATA, clobbering
		 * the command bytes we just sent. Callers expect that. */
		if (pv_i2c_wlen + pv_i2c_rlen > PV_DATA_BYTES) {
			return -EINVAL;
		}

		int rc = i2c_write_read(dev, addr, buf, pv_i2c_wlen,
					buf + pv_i2c_wlen, pv_i2c_rlen);

		if (rc == 0) {
			/* Move the reply down to offset 0 so the guest does not
			 * have to know where the write ended. */
			memmove(buf, buf + pv_i2c_wlen, pv_i2c_rlen);
			pv_result = pv_i2c_rlen;
		}
		return rc;
	}

	case PV_CMD_GPIO_CONFIG:
	case PV_CMD_GPIO_SET:
	case PV_CMD_GPIO_GET:
	case PV_CMD_GPIO_TOGGLE: {
		if (pv_gpio_pin >= PV_NGPIO) {
			return -EINVAL;
		}

		const struct gpio_dt_spec *sp = &pv_gpio[pv_gpio_pin];

		if (!gpio_is_ready_dt(sp)) {
			return -ENODEV;
		}

		switch (cmd) {
		case PV_CMD_GPIO_CONFIG:
			return gpio_pin_configure_dt(sp,
						     pv_map_gpio_flags(pv_gpio_flags));
		case PV_CMD_GPIO_SET:
			return gpio_pin_set_dt(sp, pv_gpio_val ? 1 : 0);
		case PV_CMD_GPIO_TOGGLE:
			return gpio_pin_toggle_dt(sp);
		default: {
			int v = gpio_pin_get_dt(sp);

			if (v < 0) {
				return v;
			}
			pv_gpio_val = (uint32_t)v;
			pv_result = (uint32_t)v;
			return 0;
		}
		}
	}

	default:
		return -ENOTSUP;
	}
}

static void pv_write(uint32_t off, uint32_t val)
{
	if (off >= PV_REG_DATA) {
		uint32_t i = (off - PV_REG_DATA) >> 2;

		if (i < PV_DATA_WORDS) {
			pv_data[i] = val;
		}
		return;
	}

	switch (off) {
	case PV_REG_CMD:
		pv_status = (uint32_t)pv_exec(val);
		break;
	case PV_REG_I2C_BUS:
		pv_i2c_bus = val;
		break;
	case PV_REG_I2C_ADDR:
		pv_i2c_addr = val;
		break;
	case PV_REG_I2C_WLEN:
		pv_i2c_wlen = val;
		break;
	case PV_REG_I2C_RLEN:
		pv_i2c_rlen = val;
		break;
	case PV_REG_GPIO_PIN:
		pv_gpio_pin = val;
		break;
	case PV_REG_GPIO_VAL:
		pv_gpio_val = val;
		break;
	case PV_REG_GPIO_FLAG:
		pv_gpio_flags = val;
		break;
	default:
		break;   /* read-only or unassigned: writes are dropped */
	}
}

static uint32_t pv_read(uint32_t off)
{
	if (off >= PV_REG_DATA) {
		uint32_t i = (off - PV_REG_DATA) >> 2;

		return (i < PV_DATA_WORDS) ? pv_data[i] : 0;
	}

	switch (off) {
	case PV_REG_ID:
		return PV_ID_MAGIC;
	case PV_REG_VERSION:
		return PV_VERSION;
	case PV_REG_CAPS:
		return (uint32_t)PV_NI2C | ((uint32_t)PV_NGPIO << 8) |
		       ((uint32_t)PV_DATA_BYTES << 16);
	case PV_REG_STATUS:
		return pv_status;
	case PV_REG_RESULT:
		return pv_result;
	case PV_REG_I2C_BUS:
		return pv_i2c_bus;
	case PV_REG_I2C_ADDR:
		return pv_i2c_addr;
	case PV_REG_I2C_WLEN:
		return pv_i2c_wlen;
	case PV_REG_I2C_RLEN:
		return pv_i2c_rlen;
	case PV_REG_GPIO_PIN:
		return pv_gpio_pin;
	case PV_REG_GPIO_VAL:
		return pv_gpio_val;
	case PV_REG_GPIO_FLAG:
		return pv_gpio_flags;
	default:
		return 0;
	}
}

/* ------------------------------------------------------- emulated devices */

/* 8250 UART at 0x10000000, CLINT at 0x11000000, syscon at 0x11100000.
 * These addresses come from the DTB compiled into default64mbdtc.h.
 * The paravirt bridge at 0x11200000 is checked last: it is the coldest of the
 * four and the UART is by far the hottest. */
static uint32_t mmio_store(uint32_t addy, uint32_t val)
{
	if (addy == 0x10000000) {          /* UART TX */
		guest_out((char)val);
	} else if (addy == 0x11004004) {   /* CLINT timermatch high */
		core->timermatchh = val;
	} else if (addy == 0x11004000) {   /* CLINT timermatch low */
		core->timermatchl = val;
	} else if (addy == 0x11100000) {   /* syscon: reboot / poweroff */
		core->pc = core->pc + 4;
		return val;
	} else if ((addy & PV_MASK) == PV_BASE) {
		pv_write(addy & ~PV_MASK, val);
	}
	return 0;
}

static uint32_t mmio_load(uint32_t addy)
{
	if (addy == 0x10000005) {          /* UART LSR: THR empty | RX ready */
		return 0x60 | (guest_rx_poll() >= 0 ? 1 : 0);
	} else if (addy == 0x10000000) {   /* UART RX */
		int c = guest_rx_take();

		return (c >= 0) ? (uint32_t)c : 0;
	} else if (addy == 0x1100bffc) {
		return core->timerh;
	} else if (addy == 0x1100bff8) {
		return core->timerl;
	} else if ((addy & PV_MASK) == PV_BASE) {
		return pv_read(addy & ~PV_MASK);
	}
	return 0;
}

/* ------------------------------------------------------- paravirt selftest */

/*
 * Exercise the bridge from the host before the guest ever runs, going through
 * mmio_store()/mmio_load() rather than calling pv_write()/pv_read() directly.
 * That way the address decode, the register file and the Zephyr calls are all
 * on the same path the guest will take, and a pass here means the only thing
 * left to prove is the guest's own access width.
 *
 * Output goes to the console before the guest boots, so it is visible on the
 * next flash without needing a guest-side program to exist yet.
 */

static void pv_w(uint32_t off, uint32_t v)
{
	mmio_store(PV_BASE + off, v);
}

static uint32_t pv_r(uint32_t off)
{
	return mmio_load(PV_BASE + off);
}

static void pv_st(void)
{
	int32_t s = (int32_t)pv_r(PV_REG_STATUS);

	if (s == 0) {
		us("ok");
	} else {
		us("err -");
		udec((uint32_t)(-s));
	}
}

static void pv_selftest(void)
{
	uint32_t caps;
	int fails = 0;

	us("\r\n--- paravirt I/O selftest @ 0x");
	uhex(PV_BASE);
	us(" ---\r\n");

	/* 1. Identity. Proves the decode reaches us at all. */
	uint32_t id = pv_r(PV_REG_ID);

	caps = pv_r(PV_REG_CAPS);
	us("id 0x");
	uhex(id);
	us(id == PV_ID_MAGIC ? " ok" : " BAD");
	us(", ver ");
	udec(pv_r(PV_REG_VERSION));
	us(", i2c buses ");
	udec(caps & 0xff);
	us(", gpio pins ");
	udec((caps >> 8) & 0xff);
	us(", data ");
	udec(caps >> 16);
	us(" B\r\n");
	fails += (id != PV_ID_MAGIC);

	/* 2. Register and DATA round-trip. */
	pv_w(PV_REG_I2C_ADDR, 0x5a);
	pv_w(PV_REG_DATA + 3 * 4, 0xdeadbeef);
	pv_w(PV_REG_DATA + 63 * 4, 0x01020304);

	int rt = (pv_r(PV_REG_I2C_ADDR) == 0x5a) &&
		 (pv_r(PV_REG_DATA + 3 * 4) == 0xdeadbeef) &&
		 (pv_r(PV_REG_DATA + 63 * 4) == 0x01020304) &&
		 (pv_r(PV_REG_DATA + 2 * 4) == 0);

	us("regs+data round-trip ");
	us(rt ? "ok" : "FAIL");
	us("\r\n");
	fails += !rt;

	/* Writes to a read-only register must be dropped, not aliased onto
	 * something else. */
	pv_w(PV_REG_ID, 0);
	fails += (pv_r(PV_REG_ID) != PV_ID_MAGIC);

	/* 3. GPIO. LEDs get driven high then low so the result is visible on
	 * the board as well as on the console; inputs are just read. */
	for (uint32_t i = 0; i < PV_NGPIO; i++) {
		uint32_t is_out = (i <= 2);

		pv_w(PV_REG_GPIO_PIN, i);
		pv_w(PV_REG_GPIO_FLAG, is_out ? (PV_GPIO_OUTPUT | PV_GPIO_INIT_LOW)
					      : (PV_GPIO_INPUT | PV_GPIO_PULL_UP));
		pv_w(PV_REG_CMD, PV_CMD_GPIO_CONFIG);

		us("  gpio ");
		udec(i);
		us(" ");
		us(pv_gpio_name[i]);
		us(is_out ? " out cfg " : " in  cfg ");
		pv_st();

		if ((int32_t)pv_r(PV_REG_STATUS) != 0) {
			fails++;
			us("\r\n");
			continue;
		}

		if (is_out) {
			pv_w(PV_REG_GPIO_VAL, 1);
			pv_w(PV_REG_CMD, PV_CMD_GPIO_SET);
		}

		pv_w(PV_REG_CMD, PV_CMD_GPIO_GET);
		us(" read ");
		udec(pv_r(PV_REG_GPIO_VAL));
		us(" ");
		pv_st();
		us("\r\n");
		fails += ((int32_t)pv_r(PV_REG_STATUS) != 0);
	}

	/* Leave the LEDs on briefly, then off, so a human watching the board
	 * gets an unambiguous signal that the bridge reached real hardware. */
	k_msleep(150);
	for (uint32_t i = 0; i <= 2; i++) {
		pv_w(PV_REG_GPIO_PIN, i);
		pv_w(PV_REG_GPIO_VAL, 0);
		pv_w(PV_REG_CMD, PV_CMD_GPIO_SET);
	}

	/* 4. I2C bus scan. Bounded by a deadline: if the bus has no pull-ups
	 * fitted, every address costs a hardware timeout, and this must not
	 * turn a 2.4 s boot into a minute of silence. */
	if ((caps & 0xff) == 0) {
		us("  i2c: no controller enabled (check app.overlay)\r\n");
	} else {
		int64_t t0 = k_uptime_get();
		int64_t deadline = t0 + 3000;
		int found = 0;
		uint32_t scanned = 0;

		us("  i2c scan 0x08-0x77:");
		pv_w(PV_REG_I2C_BUS, 0);

		for (uint32_t a = 0x08; a <= 0x77; a++) {
			if (k_uptime_get() > deadline) {
				us(" [aborted at 0x");
				uhex(a);
				us("]");
				break;
			}
			pv_w(PV_REG_I2C_ADDR, a);
			pv_w(PV_REG_CMD, PV_CMD_I2C_PROBE);
			scanned++;
			if ((int32_t)pv_r(PV_REG_STATUS) == 0) {
				us(" 0x");
				up("0123456789abcdef"[(a >> 4) & 0xf]);
				up("0123456789abcdef"[a & 0xf]);
				found++;
			}
		}

		if (found == 0) {
			us(" none");
		}
		us(" (");
		udec(scanned);
		us(" probed in ");
		udec((uint32_t)(k_uptime_get() - t0));
		us(" ms)\r\n");
	}

	/* An unknown command must be rejected rather than silently ignored. */
	pv_w(PV_REG_CMD, 0xff);
	fails += ((int32_t)pv_r(PV_REG_STATUS) != -ENOTSUP);

	us("--- selftest ");
	if (fails == 0) {
		us("PASS ---\r\n\r\n");
	} else {
		udec((uint32_t)fails);
		us(" FAILURES ---\r\n\r\n");
	}
}

/* ------------------------------------------------------------------ timer */

/* The guest's timebase is declared as 1 MHz in the DTB, so the emulator wants
 * elapsed microseconds. k_cycle_get_32() wraps every ~8.9 s at 480 MHz, which
 * is far longer than one step batch, so accumulate into 64 bits as we go. */
static uint32_t cyc_last;
static uint64_t cyc_total;

static uint64_t now_us(void)
{
	uint32_t c = k_cycle_get_32();

	cyc_total += (uint32_t)(c - cyc_last);
	cyc_last = c;

	return cyc_total / (uint64_t)(sys_clock_hw_cycles_per_sec() / 1000000U);
}

/* ------------------------------------------------------------- image load */

static const struct device *const nor = DEVICE_DT_GET(DT_NODELABEL(s28hl512t));

/* Longest payload this slot accepts.
 *
 * The kernel slot has a second ceiling: guest_prepare() copies it to guest
 * offset 0 and puts the DTB and emulator state at the top of SDRAM, so an
 * over-long kernel would be memcpy'd straight over them. The rootfs slot is
 * never copied anywhere, so only the flash bound applies to it. */
static uint32_t img_max_len(int idx)
{
	uint32_t cap = img_slots[idx].size - IMG_PAYLOAD_OFF;

	if (idx == SLOT_KERNEL && cap > IMG_MAX_RAM_LEN) {
		cap = IMG_MAX_RAM_LEN;
	}
	return cap;
}

/* Validate what is stored in a slot.
 *
 * `verify` controls the CRC pass, which is the expensive part: it reads the
 * whole payload through the OSPI window, and at rootfs sizes that is seconds
 * of every boot. The kernel is checked in full because it is what we are about
 * to execute; the rootfs is header-checked at boot and CRC'd only at push
 * time, where it is one pass against a transfer that already took minutes. */
static int img_check(int idx, uint32_t *len_out, bool verify)
{
	const struct img_slot *s = &img_slots[idx];
	const struct img_hdr *h = (const struct img_hdr *)SLOT_MMAP(s);
	const uint8_t *body = (const uint8_t *)(SLOT_MMAP(s) + IMG_PAYLOAD_OFF);

	if (memcmp(h->magic, s->magic, 8) != 0) {
		return -1;
	}
	if (h->len == 0 || h->len > img_max_len(idx)) {
		return -2;
	}
	if (verify && crc32_ieee(body, h->len) != h->crc) {
		return -3;
	}

	*len_out = h->len;
	return 0;
}

/* Explain a failed verify.
 *
 * "verify FAILED" on its own says almost nothing: the payload was sent, the
 * writes all returned success, and the readback disagrees. That is consistent
 * with several very different faults, and this exists to tell them apart in
 * one run rather than one per night.
 *
 * Two measurements do the discriminating.
 *
 * Erased blocks. A 64 KB block that is still entirely 0xFF was never
 * programmed. Where the first one starts is the answer to "how far did the
 * writes actually get", which is a different question from "how far did the
 * transfer get" and the two have never been distinguishable before.
 *
 * Read stability. The payload is CRC'd twice, once as the cache happens to
 * hold it and once after an explicit invalidate. Those two agreeing rules out
 * a stale-read fault; them disagreeing proves one, and that matters here
 * because there are two caches in this path and we can only flush one of
 * them. sys_cache_data_invd_range() clears the CPU D-cache, but the OSPI-B
 * bridge has its own hardware read prefetch (OSPI_B_CFG_PREFETCH_FUNCTION=1
 * in the FSP config). R_OSPI_B_Erase() flushes that prefetch buffer
 * explicitly; R_OSPI_B_Write() does not, and nothing in the flash API exposes
 * a way for us to. So a stale prefetch after a write is a fault this code
 * cannot prevent -- but it can prove.
 *
 * How to read the output:
 *   first-erased at N, tail all 0xFF  -> writes stopped landing at N. Compare
 *                                        N to where the transfer stalled and
 *                                        to the 16 MB mark.
 *   pass1 != pass2                    -> stale read. CPU D-cache or, more
 *                                        likely, the OSPI-B prefetch buffer.
 *   no erased blocks, stable, bad crc -> the bytes landed and are wrong:
 *                                        wrong address, or corrupted in
 *                                        transit.
 *   every block erased                -> nothing was programmed at all.
 */
#define IMG_DIAG_BLK (64U * 1024U)

static void img_diagnose(int idx, uint32_t len)
{
	const struct img_slot *s = &img_slots[idx];
	const struct img_hdr *h = (const struct img_hdr *)SLOT_MMAP(s);
	const uint8_t *body = (const uint8_t *)(SLOT_MMAP(s) + IMG_PAYLOAD_OFF);
	uint32_t nblk = (len + IMG_DIAG_BLK - 1) / IMG_DIAG_BLK;
	uint32_t erased_blocks = 0;
	uint32_t first_erased = UINT32_MAX;
	uint32_t unstable_blocks = 0;
	uint32_t first_unstable = UINT32_MAX;
	uint32_t crc1, crc2;

	us("DIAG: slot ");
	us(s->name);
	us(" off=0x");
	uhex(s->off);
	us(" payload=0x");
	uhex(s->off + IMG_PAYLOAD_OFF);
	us(" len=");
	udec(len);
	us("\r\n");

	us("DIAG: stored header magic=");
	for (int i = 0; i < 8; i++) {
		up(h->magic[i] >= 0x20 && h->magic[i] < 0x7f ? h->magic[i] : '?');
	}
	us(" len=");
	udec(h->len);
	us(" crc=");
	uhex(h->crc);
	us("\r\n");

	for (uint32_t b = 0; b < nblk; b++) {
		uint32_t off = b * IMG_DIAG_BLK;
		uint32_t n = MIN(IMG_DIAG_BLK, len - off);
		bool all_ff = true;

		for (uint32_t i = 0; i < n; i++) {
			if (body[off + i] != 0xFF) {
				all_ff = false;
				break;
			}
		}
		if (all_ff) {
			erased_blocks++;
			if (first_erased == UINT32_MAX) {
				first_erased = off;
			}
		}

		/* Read the same block twice with an invalidate between, so an
		 * unstable read is localised rather than merely detected. A
		 * marginal interface shows up here as a scatter of unstable
		 * blocks; a coherency fault shows up as a contiguous run. */
		uint32_t ca = crc32_ieee(body + off, n);

		sys_cache_data_invd_range((void *)(SLOT_MMAP(s) + IMG_PAYLOAD_OFF + off),
					  ROUND_UP(n, 32U));

		if (crc32_ieee(body + off, n) != ca) {
			unstable_blocks++;
			if (first_unstable == UINT32_MAX) {
				first_unstable = off;
			}
		}
	}

	us("DIAG: ");
	udec(unstable_blocks);
	us(" of ");
	udec(nblk);
	us(" blocks read differently on a second pass");
	if (first_unstable != UINT32_MAX) {
		us(", first at payload 0x");
		uhex(first_unstable);
		us(" = flash 0x");
		uhex(s->off + IMG_PAYLOAD_OFF + first_unstable);
	}
	us("\r\n");

	us("DIAG: ");
	udec(erased_blocks);
	us(" of ");
	udec(nblk);
	us(" 64K blocks still erased (0xFF)");
	if (first_erased != UINT32_MAX) {
		us(", first at payload 0x");
		uhex(first_erased);
		us(" = flash 0x");
		uhex(s->off + IMG_PAYLOAD_OFF + first_erased);
	}
	us("\r\n");

	/* Same bytes, twice, with a cache invalidate in between. */
	crc1 = crc32_ieee(body, len);
	sys_cache_data_invd_range((void *)SLOT_MMAP(s),
				  ROUND_UP(len + IMG_PAYLOAD_OFF, OSPI_ERASE_SZ));
	crc2 = crc32_ieee(body, len);

	us("DIAG: readback crc pass1=");
	uhex(crc1);
	us(" pass2=");
	uhex(crc2);
	us(crc1 == crc2 ? " stable\r\n" : " UNSTABLE -- stale read\r\n");
}

/* ---- the three steps every push shares, whatever transport carried it ----
 *
 * Both the UART loader and the TCP service go through these, so the ordering
 * that matters -- bound the length before erasing anything, payload first,
 * header last -- is written once and cannot drift between the two.
 */

/* Bound a length off the wire. Called before a single block is erased: the
 * length arrives as four raw bytes with no framing behind it, so a desynced or
 * wrong-endian host can present any 32-bit value at all, and ROUND_UP() in
 * img_erase() would wrap. Rejecting here also means a bad push leaves the
 * image already in the slot intact. */
static int img_accept_len(int idx, uint32_t len)
{
	if (len == 0 || len > img_max_len(idx)) {
		us("LOADER: ");
		us(img_slots[idx].name);
		us(" length out of range, max ");
		udec(img_max_len(idx));
		us("\r\n");
		return -1;
	}
	return 0;
}

/* Erase enough 256 KB blocks for header + payload, one block per call.
 *
 * The driver's loop is the same either way, but a 45 MB rootfs is 172 blocks
 * and well over a minute; erasing a block at a time is what lets us emit
 * progress instead of going silent for that long, which on the host side is
 * indistinguishable from a hung board. */
static int img_erase(int idx, uint32_t len, void (*tick)(void))
{
	const struct img_slot *s = &img_slots[idx];
	uint32_t need = ROUND_UP(len + IMG_PAYLOAD_OFF, OSPI_ERASE_SZ);

	if (!device_is_ready(nor)) {
		us("LOADER: NOR not ready\r\n");
		return -1;
	}

	for (uint32_t e = 0; e < need; e += OSPI_ERASE_SZ) {
		if (flash_erase(nor, s->off + e, OSPI_ERASE_SZ) != 0) {
			us("\r\nLOADER: erase failed at 0x");
			uhex(s->off + e);
			us("\r\n");
			return -1;
		}
		if (tick) {
			tick();
		}
	}

	/* Blank-check what we just erased.
	 *
	 * The erase path reports success on a status bit, and nothing verifies
	 * that the bits actually went to 1. That gap matters because a NOR
	 * program can only clear bits: programming over a block that erased
	 * incompletely yields the bitwise AND of old and new, which is data
	 * that is present, reads back identically every time, and is wrong.
	 * That is indistinguishable, after the fact, from a bad write -- the
	 * payload diagnostic cannot tell the two apart, because by then
	 * everything has been programmed over.
	 *
	 * Checking here, before a single payload byte is written, separates
	 * them completely. It costs one read pass over the erased region.
	 */
	if (IS_ENABLED(CONFIG_RVL_BLANK_CHECK)) {
		const uint32_t *p = (const uint32_t *)SLOT_MMAP(s);
		int64_t t0 = k_uptime_get();
		uint32_t bad = 0, first_bad = UINT32_MAX;

		sys_cache_data_invd_range((void *)SLOT_MMAP(s), need);

		for (uint32_t w = 0; w < need / 4; w++) {
			if (p[w] != 0xFFFFFFFFU) {
				bad++;
				if (first_bad == UINT32_MAX) {
					first_bad = w * 4;
				}
			}
		}

		if (bad) {
			us("\r\nLOADER: ERASE INCOMPLETE -- ");
			udec(bad);
			us(" of ");
			udec(need / 4);
			us(" words not 0xFFFFFFFF, first at slot+0x");
			uhex(first_bad);
			us(" = flash 0x");
			uhex(s->off + first_bad);
			us("\r\n");
			return -1;
		}

		us("\r\nLOADER: blank check ok in ");
		udec((uint32_t)(k_uptime_get() - t0));
		us(" ms\r\n");
	}

	return 0;
}

/* Write the header, then verify the slot by reading it back.
 *
 * The header goes last on purpose: until it lands the slot has no valid magic,
 * so a transfer that died halfway through the payload leaves a slot that fails
 * img_check() rather than one that looks valid and is not. */
static int img_commit(int idx, uint32_t len, uint32_t crc)
{
	const struct img_slot *s = &img_slots[idx];
	static uint8_t hdrblk[IMG_PAYLOAD_OFF];
	struct img_hdr h;
	uint32_t vlen;

	memset(hdrblk, 0xFF, sizeof(hdrblk));
	memcpy(h.magic, s->magic, 8);
	h.len = len;
	h.crc = crc;
	memcpy(hdrblk, &h, sizeof(h));

	int wrc = flash_write(nor, s->off, hdrblk, IMG_PAYLOAD_OFF);

	if (wrc != 0) {
		us("\r\nLOADER: header write failed rc=");
		udec((uint32_t)(-wrc));
		us(" wbs=");
		udec((uint32_t)flash_get_write_block_size(nor));
		us("\r\n");
		return -1;
	}

	/* The memory-mapped window is cacheable, so invalidate before verifying
	 * through it or we may read pre-erase lines. */
	sys_cache_data_invd_range((void *)SLOT_MMAP(s),
				  ROUND_UP(len + IMG_PAYLOAD_OFF, OSPI_ERASE_SZ));

	/* Verify more than once before giving up.
	 *
	 * A CRC32 that matches is worth trusting however many attempts it took
	 * -- matching by chance is one in four billion -- so if any pass agrees
	 * the payload in flash is right and an earlier disagreeing pass was a
	 * bad read, not bad data. Retrying costs a few seconds on a push that
	 * already took minutes.
	 *
	 * The attempt count is the real product here. If pushes routinely need
	 * a second or third pass, reads are marginal, and that is a far more
	 * specific finding than "verify failed".
	 *
	 * Be clear about what this does NOT do: it makes a marginal read
	 * survivable *in the loader only*. If reads really are marginal then
	 * guest_prepare()'s memcpy out of the same window at boot is exposed to
	 * exactly the same fault, and nothing here helps that.
	 */
	int vrc = -1;
	int attempt;

	for (attempt = 1; attempt <= CONFIG_RVL_VERIFY_ATTEMPTS; attempt++) {
		vrc = img_check(idx, &vlen, true);
		if (vrc == 0 && vlen == len) {
			break;
		}
		if (attempt < CONFIG_RVL_VERIFY_ATTEMPTS) {
			sys_cache_data_invd_range(
				(void *)SLOT_MMAP(s),
				ROUND_UP(len + IMG_PAYLOAD_OFF, OSPI_ERASE_SZ));
		}
	}

	if (vrc == 0 && vlen == len && attempt > 1) {
		us("LOADER: verified on attempt ");
		udec((uint32_t)attempt);
		us(" -- earlier passes disagreed, reads look marginal\r\n");
	}

	if (vrc != 0 || vlen != len) {
		us("\r\nLOADER: verify FAILED rc=");
		udec((uint32_t)(-vrc));
		us(vrc == -1 ? " (magic)" : vrc == -2 ? " (length)" : " (crc)");
		us(" expected len=");
		udec(len);
		us(" crc=");
		uhex(crc);
		us("\r\n");
		img_diagnose(idx, len);
		return -1;
	}
	return 0;
}

/* Ask whether the operator wants the loader even though the stored image is
 * fine. Without this the only way to replace a valid image is to corrupt the
 * one already in flash, which is a silly thing to have to do on purpose.
 *
 * Two properties make this safe to leave enabled always. A timeout falls
 * through to the normal boot, so a missed key costs one boot and never leaves
 * the board parked at a prompt. And bytes other than the trigger are consumed
 * and ignored rather than ending the wait, so a terminal that emits a newline
 * on open neither forces the loader nor eats the window.
 *
 * Deliberately UART-only, like rx_block(): the telnet listener is already up
 * by the time this runs, and a client that reconnects on boot must not be able
 * to divert the board into the loader.
 */
static bool loader_requested(void)
{
	int64_t end = k_uptime_get() + CONFIG_RVL_LOADER_PROMPT_MS;
	int c;

	if (CONFIG_RVL_LOADER_PROMPT_MS == 0) {
		return false;
	}

	us("press 'L' within ");
	udec(CONFIG_RVL_LOADER_PROMPT_MS);
	us(" ms for the image loader... ");

	rx_have = -1;
	do {
		c = uart_rx_take();
		if (c == 'L' || c == 'l') {
			us("loader\r\n");
			return true;
		}
	} while (k_uptime_get() < end);

	us("boot\r\n");
	return false;
}

static void uart_erase_tick(void)
{
	up('.');
}

/* Receive an image over the console UART and commit it to a slot.
 *
 * Protocol, host -> board:
 *   'S', slot (1 byte), length (4 B LE), crc32 (4 B LE), then raw bytes.
 * Board acknowledges each 4 KB block with 'K' so the host cannot outrun the
 * flash writes.
 *
 * This is the recovery path, not the fast one. The console runs at 921600 (see
 * app.overlay), which floors a 45 MB push at ~8 minutes before flash time, and
 * the block ACK serialises reception against programming on top of that. Use
 * the TCP service below for real pushes; this is what remains reachable when
 * the network does not come up, and it is the only loader available before the
 * stack is running.
 */
static int img_receive(void)
{
	static uint8_t buf[4096];
	uint32_t len, crc, off = 0;
	int slot, c;

	us("\r\nLOADER: waiting for image ('S' + slot8 + len32le + crc32le + data)\r\n");

	do {
		c = rx_block(K_SECONDS(600));
		if (c < 0) {
			us("LOADER: timeout\r\n");
			return -1;
		}
	} while (c != 'S');

	slot = rx_block(K_SECONDS(5));
	if (slot < 0) {
		return -1;
	}
	if (slot >= (int)IMG_NSLOTS) {
		us("LOADER: bad slot\r\n");
		return -1;
	}

	len = 0;
	for (int i = 0; i < 4; i++) {
		c = rx_block(K_SECONDS(5));
		if (c < 0) {
			return -1;
		}
		len |= (uint32_t)c << (8 * i);
	}
	crc = 0;
	for (int i = 0; i < 4; i++) {
		c = rx_block(K_SECONDS(5));
		if (c < 0) {
			return -1;
		}
		crc |= (uint32_t)c << (8 * i);
	}

	us("LOADER: slot=");
	us(img_slots[slot].name);
	us(" len=");
	udec(len);
	us(" crc=");
	uhex(crc);
	us("\r\n");

	if (img_accept_len(slot, len) != 0) {
		return -1;
	}

	uint32_t need = ROUND_UP(len + IMG_PAYLOAD_OFF, OSPI_ERASE_SZ);

	us("LOADER: erasing ");
	udec(need / 1024);
	us(" KB (");
	udec(need / OSPI_ERASE_SZ);
	us(" blocks)\r\n");

	int64_t t_erase = k_uptime_get();

	if (img_erase(slot, len, uart_erase_tick) != 0) {
		return -1;
	}

	us("\r\nLOADER: erased in ");
	udec((uint32_t)(k_uptime_get() - t_erase));
	us(" ms\r\n");

	/* Erasing several MB of NOR takes tens of seconds, and this loader does
	 * not buffer the UART. Anything the host sends during the erase is lost
	 * in the FIFO. Tell the host explicitly when we are ready to receive,
	 * and drop whatever arrived early. */
	rx_have = -1;
	while (uart_rx_take() >= 0) {
		/* drain */
	}
	us("\r\n<RDY>\r\n");

	/* Payload first, header last, so a truncated transfer never leaves a
	 * valid-looking header pointing at a partial image. */
	while (off < len) {
		uint32_t n = MIN((uint32_t)sizeof(buf), len - off);

		for (uint32_t i = 0; i < n; i++) {
			c = rx_block(K_SECONDS(10));
			if (c < 0) {
				us("LOADER: rx timeout at ");
				udec(off + i);
				us("\r\n");
				return -1;
			}
			buf[i] = (uint8_t)c;
		}

		if (flash_write(nor, img_slots[slot].off + IMG_PAYLOAD_OFF + off,
				buf, n) != 0) {
			us("LOADER: write failed\r\n");
			return -1;
		}
		off += n;
		up('K');
	}

	if (img_commit(slot, len, crc) != 0) {
		return -1;
	}

	us("\r\nLOADER: image committed and verified\r\n");
	return 0;
}

/* ------------------------------------------------------- image load: TCP */

#ifdef CONFIG_RVL_NETLOAD

/*
 * The same push, over a socket, as a service that is always listening.
 *
 * Why this is a thread rather than another boot-time prompt: the network stack
 * is not up at the point the UART loader runs, so a TCP loader cannot live
 * there at all. Running it as a peer of the telnet thread turns out to be the
 * better shape anyway -- the board stays booted and serving its guest, a push
 * lands in the *other* half of flash while that guest keeps running, and the
 * new image takes effect at the next reset. There is no two-second window to
 * catch, and a push that fails leaves the running system untouched.
 *
 * Two things make writing flash underneath a live guest safe TODAY:
 *
 *   1. guest_prepare() copies the image out of OSPI into SDRAM at boot, so
 *      once the guest is running nothing reads the OSPI window at all. The
 *      guest's RAM is SDRAM; the flash is not in its path.
 *   2. The flash driver serialises every operation on its own semaphore, and
 *      this is the only writer.
 *
 * *** BOTH OF THOSE STOP HOLDING THE MOMENT THE GUEST READS FLASH DIRECTLY, ***
 * which is exactly where the XIP / virtio-blk rootfs work is heading. An
 * OSPI-B device cannot serve memory-mapped reads while a program or erase is
 * in progress, so a push would corrupt every guest read racing it -- silently,
 * because the mapped window has no way to report a fault. When that lands, one
 * of these has to be true instead:
 *
 *   - A/B slots: push to the inactive copy, switch at reset. Cheapest here,
 *     since slots already exist; it costs flash, not code.
 *   - Quiesce the guest for the duration: stop the emulator thread before the
 *     erase and restart it after the commit. Simple, but freezes the guest for
 *     the whole multi-minute push.
 *   - Stage to SDRAM and commit at reset, which needs RAM we do not have at
 *     rootfs sizes.
 * Whichever is chosen, the D-cache invalidate in img_commit() also has to cover
 * every region the guest may have cached, not just the slot being written.
 *
 * Protocol, host -> board, identical to the UART loader's:
 *   'S', slot (1 byte), length (4 B LE), crc32 (4 B LE), then the payload.
 * There is no per-block ACK. TCP is already reliable and already flow
 * controlled, and the ACK was what serialised reception against programming;
 * dropping it is most of the point of this path. The board replies with a
 * single status line per phase so the host can report progress and failures.
 */

/* Double buffer. The socket fills one while the flash programs the other.
 *
 * This is worth having but it is not where the time goes: flash programming is
 * roughly four times slower than the link, so overlapping them hides the
 * network behind the flash rather than the other way around. What it really
 * buys is that the socket keeps draining during a program cycle, so the TCP
 * window never slams shut and the sender never stalls. */
static uint8_t nl_buf[2][CONFIG_RVL_NETLOAD_BUF];
/* volatile is insurance, not a fix for a known bug: the k_sem calls either
 * side of these are real function calls and already order them. It costs
 * nothing and removes one class of doubt from a path that is currently under
 * investigation. */
static volatile uint32_t nl_blen[2];
static volatile uint32_t nl_boff[2];
static struct k_sem nl_free;      /* buffers the producer may fill */
static struct k_sem nl_filled;    /* buffers the writer must program */
static volatile int nl_wrc;       /* first write error, sticky */
static volatile uint32_t nl_wfail_off;   /* flash offset it failed at */
static volatile uint32_t nl_wr_off;      /* flash offset currently being written */
static volatile bool nl_wr_busy;         /* inside flash_write() right now */

/* Set if a writer thread ever had to be abandoned. Its k_thread object and
 * stack are then permanently unsafe to reuse, so further pushes are refused
 * rather than corrupting a live thread. */
static bool nl_writer_lost;

K_THREAD_STACK_DEFINE(nl_wstack, CONFIG_RVL_NETLOAD_WRITER_STACK);
static struct k_thread nl_wtcb;

/* Consumer. Started per push and joined at the end, so a writer can never
 * outlive the transfer that created it and program a stale buffer into the
 * next one. A zero-length buffer is the end sentinel.
 *
 * On failure it keeps draining rather than exiting: the producer is blocked on
 * nl_free, and a writer that stopped handing buffers back would deadlock it. */
static void nl_writer(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (int w = 0;; w ^= 1) {
		k_sem_take(&nl_filled, K_FOREVER);

		if (nl_blen[w] == 0) {
			return;
		}
		/* Published before the call so the producer's stall report can
		 * name the exact write that did not come back. */
		nl_wr_off = nl_boff[w];
		nl_wr_busy = true;

		if (nl_wrc == 0 &&
		    flash_write(nor, nl_boff[w], nl_buf[w], nl_blen[w]) != 0) {
			/* Keep the address. "write failed" without one leaves
			 * no way to tell a transport problem from an
			 * offset-dependent flash problem. */
			nl_wfail_off = nl_boff[w];
			nl_wrc = -EIO;
		}
		nl_wr_busy = false;
		k_sem_give(&nl_free);
	}
}

/* Deliberately not tn_send_all(): that lives behind CONFIG_RVL_TELNET, and the
 * TCP loader has no reason to stop working when the console bridge is off. */
static int nl_send_str(int s, const char *msg)
{
	const uint8_t *p = (const uint8_t *)msg;
	size_t n = strlen(msg);

	while (n) {
		ssize_t w = zsock_send(s, p, n, 0);

		if (w <= 0) {
			return -1;
		}
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

/* Announce the slot table before reading anything.
 *
 * The board is the authority on where its slots are, and the host tool carries
 * its own copy of those numbers. Sending them lets the host refuse to push
 * against a firmware whose layout it does not recognise -- which matters
 * because the failure it prevents is "erased the wrong region", and that one is
 * not recoverable by retrying. */
static int nl_send_banner(int s)
{
	char line[128];
	int n = 0;

	n += snprintk(line + n, sizeof(line) - n, "RA8LDR 1");
	for (int i = 0; i < (int)IMG_NSLOTS; i++) {
		n += snprintk(line + n, sizeof(line) - n, " %s:%u:%u",
			      img_slots[i].name, img_slots[i].off,
			      img_max_len(i));
	}
	n += snprintk(line + n, sizeof(line) - n, "\n");

	return nl_send_str(s, line);
}

/* Bounded receive.
 *
 * This does its own waiting with zsock_poll() rather than relying on
 * SO_RCVTIMEO, because SO_RCVTIMEO is not reliably there: Zephyr gates the
 * whole option behind CONFIG_NET_CONTEXT_RCVTIMEO, and when that is off
 * setsockopt fails and the socket silently keeps blocking forever. That is
 * not a hypothetical -- it is what wedged this service until a reset, because
 * a listener stuck in recv() never gets back to accept() and every later
 * connection then sits unaccepted in the backlog looking, from the host, like
 * a dead board.
 *
 * The Kconfig is enabled too, but the poll is what makes the timeout a
 * property of this code instead of a property of the configuration.
 * tn_serve() already polls, so this path is known good in this build.
 *
 * Returns bytes read, 0 on timeout, -1 on error or orderly close.
 */
static int nl_recv(int s, uint8_t *p, size_t n, int timeout_s)
{
	struct zsock_pollfd pfd = { .fd = s, .events = ZSOCK_POLLIN };
	int pr = zsock_poll(&pfd, 1, timeout_s * 1000);

	if (pr == 0) {
		return 0;                       /* timed out */
	}
	if (pr < 0 || (pfd.revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL))) {
		return -1;
	}

	ssize_t got = zsock_recv(s, p, n, 0);

	return got > 0 ? (int)got : -1;
}

/* Read exactly n bytes, or fail. Used for the fixed-size header only. */
static int nl_recv_exact(int s, uint8_t *p, size_t n)
{
	while (n) {
		int got = nl_recv(s, p, n, CONFIG_RVL_NETLOAD_RX_TIMEOUT_S);

		if (got <= 0) {
			return -1;
		}
		p += got;
		n -= (size_t)got;
	}
	return 0;
}

/* Send "BLKCRC <blocksize>", then one hex CRC per block, then "ENDCRC". */
static void nl_send_blkcrc(int s, int idx, uint32_t len)
{
	const struct img_slot *sl = &img_slots[idx];
	const uint8_t *body = (const uint8_t *)(SLOT_MMAP(sl) + IMG_PAYLOAD_OFF);
	uint32_t nblk = (len + IMG_DIAG_BLK - 1) / IMG_DIAG_BLK;
	char line[160];
	int n = 0;

	snprintk(line, sizeof(line), "BLKCRC %u %u\n", IMG_DIAG_BLK, nblk);
	if (nl_send_str(s, line) != 0) {
		return;
	}

	for (uint32_t b = 0; b < nblk; b++) {
		uint32_t off = b * IMG_DIAG_BLK;
		uint32_t sz = MIN(IMG_DIAG_BLK, len - off);

		n += snprintk(line + n, sizeof(line) - n, "%08x ",
			      crc32_ieee(body + off, sz));

		/* Flush on a whole line rather than per block: 16 CRCs is one
		 * send instead of sixteen, and the host parses whitespace. */
		if (n > (int)sizeof(line) - 12 || b + 1 == nblk) {
			line[n++] = '\n';
			line[n] = '\0';
			if (nl_send_str(s, line) != 0) {
				return;
			}
			n = 0;
		}
	}
	nl_send_str(s, "ENDCRC\n");
}

static int nl_push(int s)
{
	uint8_t hdr[10];   /* 'S', slot, len32le, crc32le */
	uint32_t len, crc, off = 0;
	int slot;

	/* Belt to nl_recv()'s braces. The poll is what actually enforces the
	 * timeout; this only helps any send-side blocking. The return is
	 * checked and reported rather than discarded -- swallowing it with
	 * (void) is precisely how the missing CONFIG_NET_CONTEXT_RCVTIMEO
	 * stayed invisible while it wedged the service. */
	struct zsock_timeval rcvto = {
		.tv_sec = CONFIG_RVL_NETLOAD_RX_TIMEOUT_S,
		.tv_usec = 0,
	};
	int one = 1;

	if (zsock_setsockopt(s, ZSOCK_SOL_SOCKET, ZSOCK_SO_RCVTIMEO,
			     &rcvto, sizeof(rcvto)) < 0) {
		us("netload: note, SO_RCVTIMEO unavailable; poll timeout in use\r\n");
	}
	(void)zsock_setsockopt(s, ZSOCK_SOL_SOCKET, ZSOCK_SO_KEEPALIVE,
			       &one, sizeof(one));

	if (nl_writer_lost) {
		us("netload: refusing push, writer thread was abandoned\r\n");
		nl_send_str(s, "ERR loader disabled, reset the board\n");
		return -1;
	}

	if (nl_send_banner(s) != 0) {
		return -1;
	}
	if (nl_recv_exact(s, hdr, sizeof(hdr)) != 0) {
		return -1;
	}
	if (hdr[0] != 'S') {
		nl_send_str(s, "ERR bad magic\n");
		return -1;
	}
	slot = hdr[1];
	len = (uint32_t)hdr[2] | ((uint32_t)hdr[3] << 8) |
	      ((uint32_t)hdr[4] << 16) | ((uint32_t)hdr[5] << 24);
	crc = (uint32_t)hdr[6] | ((uint32_t)hdr[7] << 8) |
	      ((uint32_t)hdr[8] << 16) | ((uint32_t)hdr[9] << 24);

	if (slot >= (int)IMG_NSLOTS) {
		nl_send_str(s, "ERR bad slot\n");
		return -1;
	}

	us("netload: slot=");
	us(img_slots[slot].name);
	us(" len=");
	udec(len);
	us(" crc=");
	uhex(crc);
	us("\r\n");

	/* Bounded before a single block is erased, exactly as on the UART side,
	 * so a bad header leaves the stored image intact. */
	if (img_accept_len(slot, len) != 0) {
		nl_send_str(s, "ERR length out of range\n");
		return -1;
	}
	if (nl_send_str(s, "OK header\n") != 0) {
		return -1;
	}

	int64_t t0 = k_uptime_get();

	if (img_erase(slot, len, NULL) != 0) {
		nl_send_str(s, "ERR erase failed\n");
		return -1;
	}

	us("netload: erased in ");
	udec((uint32_t)(k_uptime_get() - t0));
	us(" ms\r\n");
	if (nl_send_str(s, "OK erased\n") != 0) {
		return -1;
	}

	k_sem_init(&nl_free, 2, 2);
	k_sem_init(&nl_filled, 0, 2);
	nl_wrc = 0;
	nl_wfail_off = 0;

	k_thread_create(&nl_wtcb, nl_wstack, K_THREAD_STACK_SIZEOF(nl_wstack),
			nl_writer, NULL, NULL, NULL,
			CONFIG_RVL_NETLOAD_WRITER_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&nl_wtcb, "imgwrite");

	int64_t t_rx = k_uptime_get();
	bool failed = false;
	bool stalled = false;
	int i = 0;

	/* Every exit from this loop happens while holding buffer `i`, which is
	 * what lets the sentinel below always have a buffer to go in. */
	for (;;) {
		/* Bounded, for the same reason the recv above is bounded, and
		 * it is the same bug: this thread stops draining the socket
		 * while it waits here, so a writer that never hands a buffer
		 * back closes the TCP window and the host's send() blocks
		 * partway through a transfer that was progressing normally.
		 * From the outside that looks like a mid-stream stall with a
		 * healthy board, and it produces no console output at all.
		 *
		 * A buffer should come back in ~35 ms (8 KB of 64-byte page
		 * programs). Waiting seconds means the flash operation is not
		 * returning, which FSP can do: several of its register waits
		 * are unbounded FSP_HARDWARE_REGISTER_WAIT spins with no
		 * timeout of their own. */
		if (k_sem_take(&nl_free, K_MSEC(CONFIG_RVL_NETLOAD_STALL_MS)) != 0) {
			us("netload: STALLED, no buffer back for ");
			udec(CONFIG_RVL_NETLOAD_STALL_MS);
			us(" ms; writer ");
			us(nl_wr_busy ? "still inside flash_write at 0x"
				      : "idle, last write at 0x");
			uhex(nl_wr_off);
			us(", ");
			udec(off);
			us(" of ");
			udec(len);
			us(" bytes done\r\n");
			stalled = true;
			failed = true;
			break;
		}

		if (off >= len || nl_wrc != 0) {
			break;
		}

		/* Fill the buffer completely before handing it over rather than
		 * programming whatever one recv() happened to return. Writes
		 * then land in large aligned runs instead of TCP-segment-sized
		 * dribbles, which matters when each one costs a program-and-
		 * poll cycle per 64 bytes. */
		uint32_t want = MIN((uint32_t)CONFIG_RVL_NETLOAD_BUF, len - off);
		uint32_t have = 0;

		while (have < want) {
			int got = nl_recv(s, (uint8_t *)nl_buf[i] + have,
					  want - have,
					  CONFIG_RVL_NETLOAD_RX_TIMEOUT_S);

			if (got <= 0) {
				us(got == 0 ? "netload: rx timeout\r\n"
					    : "netload: rx error\r\n");
				failed = true;
				break;
			}
			have += (uint32_t)got;
		}
		if (failed) {
			break;
		}

		nl_blen[i] = have;
		nl_boff[i] = img_slots[slot].off + IMG_PAYLOAD_OFF + off;
		k_sem_give(&nl_filled);
		off += have;
		i ^= 1;

		/* A mark every 1 MB, on the UART rather than the socket. If a
		 * push stalls, the last one printed says how far it got and at
		 * what flash address -- which is the first thing anyone asks
		 * and, before this, was not recorded anywhere.
		 *
		 * 1 MB not 4: the first stall we saw happened at 2.38 MB and
		 * fell between two 4 MB marks, so the console said nothing at
		 * all about a push that had been running for thirty seconds. */
		if ((off & ((1U << 20) - 1)) < CONFIG_RVL_NETLOAD_BUF) {
			us("netload: ");
			udec(off >> 20);
			us(" MB at flash 0x");
			uhex(img_slots[slot].off + IMG_PAYLOAD_OFF + off);
			us("\r\n");
		}
	}

	if (stalled) {
		/* The stall path is the one case where we do not own buffer i,
		 * so there is nowhere to put the sentinel and the writer cannot
		 * be joined -- it is wedged inside the flash driver. Abandon it
		 * and refuse later pushes rather than reusing a thread object
		 * that is still live, which would corrupt the scheduler.
		 *
		 * This is a genuine dead end for the service: recovering the
		 * OSPI controller from here needs a reset. Saying so beats
		 * hanging, which is what this did before. */
		nl_writer_lost = true;
		us("netload: writer abandoned, loader disabled until reset\r\n");
		nl_send_str(s, "ERR writer stalled, reset the board\n");
		return -1;
	}

	nl_blen[i] = 0;
	k_sem_give(&nl_filled);

	if (k_thread_join(&nl_wtcb, K_MSEC(CONFIG_RVL_NETLOAD_STALL_MS)) != 0) {
		nl_writer_lost = true;
		us("netload: writer did not exit, loader disabled until reset\r\n");
		nl_send_str(s, "ERR writer stalled, reset the board\n");
		return -1;
	}

	/* Flash first: a write failure also stops the producer, so it would
	 * otherwise be reported as the short transfer it causes rather than as
	 * the cause. */
	if (nl_wrc != 0) {
		us("netload: flash write failed at 0x");
		uhex(nl_wfail_off);
		us(" (payload offset ");
		udec(nl_wfail_off - img_slots[slot].off - IMG_PAYLOAD_OFF);
		us(")\r\n");
		nl_send_str(s, "ERR flash write failed\n");
		return -1;
	}
	if (failed || off != len) {
		us("netload: transfer died at payload offset ");
		udec(off);
		us(" of ");
		udec(len);
		us(", flash 0x");
		uhex(img_slots[slot].off + IMG_PAYLOAD_OFF + off);
		us("\r\n");
		nl_send_str(s, "ERR short transfer\n");
		return -1;
	}

	uint32_t ms = (uint32_t)(k_uptime_get() - t_rx);

	us("netload: ");
	udec(len);
	us(" bytes in ");
	udec(ms);
	us(" ms\r\n");

	if (img_commit(slot, len, crc) != 0) {
		/* Stream a CRC per 64 KB block so the host can diff them
		 * against the source file it still has in memory. The board
		 * cannot do that comparison -- it has never seen the original
		 * -- but it is the comparison that matters, because *where* the
		 * corruption sits is the discriminator: a contiguous tail says
		 * the writes stopped landing, a scattered handful says
		 * individual operations are failing, and one bad block in an
		 * otherwise perfect image says something quite different again.
		 * A few KB of CRCs buys that without a 24 MB readback. */
		nl_send_blkcrc(s, slot, len);
		nl_send_str(s, "ERR verify failed\n");
		return -1;
	}

	us("netload: ");
	us(img_slots[slot].name);
	us(" committed and verified\r\n");
	nl_send_str(s, "OK committed\n");
	return 0;
}

static void nl_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (net_addr_now() == NULL) {
		k_msleep(250);
	}

	for (;;) {
		struct net_sockaddr_in sa = {
			.sin_family = NET_AF_INET,
			.sin_port = net_htons(CONFIG_RVL_NETLOAD_PORT),
			.sin_addr.s_addr = 0,   /* NET_INADDR_ANY */
		};
		int ls = zsock_socket(NET_AF_INET, NET_SOCK_STREAM, NET_IPPROTO_TCP);

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

		for (;;) {
			struct net_sockaddr_in peer;
			net_socklen_t plen = sizeof(peer);
			int cs = zsock_accept(ls, (struct net_sockaddr *)&peer, &plen);

			if (cs < 0) {
				break;   /* rebuild the listener */
			}

			us("netload: connection from ");
			print_ipv4(&peer.sin_addr);
			us("\r\n");

			(void)nl_push(cs);
			zsock_close(cs);
		}

		zsock_close(ls);
	}
}

K_THREAD_STACK_DEFINE(nl_stack, CONFIG_RVL_NETLOAD_THREAD_STACK);
static struct k_thread nl_tcb;

#endif /* CONFIG_RVL_NETLOAD */

/* ------------------------------------------------------------------- boot */

static void guest_prepare(uint32_t img_len)
{
	uint8_t *ram = (uint8_t *)SDRAM_BASE;
	uint32_t dtb_ptr;

	us("copy ");
	udec(img_len);
	us(" B flash->sdram... ");

	int64_t t0 = k_uptime_get();

	/* Kernel at guest offset 0, everything above it zeroed: the guest's
	 * .bss and free pages must start clean. */
	memcpy(ram, (const void *)(SLOT_MMAP(&img_slots[SLOT_KERNEL]) +
				   IMG_PAYLOAD_OFF), img_len);
	memset(ram + img_len, 0, GUEST_RAM_SIZE - img_len);

	us("ok ");
	udec((uint32_t)(k_uptime_get() - t0));
	us(" ms\r\n");

	/* DTB then emulator state occupy the top of guest RAM. */
	dtb_ptr = GUEST_RAM_SIZE - sizeof(default64mbdtb) -
		  sizeof(struct MiniRV32IMAState);
	memcpy(ram + dtb_ptr, default64mbdtb, sizeof(default64mbdtb));

	/* Patch the DTB's memory size. The stock blob carries sentinel
	 * 0x00c0ff03 at byte 0x13c; without this the guest believes it has
	 * 64 MB of usable RAM and walks over the DTB and our own state. */
	uint32_t *dtb = (uint32_t *)(ram + dtb_ptr);

	if (dtb[0x13c / 4] == 0x00c0ff03) {
		uint32_t v = dtb_ptr;

		dtb[0x13c / 4] = (v >> 24) | (((v >> 16) & 0xff) << 8) |
				 (((v >> 8) & 0xff) << 16) | ((v & 0xff) << 24);
	} else {
		us("WARN: dtb ram sentinel not found\r\n");
	}

	core = (struct MiniRV32IMAState *)(ram + GUEST_RAM_SIZE -
					   sizeof(struct MiniRV32IMAState));
	memset(core, 0, sizeof(*core));

	core->pc = MINIRV32_RAM_IMAGE_OFFSET;
	core->regs[10] = 0x00;                                  /* a0 = hart id  */
	core->regs[11] = dtb_ptr + MINIRV32_RAM_IMAGE_OFFSET;   /* a1 = DTB phys */
	core->extraflags |= 3;                                  /* machine mode  */
}

int main(void)
{
	uint32_t img_len = 0;
	int rc;

	if (!device_is_ready(uart)) {
		return -1;
	}

	us("\r\n=== rv32ima Linux on EK-RA8D1 ===\r\n");
	us("core ");
	udec(sys_clock_hw_cycles_per_sec() / 1000000U);
	us(" MHz, sdram ");
	udec(SDRAM_SIZE / (1024 * 1024));
	us(" MB @ 0x");
	uhex(SDRAM_BASE);
	us("\r\n");

	/* Before the guest exists, prove the bridge works from this side. */
	pv_selftest();

#ifdef CONFIG_NETWORKING
	net_bringup();
#endif
#ifdef CONFIG_RVL_TELNET
	k_thread_create(&tn_tcb, tn_stack, K_THREAD_STACK_SIZEOF(tn_stack),
			tn_thread, NULL, NULL, NULL,
			CONFIG_RVL_TELNET_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&tn_tcb, "telnet");
#endif
#ifdef CONFIG_RVL_NETLOAD
	k_thread_create(&nl_tcb, nl_stack, K_THREAD_STACK_SIZEOF(nl_stack),
			nl_thread, NULL, NULL, NULL,
			CONFIG_RVL_NETLOAD_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&nl_tcb, "imgload");
#endif

#ifdef CONFIG_NETWORKING
	/* Everything from here on either busy-polls the UART (the loader) or
	 * runs guest instructions, and neither ever blocks or yields. Zephyr
	 * schedules preemptible threads strictly by priority, so at the default
	 * priority 0 this thread would starve the Ethernet RX thread (2), the
	 * TCP worker (2) and the telnet thread (5) for as long as it runs --
	 * which, for the loader, is the length of a multi-megabyte upload.
	 * Drop below all of them and let the network stack preempt at will. */
	k_thread_priority_set(k_current_get(), CONFIG_RVL_EMU_PRIORITY);
#endif

	/* Forced loader first, then the usual check. Ordering it this way is
	 * what makes the forced path recoverable: img_receive() erases nothing
	 * until it has seen 'S' and a plausible length, so an operator who hits
	 * 'L' and then changes their mind can let it time out and the stored
	 * image still boots. */
	if (loader_requested() && img_receive() != 0) {
		us("LOADER: aborted, falling back to the stored image\r\n");
	}

	rc = img_check(SLOT_KERNEL, &img_len, true);
	if (rc != 0) {
		us("no valid kernel at 0x");
		uhex(SLOT_MMAP(&img_slots[SLOT_KERNEL]));
		us(" (rc ");
		udec((uint32_t)(-rc));
		us(")\r\n");

		if (img_receive() != 0 ||
		    img_check(SLOT_KERNEL, &img_len, true) != 0) {
			us("FATAL: no image, halting\r\n");
			return -1;
		}
	}

	us("kernel ok, ");
	udec(img_len);
	us(" bytes\r\n");

	/* Report the rootfs slot but do not touch it. Nothing consumes it yet
	 * -- the guest reaches its filesystem through the kernel image today --
	 * so this exists to make a pushed rootfs visible rather than silent.
	 *
	 * Header-checked only. A full CRC here would read the whole slot
	 * through the OSPI window on every single boot, which at 45 MB is
	 * seconds of dead time to re-verify something the push already
	 * verified. The push is where the CRC earns its cost. */
	uint32_t rfs_len;

	if (img_check(SLOT_ROOTFS, &rfs_len, false) == 0) {
		us("rootfs slot: ");
		udec(rfs_len);
		us(" bytes at 0x");
		uhex(SLOT_MMAP(&img_slots[SLOT_ROOTFS]));
		us(" (header only, not verified)\r\n");
	} else {
		us("rootfs slot: empty\r\n");
	}

	guest_prepare(img_len);

	us("booting guest...\r\n\r\n");

	cyc_last = k_cycle_get_32();
	cyc_total = 0;

	uint64_t last_us = now_us();
	int64_t t_boot = k_uptime_get();

	for (;;) {
		uint64_t t = now_us();
		uint32_t elapsed = (uint32_t)(t - last_us);

		last_us = t;

		int ret = MiniRV32IMAStep(core, (uint8_t *)SDRAM_BASE, 0,
					  elapsed, 1024);

		switch (ret) {
		case 0:
			break;
		case 1:
			/* WFI: nothing to do but keep the guest clock moving. */
			k_busy_wait(50);
			break;
		case 3:
			/* Fault. Keep running; the guest may recover. */
			break;
		case 0x7777:
			us("\r\n[reboot requested]\r\n");
			guest_prepare(img_len);
			break;
		case 0x5555:
			us("\r\n[poweroff] after ");
			udec((uint32_t)(k_uptime_get() - t_boot));
			us(" ms, cycles 0x");
			uhex(core->cycleh);
			uhex(core->cyclel);
			us("\r\n");
			return 0;
		default:
			us("\r\n[unknown step result]\r\n");
			break;
		}
	}
}
