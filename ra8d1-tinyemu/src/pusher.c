/*
 * pusher.c - the RA8LDR image loader over TCP, on port 5555.
 *
 * Ported from ra8d1-linux/rvlinux/src/main.c, where this loader is running on
 * this same board and this same flash against the nommu guest. The wire
 * protocol, the slot geometry, the erase/receive/commit ordering and every
 * reliability fix that path gained on 2026-08-08 are carried over unchanged,
 * so ra8d1-linux/rvlinux/tools/pushimage.py drives this build with no edits.
 * Only the differences are argued here; see ra8d1-linux/notes/03-image-storage.md
 * for the original reasoning and notes/pusher.md for this port.
 *
 * --------------------------------------------------------------------------
 * The one real difference: this guest reads flash while it runs.
 *
 * rvlinux copies its kernel out of OSPI into SDRAM at boot and never looks at
 * the window again, so a push there lands in flash underneath a running guest
 * and takes effect at the next reset. The comment in that file says, in as
 * many words, that this stops being true the moment the guest reads flash
 * directly - which is exactly what this app does. virtio-blk hands the rootfs
 * slot to the guest as a pointer into the memory-mapped OSPI window and the
 * guest reads it in place for its whole life, because that is the only reason
 * a 55 MB rootfs fits on a board with 64 MB of SDRAM.
 *
 * An OSPI-B device cannot serve memory-mapped reads while a program or erase
 * is in progress, and the mapped window has no way to report a fault, so a
 * push racing a live guest corrupts every read that collides with it -
 * silently. Erasing the slot the guest is running from is worse still.
 *
 * So: a push stops the guest before it erases anything, and the board reboots
 * into the new image when the operator is done pushing. rv_request_poweroff()
 * already ends the run loop at the next instruction-slice boundary from
 * another thread - it is how the guest's own syscon poweroff works - so this
 * needs no new emulator code and no change to main()'s loop.
 *
 * Three alternatives were weighed and rejected; notes/pusher.md records why.
 * The short version: A/B slots need twice the flash and the rootfs slot is
 * already most of the chip, staging to SDRAM needs RAM the guest is using,
 * and letting the push race the guest is the corruption above.
 *
 * --------------------------------------------------------------------------
 * Why the reboot is deferred rather than immediate.
 *
 * The normal operation is two pushes back to back - kernel, then rootfs - and
 * rebooting after the first would boot a new kernel against the old rootfs,
 * then make the operator wait out DHCP before the second push could start. So
 * a successful push arms a reboot CONFIG_RVT_PUSHER_REBOOT_DELAY_S in the
 * future and any new connection defers it again. The board reboots once the
 * operator stops pushing, which is when they wanted it to.
 *
 * A *failed* push does not arm it, deliberately: pushimage.py retries three
 * times by default and a board that rebooted between attempts would refuse
 * every one of them with a connection error. The guest is already stopped and
 * the slot is already erased, so staying up costs nothing and is what makes
 * the retry work.
 */

#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>

#include <errno.h>
#include <string.h>

#include "image.h"
#include "pusher.h"
#include "rv_machine.h"
#include "rv_platform.h"
#include "telnet.h"

/* ------------------------------------------------------------ tiny output
 *
 * Same shape as the copies in main.c and telnet.c, and duplicated for the same
 * reason they are: these have to reach the physical UART with no dependency on
 * anything else in the application being configured in. Never plat_putc() -
 * that is the guest's console and follows a telnet client.
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

static void uhex(uint32_t v)
{
	us("0x");
	for (int i = 28; i >= 0; i -= 4) {
		uint32_t d = (v >> i) & 0xf;

		up((char)(d < 10 ? '0' + d : 'a' + d - 10));
	}
}

static void print_ipv4(const struct net_in_addr *a)
{
	for (int i = 0; i < 4; i++) {
		udec(a->s4_addr[i]);
		if (i < 3) {
			up('.');
		}
	}
}

static bool have_addr(void)
{
	struct net_if *iface = net_if_get_default();

	return iface != NULL &&
	       net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) != NULL;
}

/* ------------------------------------------------------- the one hard rule
 *
 * *** THE S28HL512T FORBIDS PROGRAMMING THE SAME 16-BYTE UNIT TWICE. ***
 *
 * This is Semper NOR with internal ECC over 16-byte data units. The datasheet
 * prohibits a second program to a unit without an erase in between: the ECC
 * syndrome is computed and stored when the unit is programmed, and programming
 * it again leaves the syndrome inconsistent with the data. The part answers
 * either with a Program Error or - far worse - with data that is present,
 * stable across every read, and wrong. Infineon documents the rule and the
 * U-Boot and Linux MTD drivers for the S28 series enforce it.
 *
 * It is the leading suspect for BUILD.md's intermittent single-block verify
 * failure, which has exactly that signature: written, read back stably twice,
 * and wrong.
 *
 * Three consequences, all of them enforced below rather than left to care:
 *
 *   1. Nothing here ever re-programs. CONFIG_RVT_PUSHER_VERIFY_ATTEMPTS is a
 *      *read* retry - it re-reads and re-CRCs the same bytes, it never rewrites
 *      them. A verify that fails all its attempts fails the whole push and the
 *      host re-pushes, which erases first. DO NOT ADD A REWRITE-ON-FAILURE PATH
 *      HERE; it would be the corruption above, on purpose.
 *   2. The header and the payload must not share a unit. They do not: the
 *      header write covers [0, IMG_PAYLOAD_OFF) and the payload starts at
 *      IMG_PAYLOAD_OFF, which the assert below pins to a unit boundary.
 *   3. Every payload program has to start and end on a unit boundary, or the
 *      *next* program would touch the tail unit of the previous one. Buffer-
 *      sized writes do by construction; the final short one is padded with
 *      0xFF in nl_push().
 */
#define FLASH_ECC_UNIT 16U

BUILD_ASSERT(IMG_PAYLOAD_OFF % FLASH_ECC_UNIT == 0,
	     "payload must start on a 16-byte ECC unit, or the header write and "
	     "the first payload write share one and the second corrupts it");
BUILD_ASSERT(CONFIG_RVT_PUSHER_BUF % FLASH_ECC_UNIT == 0,
	     "every full-buffer write must end on a 16-byte ECC unit, or each "
	     "write reprograms the unit the previous one left half done");

/* ------------------------------------------------------------ slot limits */

/*
 * Guest RAM the kernel may not run into.
 *
 * rv_machine_init() copies the kernel to the base of guest RAM and puts the
 * generated devicetree in the last megabyte, so a kernel longer than this
 * would be memcpy'd over the DTB. Necessary but not sufficient: the kernel's
 * BSS extends past the image (rv_kernel_footprint() reads image_size out of
 * the Image header for exactly that reason) and the loader cannot know it from
 * a length on the wire. rv_machine_init() rejects that case with -ENOMEM.
 *
 * On this board the flash bound always binds first - the kernel slot is 8 MB
 * and the SDRAM is 64 - so this ceiling has never been the one that fired. It
 * is here so that shrinking the SDRAM or growing the slot cannot quietly turn
 * a push into an overwritten devicetree.
 */
#define IMG_KERNEL_RAM_RESERVE (1u << 20)

static uint32_t img_max_len(int slot)
{
	uint32_t writable;
	uint32_t cap;

	(void)plat_slot_offset(slot, &writable);
	if (writable <= IMG_PAYLOAD_OFF) {
		return 0;
	}
	cap = writable - IMG_PAYLOAD_OFF;

	if (slot == PLAT_SLOT_KERNEL) {
		size_t ram_size;
		uint32_t ram_cap;

		(void)plat_guest_ram(&ram_size);
		ram_cap = ram_size > IMG_KERNEL_RAM_RESERVE
				  ? (uint32_t)(ram_size - IMG_KERNEL_RAM_RESERVE)
				  : 0;
		if (cap > ram_cap) {
			cap = ram_cap;
		}
	}
	return cap;
}

/* ------------------------------------------------------- stopping the guest
 *
 * The handshake is one flag and one semaphore. main() calls
 * rvt_pusher_guest_halted() as soon as its run loop returns, whatever stopped
 * it, so this works equally for a guest that powered itself off, a board whose
 * image never booted, and a guest this file stopped on purpose.
 */

static K_SEM_DEFINE(halt_ack, 0, 1);
static volatile bool guest_stopped;   /* the run loop has returned */
static volatile bool halt_by_pusher;  /* ... and this is why */

bool rvt_pusher_guest_halted(void)
{
	guest_stopped = true;
	k_sem_give(&halt_ack);
	return halt_by_pusher;
}

/*
 * Ask the emulator to stop, and wait until it has.
 *
 * The request is repeated rather than issued once because of a narrow but real
 * race at boot: main() runs the image CRCs (seconds, at rootfs sizes) before
 * rv_machine_init(), and rv_machine_init() memsets the machine - including the
 * stop flag - on its way past. A push that arrived during the CRC would have
 * its single request erased and then wait out the timeout against a guest that
 * runs happily forever. Re-asking every 100 ms costs nothing and closes it.
 */
static bool halt_guest(void)
{
	int64_t deadline = k_uptime_get() + CONFIG_RVT_PUSHER_HALT_MS;
	int64_t t0 = k_uptime_get();

	if (guest_stopped) {
		return true;
	}

	us("pusher: stopping the guest -- this app reads the rootfs in place "
	   "out of the flash being written\r\n");
	halt_by_pusher = true;

	do {
		rv_request_poweroff();
		if (k_sem_take(&halt_ack, K_MSEC(100)) == 0) {
			us("pusher: guest halted in ");
			udec((uint32_t)(k_uptime_get() - t0));
			us(" ms\r\n");
			return true;
		}
	} while (k_uptime_get() < deadline);

	us("pusher: guest did NOT stop within ");
	udec(CONFIG_RVT_PUSHER_HALT_MS);
	us(" ms; refusing to erase\r\n");
	return false;
}

/* ------------------------------------------------------------- slot access */

/* Validate what is stored in a slot, straight out of the memory-mapped window.
 *
 * Deliberately a second implementation of main.c's slot_check() rather than a
 * shared one: that one answers "is this bootable", this one answers "is this
 * what I just wrote", and the two differ in what they do with the length. They
 * agree on the format because both take it from image.h, which is the part
 * that must not drift. */
static int img_check(int slot, uint32_t *len_out, bool verify)
{
	size_t slot_size;
	const uint8_t *base = plat_slot_base(slot, &slot_size);
	const struct img_hdr *h = (const struct img_hdr *)base;
	const uint8_t *body;

	if (base == NULL) {
		return -1;
	}
	body = base + IMG_PAYLOAD_OFF;

	if (memcmp(h->magic, img_slot_magic(slot), 8) != 0) {
		return -1;
	}
	if (h->len == 0 || h->len > img_max_len(slot)) {
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
 * writes all returned success, and the readback disagrees. Two measurements
 * discriminate between the faults that produces.
 *
 * Erased blocks. A 64 KB block still entirely 0xFF was never programmed, and
 * where the first one starts answers "how far did the writes get", which is a
 * different question from "how far did the transfer get".
 *
 * Read stability. The payload is CRC'd twice with an explicit invalidate
 * between, because there are two caches here and only one of them can be
 * flushed: sys_cache_data_invd_range() clears the CPU D-cache, but the OSPI-B
 * bridge has its own hardware read prefetch that R_OSPI_B_Write() does not
 * flush and the flash API does not expose. A stale prefetch is a fault this
 * code cannot prevent, but it can prove.
 */
#define IMG_DIAG_BLK (64U * 1024U)

static void img_diagnose(int slot, uint32_t len)
{
	size_t slot_size;
	const uint8_t *base = plat_slot_base(slot, &slot_size);
	const struct img_hdr *h = (const struct img_hdr *)base;
	const uint8_t *body = base + IMG_PAYLOAD_OFF;
	uint32_t off_flash = plat_slot_offset(slot, NULL);
	uint32_t nblk = (len + IMG_DIAG_BLK - 1) / IMG_DIAG_BLK;
	uint32_t erased_blocks = 0, first_erased = UINT32_MAX;
	uint32_t unstable_blocks = 0, first_unstable = UINT32_MAX;
	uint32_t crc1, crc2;

	us("DIAG: slot ");
	us(img_slot_name(slot));
	us(" off=");
	uhex(off_flash);
	us(" payload=");
	uhex(off_flash + IMG_PAYLOAD_OFF);
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
		uint32_t ca;

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

		/* Same block twice with an invalidate between, so an unstable
		 * read is localised rather than merely detected: a marginal
		 * interface scatters, a coherency fault runs contiguously. */
		ca = crc32_ieee(body + off, n);
		sys_cache_data_invd_range((void *)(body + off), ROUND_UP(n, 32U));
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
		us(", first at payload ");
		uhex(first_unstable);
		us(" = flash ");
		uhex(off_flash + IMG_PAYLOAD_OFF + first_unstable);
	}
	us("\r\n");

	us("DIAG: ");
	udec(erased_blocks);
	us(" of ");
	udec(nblk);
	us(" 64K blocks still erased (0xFF)");
	if (first_erased != UINT32_MAX) {
		us(", first at payload ");
		uhex(first_erased);
		us(" = flash ");
		uhex(off_flash + IMG_PAYLOAD_OFF + first_erased);
	}
	us("\r\n");

	crc1 = crc32_ieee(body, len);
	sys_cache_data_invd_range((void *)base,
				  ROUND_UP(len + IMG_PAYLOAD_OFF,
					   plat_flash_erase_size()));
	crc2 = crc32_ieee(body, len);

	us("DIAG: readback crc pass1=");
	uhex(crc1);
	us(" pass2=");
	uhex(crc2);
	us(crc1 == crc2 ? " stable\r\n" : " UNSTABLE -- stale read\r\n");
}

/* ------------------------------------ the three steps every push goes through
 *
 * Bound the length before erasing anything, payload first, header last.
 */

/* Bound a length off the wire. Called before a single block is erased: the
 * length arrives as four raw bytes with no framing behind it, so a desynced or
 * wrong-endian host can present any 32-bit value at all, and the ROUND_UP() in
 * img_erase() would wrap. Rejecting here also means a bad push leaves the image
 * already in the slot intact - and, since nothing has been stopped yet, leaves
 * the guest running too. */
static int img_accept_len(int slot, uint32_t len)
{
	if (len == 0 || len > img_max_len(slot)) {
		us("pusher: ");
		us(img_slot_name(slot));
		us(" length out of range, max ");
		udec(img_max_len(slot));
		us("\r\n");
		return -1;
	}
	return 0;
}

/* Erase enough blocks for header + payload, one block per call.
 *
 * The driver's loop is the same either way, but a 36 MB rootfs is 143 blocks
 * and takes about two minutes; erasing a block at a time is what lets a dot
 * reach the console instead of going silent for that long, which is
 * indistinguishable from a hung board. */
static int img_erase(int slot, uint32_t len)
{
	const struct device *nor = plat_flash_dev();
	uint32_t off = plat_slot_offset(slot, NULL);
	uint32_t esz = plat_flash_erase_size();
	uint32_t need = ROUND_UP(len + IMG_PAYLOAD_OFF, esz);
	size_t slot_size;
	const uint8_t *base = plat_slot_base(slot, &slot_size);
	uint32_t dots = 0;
	uint32_t check_ms = 0;

	if (!device_is_ready(nor)) {
		us("pusher: NOR not ready\r\n");
		return -1;
	}

	for (uint32_t e = 0; e < need; e += esz) {
		if (flash_erase(nor, off + e, esz) != 0) {
			us("\r\npusher: erase failed at ");
			uhex(off + e);
			us("\r\n");
			return -1;
		}

		/* Blank-check this block, now, before erasing the next one.
		 *
		 * The erase path reports success on a status bit and nothing
		 * verifies that the bits actually went to 1. That gap matters
		 * because a NOR program can only clear bits: programming over a
		 * block that erased incompletely yields the bitwise AND of old
		 * and new, which is data that is present, reads back identically
		 * every time, and is wrong. After the fact that is
		 * indistinguishable from a bad write. Checking before a single
		 * payload byte is written separates them completely.
		 *
		 * Per block rather than in one pass at the end, changed
		 * 2026-08-09. One pass over a 54 MB rootfs slot meant a single
		 * sys_cache_data_invd_range() of 1.78 million cache-line
		 * operations and 14.2 million reads with nothing on the console
		 * throughout - and a 54 MB push is the one that takes the board
		 * down with no output. Per block it is 8192 line operations and
		 * 65536 reads between two printable points, so a hang inside it
		 * now lands on a block boundary this loop has already named.
		 * It also localises a bad block to the erase that produced it
		 * rather than to the whole span.
		 */
		if (IS_ENABLED(CONFIG_RVT_PUSHER_BLANK_CHECK)) {
			const uint32_t *p = (const uint32_t *)(base + e);
			int64_t t0 = k_uptime_get();
			uint32_t bad = 0, first_bad = UINT32_MAX;

			sys_cache_data_invd_range((void *)p, esz);

			for (uint32_t w = 0; w < esz / 4; w++) {
				if (p[w] != 0xFFFFFFFFU) {
					bad++;
					if (first_bad == UINT32_MAX) {
						first_bad = w * 4;
					}
				}
			}

			if (bad != 0) {
				us("\r\npusher: ERASE INCOMPLETE at block ");
				uhex(off + e);
				us(" -- ");
				udec(bad);
				us(" of ");
				udec(esz / 4);
				us(" words not 0xFFFFFFFF, first at flash ");
				uhex(off + e + first_bad);
				us("\r\n");
				return -1;
			}
			check_ms += (uint32_t)(k_uptime_get() - t0);
		}

		/* One dot per 256 KB block, ~899 ms of it measured on this part,
		 * and a newline every 16 MB so a full rootfs erase does not
		 * become one 200-character line. */
		up('.');
		if (++dots % 64 == 0) {
			us("\r\n");
		}
	}

	if (IS_ENABLED(CONFIG_RVT_PUSHER_BLANK_CHECK)) {
		us("\r\npusher: blank check ok, ");
		udec(check_ms);
		us(" ms over ");
		udec(dots);
		us(" blocks\r\n");
	}

	return 0;
}

/* Write the header, then verify the slot by reading it back.
 *
 * The header goes last on purpose: until it lands the slot has no valid magic,
 * so a transfer that died halfway through the payload leaves a slot that fails
 * img_check() rather than one that looks valid and is not.
 *
 * The whole IMG_PAYLOAD_OFF block is written in one program of 16 meaningful
 * bytes and 4080 of 0xFF, which is the first and only time anything programs
 * that region since the erase. It shares no 16-byte ECC unit with the payload,
 * which starts where this write ends. */
static int img_commit(int slot, uint32_t len, uint32_t crc)
{
	const struct device *nor = plat_flash_dev();
	uint32_t off = plat_slot_offset(slot, NULL);
	size_t slot_size;
	const uint8_t *base = plat_slot_base(slot, &slot_size);
	uint32_t span = ROUND_UP(len + IMG_PAYLOAD_OFF, plat_flash_erase_size());
	static uint8_t hdrblk[IMG_PAYLOAD_OFF];
	struct img_hdr h;
	uint32_t vlen = 0;
	int vrc = -1;
	int attempt;
	int wrc;

	memset(hdrblk, 0xFF, sizeof(hdrblk));
	memcpy(h.magic, img_slot_magic(slot), 8);
	h.len = len;
	h.crc = crc;
	memcpy(hdrblk, &h, sizeof(h));

	wrc = flash_write(nor, off, hdrblk, IMG_PAYLOAD_OFF);
	if (wrc != 0) {
		us("\r\npusher: header write failed rc=");
		udec((uint32_t)(-wrc));
		us(" wbs=");
		udec((uint32_t)flash_get_write_block_size(nor));
		us("\r\n");
		return -1;
	}

	/* The memory-mapped window is cacheable, so invalidate before verifying
	 * through it or we may read pre-erase lines. */
	sys_cache_data_invd_range((void *)base, span);

	/*
	 * Verify more than once before giving up. A CRC32 that matches is worth
	 * trusting however many attempts it took - matching by chance is one in
	 * four billion - so if any pass agrees, the payload in flash is right
	 * and an earlier disagreeing pass was a bad read, not bad data.
	 *
	 * The attempt count is the real product here: if pushes routinely need
	 * a second or third pass then reads are marginal, which is a far more
	 * specific finding than "verify failed".
	 *
	 * These are RE-READS. Nothing is rewritten between attempts, and nothing
	 * may be: a second program to a 16-byte unit that has not been erased in
	 * between corrupts it on this part. See FLASH_ECC_UNIT at the top of the
	 * file. A push that exhausts its attempts fails the whole push, and the
	 * host's retry erases the slot before it sends anything.
	 */
	for (attempt = 1; attempt <= CONFIG_RVT_PUSHER_VERIFY_ATTEMPTS; attempt++) {
		vrc = img_check(slot, &vlen, true);
		if (vrc == 0 && vlen == len) {
			break;
		}
		if (attempt < CONFIG_RVT_PUSHER_VERIFY_ATTEMPTS) {
			sys_cache_data_invd_range((void *)base, span);
		}
	}

	if (vrc == 0 && vlen == len) {
		if (attempt > 1) {
			us("pusher: verified on attempt ");
			udec((uint32_t)attempt);
			us(" -- earlier passes disagreed, reads look marginal\r\n");
		}
		return 0;
	}

	us("\r\npusher: verify FAILED rc=");
	udec((uint32_t)(-vrc));
	us(vrc == -1 ? " (magic)" : vrc == -2 ? " (length)" : " (crc)");
	us(" expected len=");
	udec(len);
	us(" crc=");
	uhex(crc);
	us("\r\n");
	img_diagnose(slot, len);
	return -1;
}

/* ---------------------------------------------------------- the flash writer
 *
 * Double buffered: the socket fills one buffer while the flash programs the
 * other. This is not where the time goes - programming is roughly four times
 * slower than the link, so the overlap hides the network behind the flash
 * rather than the other way around. What it buys is that the socket keeps
 * draining during a program cycle, so the TCP window never slams shut and the
 * sender never stalls mid-transfer.
 */

static uint8_t nl_buf[2][CONFIG_RVT_PUSHER_BUF];
/* volatile is insurance rather than a fix: the k_sem calls either side of these
 * are real function calls and already order them. */
static volatile uint32_t nl_blen[2];
static volatile uint32_t nl_boff[2];
static struct k_sem nl_free;      /* buffers the producer may fill */
static struct k_sem nl_filled;    /* buffers the writer must program */
static volatile int nl_wrc;       /* first write error, sticky */
static volatile uint32_t nl_wfail_off;   /* flash offset it failed at */
static volatile uint32_t nl_wr_off;      /* offset currently being written */
static volatile bool nl_wr_busy;         /* inside flash_write() right now */

/* Cleared at the start of every push; set by the writer the first time
 * flash_write() returns. One line, and it is the line that splits "died before
 * any payload write completed" from "died after at least one did" - which is
 * precisely the question the 54 MB failure could not answer. */
static volatile bool nl_first_done;

/* Set if a writer thread ever had to be abandoned. Its k_thread object and
 * stack are then permanently unsafe to reuse. */
static bool nl_writer_lost;

K_THREAD_STACK_DEFINE(nl_wstack, CONFIG_RVT_PUSHER_WRITER_STACK);
static struct k_thread nl_wtcb;

/* Started per push and joined at the end, so a writer can never outlive the
 * transfer that created it and program a stale buffer into the next one. A
 * zero-length buffer is the end sentinel.
 *
 * On failure it keeps draining rather than exiting: the producer is blocked on
 * nl_free, and a writer that stopped handing buffers back would deadlock it. */
static void nl_writer(void *a, void *b, void *c)
{
	const struct device *nor = plat_flash_dev();

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
		    flash_write(nor, nl_boff[w], (const void *)nl_buf[w],
				nl_blen[w]) != 0) {
			/* Keep the address. "write failed" without one leaves
			 * no way to tell a transport problem from an
			 * offset-dependent flash problem. */
			nl_wfail_off = nl_boff[w];
			nl_wrc = -EIO;
		}
		nl_wr_busy = false;
		if (!nl_first_done) {
			nl_first_done = true;
			us("pusher: first flash_write returned\r\n");
		}
		k_sem_give(&nl_free);
	}
}

/* ------------------------------------------------------------ socket helpers
 *
 * Deliberately not telnet.c's tn_send_all(): that lives behind
 * CONFIG_RVT_TELNET, and the loader has no reason to stop working when the
 * console bridge is off.
 */

static int nl_send_str(int s, const char *msg)
{
	const uint8_t *p = (const uint8_t *)msg;
	size_t n = strlen(msg);

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

/* Announce the slot table before reading anything.
 *
 * The board is the authority on where its slots are, and the host tool carries
 * its own copy of those numbers. Sending them lets the host refuse to push
 * against a firmware whose layout it does not recognise - which matters because
 * the failure that prevents is "erased the wrong region", and no amount of
 * retrying fixes that one.
 *
 * Format is fixed by pushimage.py: "RA8LDR 1 name:off:cap ...", slots in the
 * order the tool indexes them, so kernel must be first. */
static int nl_send_banner(int s)
{
	char line[128];
	int n = 0;

	n += snprintk(line + n, sizeof(line) - n, "RA8LDR 1");
	for (int i = 0; i < PLAT_SLOT_COUNT; i++) {
		n += snprintk(line + n, sizeof(line) - n, " %s:%u:%u",
			      img_slot_name(i), plat_slot_offset(i, NULL),
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
 * setsockopt fails and the socket silently keeps blocking forever. On the
 * rvlinux board that wedged the service until a reset - a listener stuck in
 * recv() never gets back to accept(), and every later connection then sits
 * unaccepted in the backlog looking, from the host, like a dead board.
 *
 * Returns bytes read, 0 on timeout, -1 on error or orderly close.
 */
static int nl_recv(int s, uint8_t *p, size_t n, int timeout_s)
{
	struct zsock_pollfd pfd = { .fd = s, .events = ZSOCK_POLLIN };
	int pr = zsock_poll(&pfd, 1, timeout_s * 1000);
	ssize_t got;

	if (pr == 0) {
		return 0;                       /* timed out */
	}
	if (pr < 0 || (pfd.revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP |
				      ZSOCK_POLLNVAL))) {
		return -1;
	}

	got = zsock_recv(s, p, n, 0);
	return got > 0 ? (int)got : -1;
}

/* Read exactly n bytes, or fail. Used for the fixed-size header only. */
static int nl_recv_exact(int s, uint8_t *p, size_t n)
{
	while (n != 0) {
		int got = nl_recv(s, p, n, CONFIG_RVT_PUSHER_RX_TIMEOUT_S);

		if (got <= 0) {
			return -1;
		}
		p += got;
		n -= (size_t)got;
	}
	return 0;
}

/* Send "BLKCRC <blocksize> <nblocks>", one hex CRC per block, then "ENDCRC".
 *
 * The board cannot do the comparison - it has never seen the original file -
 * but it is the comparison that matters, because *where* the corruption sits is
 * the discriminator: a contiguous tail says the writes stopped landing, a
 * scattered handful says individual operations are failing, one bad block in an
 * otherwise perfect image says something different again. A few KB of CRCs buys
 * that without reading 36 MB back over the socket. */
static void nl_send_blkcrc(int s, int slot, uint32_t len)
{
	size_t slot_size;
	const uint8_t *body = plat_slot_base(slot, &slot_size) + IMG_PAYLOAD_OFF;
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

		/* Flush a whole line rather than per block: 16 CRCs is one send
		 * instead of sixteen, and the host parses whitespace. */
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

/* ------------------------------------------------------------------- a push */

/* Set once a push has committed successfully, so the accept loop knows to
 * reboot the board when the operator stops pushing. `reboot_at` is pushed
 * forward again by every connection that arrives inside the window. */
static bool reboot_armed;
static int64_t reboot_at;

static void arm_reboot(void)
{
	reboot_at = k_uptime_get() +
		    (int64_t)CONFIG_RVT_PUSHER_REBOOT_DELAY_S * 1000;
}

static int nl_push(int s)
{
	uint8_t hdr[10];   /* 'S', slot, len32le, crc32le */
	uint32_t len, crc, off = 0, slot_off;
	int slot;
	struct zsock_timeval rcvto = {
		.tv_sec = CONFIG_RVT_PUSHER_RX_TIMEOUT_S,
		.tv_usec = 0,
	};
	int one = 1;
	int64_t t0, t_rx;
	bool failed = false;
	bool stalled = false;
	int i = 0;

	/* Belt to nl_recv()'s braces; the poll is what actually enforces the
	 * timeout. The return is reported rather than discarded - swallowing it
	 * is precisely how a missing CONFIG_NET_CONTEXT_RCVTIMEO stayed
	 * invisible while it wedged the rvlinux service. */
	if (zsock_setsockopt(s, ZSOCK_SOL_SOCKET, ZSOCK_SO_RCVTIMEO,
			     &rcvto, sizeof(rcvto)) < 0) {
		us("pusher: note, SO_RCVTIMEO unavailable; poll timeout in use\r\n");
	}
	(void)zsock_setsockopt(s, ZSOCK_SOL_SOCKET, ZSOCK_SO_KEEPALIVE,
			       &one, sizeof(one));

	if (nl_writer_lost) {
		us("pusher: refusing push, writer thread was abandoned\r\n");
		nl_send_str(s, "ERR loader disabled, reset the board\n");
		return -1;
	}

	if (nl_send_banner(s) != 0) {
		/* Seen on hardware: the connection right after an aborted
		 * multi-MB push can fail here while the dead connection's
		 * buffers are still tearing down, and without this line the
		 * console says "connection from X" and then nothing - the
		 * host sees an empty read and cannot tell a dead pusher from
		 * a busy one. */
		us("pusher: banner send failed; likely transient buffer "
		   "exhaustion, connect again\r\n");
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

	if (slot < 0 || slot >= PLAT_SLOT_COUNT) {
		nl_send_str(s, "ERR bad slot\n");
		return -1;
	}
	slot_off = plat_slot_offset(slot, NULL);

	us("pusher: slot=");
	us(img_slot_name(slot));
	us(" len=");
	udec(len);
	us(" crc=");
	uhex(crc);
	us("\r\n");

	/* Bounded before anything is stopped or erased, so a bad header leaves
	 * both the stored image and the running guest alone. */
	if (img_accept_len(slot, len) != 0) {
		nl_send_str(s, "ERR length out of range\n");
		return -1;
	}

	/* This is the commit point: the header is valid, the sender means it,
	 * and from here the guest cannot survive anyway because its rootfs is
	 * about to be erased underneath it. */
	if (!halt_guest()) {
		nl_send_str(s, "ERR guest would not stop, reset the board\n");
		return -1;
	}

	if (nl_send_str(s, "OK header\n") != 0) {
		return -1;
	}

	t0 = k_uptime_get();
	us("pusher: erasing ");
	udec(ROUND_UP(len + IMG_PAYLOAD_OFF, plat_flash_erase_size()) / 1024);
	us(" KB\r\n");

	if (img_erase(slot, len) != 0) {
		nl_send_str(s, "ERR erase failed\n");
		return -1;
	}

	us("pusher: erased in ");
	udec((uint32_t)(k_uptime_get() - t0));
	us(" ms\r\n");
	if (nl_send_str(s, "OK erased\n") != 0) {
		return -1;
	}

	k_sem_init(&nl_free, 2, 2);
	k_sem_init(&nl_filled, 0, 2);
	nl_wrc = 0;
	nl_wfail_off = 0;
	nl_first_done = false;

	k_thread_create(&nl_wtcb, nl_wstack, K_THREAD_STACK_SIZEOF(nl_wstack),
			nl_writer, NULL, NULL, NULL,
			CONFIG_RVT_PUSHER_WRITER_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&nl_wtcb, "imgwrite");

	/* The last line before the payload phase, and the first thing to look
	 * for when a push dies early: it proves the erase, the blank check and
	 * the writer thread creation are all behind us, which the previous
	 * silent gap between "OK erased" and the 1 MB mark did not. */
	us("pusher: payload starts at flash ");
	uhex(slot_off + IMG_PAYLOAD_OFF);
	us(", ");
	udec(len);
	us(" bytes in ");
	udec((len + CONFIG_RVT_PUSHER_BUF - 1) / CONFIG_RVT_PUSHER_BUF);
	us(" buffers\r\n");

	t_rx = k_uptime_get();

	/* Every exit from this loop happens while holding buffer `i`, which is
	 * what lets the sentinel below always have a buffer to go in. */
	for (;;) {
		uint32_t want, have = 0;

		/* Bounded, for the same reason the recv is bounded and it is the
		 * same bug: this thread stops draining the socket while it waits
		 * here, so a writer that never hands a buffer back closes the TCP
		 * window and the host's send() blocks partway through a transfer
		 * that was progressing normally. From the outside that looks like
		 * a mid-stream stall with a healthy board and produces no console
		 * output at all.
		 *
		 * A buffer should come back in ~35 ms (8 KB of 64-byte page
		 * programs). Waiting seconds means the flash operation is not
		 * returning, which FSP can do: several of its register waits are
		 * unbounded spins with no timeout of their own. */
		if (k_sem_take(&nl_free,
			       K_MSEC(CONFIG_RVT_PUSHER_STALL_MS)) != 0) {
			us("pusher: STALLED, no buffer back for ");
			udec(CONFIG_RVT_PUSHER_STALL_MS);
			us(" ms; writer ");
			us(nl_wr_busy ? "still inside flash_write at "
				      : "idle, last write at ");
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
		 * programming whatever one recv() happened to return. Writes then
		 * land in large aligned runs instead of TCP-segment-sized
		 * dribbles, which matters when each one costs a program-and-poll
		 * cycle per 64 bytes. */
		want = MIN((uint32_t)CONFIG_RVT_PUSHER_BUF, len - off);
		while (have < want) {
			int got = nl_recv(s, (uint8_t *)nl_buf[i] + have,
					  want - have,
					  CONFIG_RVT_PUSHER_RX_TIMEOUT_S);

			if (got <= 0) {
				us(got == 0 ? "pusher: rx timeout\r\n"
					    : "pusher: rx error\r\n");
				failed = true;
				break;
			}
			have += (uint32_t)got;
		}
		if (failed) {
			break;
		}

		/*
		 * Pad the last program up to a 16-byte ECC unit with 0xFF.
		 *
		 * Only the final chunk can be short - `want` is the whole buffer
		 * until then - so this runs once per push and adds at most 15
		 * bytes past the image, into a region that was erased anyway and
		 * that nothing reads: the verify CRCs exactly `len` bytes.
		 *
		 * It is not strictly required today, because nothing programs
		 * past the payload and so the half-filled tail unit would never
		 * be touched a second time. It is here so that stays true by
		 * construction rather than by argument, given what a second
		 * program to a unit does on this part (see the top of the file).
		 * ROUND_UP cannot overflow the buffer: a short chunk is at most
		 * CONFIG_RVT_PUSHER_BUF - 1, and the buffer size is a multiple
		 * of the unit.
		 */
		nl_blen[i] = ROUND_UP(have, FLASH_ECC_UNIT);
		if (nl_blen[i] != have) {
			memset(nl_buf[i] + have, 0xFF, nl_blen[i] - have);
		}
		nl_boff[i] = slot_off + IMG_PAYLOAD_OFF + off;
		k_sem_give(&nl_filled);
		/* The unpadded count: `off` is a position in the image, and the
		 * padding is not part of it. */
		off += have;
		i ^= 1;

		/*
		 * Progress, on the UART rather than the socket, at three
		 * granularities. If a push stalls, the last mark printed says
		 * how far it got and at what flash address.
		 *
		 * The first 128 KB is reported per buffer. That is not
		 * excessive: BOTH 54 MB failures died at or before 0.06 MB -
		 * about seven buffers - and the old code's first mark was at
		 * 1 MB, so the console said nothing at all about either of them.
		 * A 64 KB granularity would still have missed a death at 62 KB.
		 * The blind spot was the whole finding, so it is closed with
		 * room to spare rather than exactly.
		 *
		 * 1 MB rather than 4 for the long run: the first stall seen on
		 * rvlinux was at 2.38 MB and fell between two 4 MB marks.
		 */
		if (off <= (128U << 10) ||
		    (off < (1U << 20) && (off & ((64U << 10) - 1)) <
					 CONFIG_RVT_PUSHER_BUF) ||
		    (off & ((1U << 20) - 1)) < CONFIG_RVT_PUSHER_BUF) {
			us("pusher: ");
			if (off < (1U << 20)) {
				udec(off >> 10);
				us(" KB");
			} else {
				udec(off >> 20);
				us(" MB");
			}
			us(" at flash ");
			uhex(slot_off + IMG_PAYLOAD_OFF + off);
			us("\r\n");
		}
	}

	if (stalled) {
		/* The stall path is the one case where we do not own buffer i,
		 * so there is nowhere to put the sentinel and the writer cannot
		 * be joined - it is wedged inside the flash driver. Abandon it
		 * and refuse later pushes rather than reusing a thread object
		 * that is still live, which would corrupt the scheduler.
		 *
		 * Recovering the OSPI controller from here needs a reset, and
		 * the guest is stopped anyway, so the board takes itself down
		 * once the message is on the wire. */
		nl_writer_lost = true;
		us("pusher: writer abandoned, the OSPI controller needs a reset\r\n");
		nl_send_str(s, "ERR writer stalled, reset the board\n");
		return -1;
	}

	nl_blen[i] = 0;
	k_sem_give(&nl_filled);

	if (k_thread_join(&nl_wtcb, K_MSEC(CONFIG_RVT_PUSHER_STALL_MS)) != 0) {
		nl_writer_lost = true;
		us("pusher: writer did not exit, loader disabled until reset\r\n");
		nl_send_str(s, "ERR writer stalled, reset the board\n");
		return -1;
	}

	/* Flash first: a write failure also stops the producer, so it would
	 * otherwise be reported as the short transfer it causes rather than as
	 * the cause. */
	if (nl_wrc != 0) {
		us("pusher: flash write failed at ");
		uhex(nl_wfail_off);
		us(" (payload offset ");
		udec(nl_wfail_off - slot_off - IMG_PAYLOAD_OFF);
		us(")\r\n");
		nl_send_str(s, "ERR flash write failed\n");
		return -1;
	}
	if (failed || off != len) {
		us("pusher: transfer died at payload offset ");
		udec(off);
		us(" of ");
		udec(len);
		us(", flash ");
		uhex(slot_off + IMG_PAYLOAD_OFF + off);
		us("\r\n");
		nl_send_str(s, "ERR short transfer\n");
		return -1;
	}

	us("pusher: ");
	udec(len);
	us(" bytes in ");
	udec((uint32_t)(k_uptime_get() - t_rx));
	us(" ms\r\n");

	if (img_commit(slot, len, crc) != 0) {
		nl_send_blkcrc(s, slot, len);
		nl_send_str(s, "ERR verify failed\n");
		return -1;
	}

	us("pusher: ");
	us(img_slot_name(slot));
	us(" committed and verified\r\n");
	nl_send_str(s, "OK committed\n");
	reboot_armed = true;
	return 0;
}

/* ------------------------------------------------------------- the listener */

static void pusher_reboot(const char *why)
{
	us("\r\npusher: ");
	us(why);
	us("; rebooting\r\n");
	/* plat_uart_putc() is uart_poll_out(), which does not return until the
	 * byte is on the wire, so the line above is already sent. The sleep is
	 * for the socket: give the stack a moment to push the last status line
	 * out before the MAC stops existing. */
	k_msleep(200);
	sys_reboot(SYS_REBOOT_COLD);
}

static void nl_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (!have_addr()) {
		k_msleep(250);
	}

	for (;;) {
		struct net_sockaddr_in sa = {
			.sin_family = NET_AF_INET,
			.sin_port = net_htons(CONFIG_RVT_PUSHER_PORT),
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

		for (;;) {
			struct zsock_pollfd pfd = { .fd = ls,
						    .events = ZSOCK_POLLIN };
			struct net_sockaddr_in peer;
			net_socklen_t plen = sizeof(peer);
			int wait = -1;
			int pr, cs;

			/*
			 * Wait for a client, or for the deferred reboot to come
			 * due. Polling rather than blocking in accept() is the
			 * whole mechanism: a connection that arrives inside the
			 * window pushes the deadline out again, so a two-slot
			 * push runs to completion and the board reboots when the
			 * operator has finished rather than between their two
			 * commands.
			 */
			if (reboot_armed && CONFIG_RVT_PUSHER_REBOOT_DELAY_S > 0) {
				int64_t left = reboot_at - k_uptime_get();

				if (left <= 0) {
					pusher_reboot("image pushed and no "
						      "further connection");
				}
				wait = (int)left;
			}

			pr = zsock_poll(&pfd, 1, wait);
			if (pr == 0) {
				continue;       /* deadline check above */
			}
			/* Anything other than a readable listener means the
			 * socket is finished. Falling through to accept() would
			 * block there forever, and with it the deferred reboot -
			 * so this is the one revents check that has to be here
			 * rather than left to accept()'s return. */
			if (pr < 0 || (pfd.revents & (ZSOCK_POLLERR |
						      ZSOCK_POLLHUP |
						      ZSOCK_POLLNVAL))) {
				break;          /* rebuild the listener */
			}

			cs = zsock_accept(ls, (struct net_sockaddr *)&peer,
					  &plen);
			if (cs < 0) {
				break;          /* rebuild the listener */
			}

			us("pusher: connection from ");
			print_ipv4(&peer.sin_addr);
			us("\r\n");

			(void)nl_push(cs);
			zsock_close(cs);

			if (nl_writer_lost) {
				/* The OSPI controller only comes back with a
				 * reset, and the host has been told so. */
				pusher_reboot("flash writer wedged");
			}
			if (reboot_armed && CONFIG_RVT_PUSHER_REBOOT_DELAY_S > 0) {
				arm_reboot();
				us("pusher: rebooting into the new image in ");
				udec(CONFIG_RVT_PUSHER_REBOOT_DELAY_S);
				us(" s unless another push arrives\r\n");
			} else if (reboot_armed) {
				us("pusher: image committed; reset the board to "
				   "boot it\r\n");
			} else if (guest_stopped) {
				us("pusher: guest is stopped; push again or "
				   "reset the board\r\n");
			}
		}

		zsock_close(ls);
	}
}

K_THREAD_STACK_DEFINE(nl_stack, CONFIG_RVT_PUSHER_THREAD_STACK);
static struct k_thread nl_tcb;

void rvt_pusher_start(void)
{
	k_thread_create(&nl_tcb, nl_stack, K_THREAD_STACK_SIZEOF(nl_stack),
			nl_thread, NULL, NULL, NULL,
			CONFIG_RVT_PUSHER_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&nl_tcb, "pusher");
}
