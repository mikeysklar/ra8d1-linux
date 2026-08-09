/*
 * MMU-capable RISC-V Linux on the Renesas EK-RA8D1 (Cortex-M85 @ 480 MHz).
 *
 * Runs TinyEMU's RV32 core - Sv32 paging, S-mode, a 256-entry softmmu TLB -
 * over the board's 64 MB external SDRAM. The point of the MMU is not the MMU:
 * Buildroot gates python3 on BR2_USE_MMU, so an MMU guest is the shortest
 * path to Blinka on this board. See ra8d1-linux/notes/sv32-mmu.md for how
 * that was decided and notes/00-port.md for this port.
 *
 * The guest boots straight into S-mode. There is no OpenSBI or BBL image
 * underneath it; SBI is implemented in host C in emu/rv_hostsbi.c.
 *
 * Host memory map:
 *   0x68000000  64 MB SDRAM   guest RAM (the guest sees it at 0x80000000)
 *   0x90000000  64 MB OSPI    memory-mapped for reads
 *   0x90040000  kernel slot   "RA8LINUX"
 *   0x90840000  rootfs slot   "RA8ROOTF", read in place as virtio-blk vda
 *
 * The rootfs is never copied into SDRAM. It is handed to the block device as
 * a pointer into the OSPI window, which is what makes a 55 MB root filesystem
 * possible on a board with 64 MB of RAM that the kernel already wants 26 of.
 *
 * With an empty kernel slot the app boots a compiled-in RV32 self-test
 * instead (src/testrom.h), so a freshly flashed board says something true
 * about the emulator without anyone having to push an image first.
 */

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/crc.h>

#include <string.h>

#include "image.h"
#include "pusher.h"
#include "rv_hostsbi.h"
#include "rv_machine.h"
#include "rv_platform.h"
#include "rv_virtio.h"
#include "netbridge.h"
#include "telnet.h"
#include "testrom.h"

/* The on-flash image container lives in src/image.h, because src/pusher.c
 * writes what this file reads and the two must agree byte for byte - it is the
 * header the host tool parses. */
BUILD_ASSERT(sizeof(struct img_hdr) == IMG_HDR_SIZE,
	     "header must match what the rvlinux pusher writes");

/*
 * RISC-V Linux Image header magic (arch/riscv/include/asm/image.h), at +56.
 *
 * Checked before booting anything out of flash, because the kernel slot on
 * this board may well contain the *nommu* rv32ima image the rvlinux app uses.
 * That image carries the same "RA8LINUX" slot magic and a valid CRC, but it is
 * a machine-mode nommu kernel: this machine would enter it in S-mode and it
 * would die somewhere unhelpful. One four-byte comparison turns that from a
 * mystery into a sentence on the console.
 */
#define RV_IMAGE_MAGIC2     0x05435352u  /* "RSC\x05" */
#define RV_IMAGE_MAGIC2_OFF 56U
#define RV_IMAGE_TEXTOFF_OFF 8U

/*
 * On rv32, arch/riscv/kernel/head.S sets text_offset to 0 for a
 * CONFIG_RISCV_M_MODE build and 0x400000 otherwise. That makes it a reliable
 * marker for the nommu, machine-mode kernel the rvlinux app boots - which
 * carries the same slot magic, the same Image magic and a perfectly good CRC,
 * and which this machine would enter in S-mode and hang. Found the hard way:
 * the first boot on real silicon did exactly that.
 */
#define RV_IMAGE_MMODE_TEXT_OFFSET 0

/* ------------------------------------------------------------------ output
 *
 * The application's own banner and diagnostics, always on the physical UART.
 * Not plat_putc(): that is the *guest's* console and follows a telnet client
 * when one is attached, which is exactly where these lines must not go.
 */

static void up(char c)
{
	plat_uart_putc(c);
}

static void us(const char *s)
{
	plat_uart_puts(s);
}

static void udec(uint64_t v)
{
	char buf[24];
	int n = 0;

	if (v == 0) {
		up('0');
		return;
	}
	while (v > 0 && n < (int)sizeof(buf)) {
		buf[n++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (n-- > 0) {
		up(buf[n]);
	}
}

static void uhex(uint32_t v)
{
	int i;

	us("0x");
	for (i = 28; i >= 0; i -= 4) {
		uint32_t d = (v >> i) & 0xf;

		up((char)(d < 10 ? '0' + d : 'a' + d - 10));
	}
}

/* ------------------------------------------------------------- slot check */

/*
 * Validate one slot in place, straight out of the memory-mapped flash window.
 * Returns the payload pointer and sets *len, or NULL.
 *
 * The CRC costs one pass over the image at boot - about 26 MB for the kernel
 * and 55 for the rootfs, which is not free. It is still worth it for the
 * kernel: a half-written image otherwise shows up as a crash somewhere
 * unrelated, which is far more expensive to debug than a slow boot.
 *
 * For the rootfs it is optional and off by default (CONFIG_RVT_CRC_ROOTFS),
 * because the failure it catches is one the filesystem also catches, and 55 MB
 * of OSPI reads on every boot is a real cost for a second opinion.
 */
static const uint8_t *slot_check(int slot, uint32_t *len, uint32_t *ms,
				 bool verify)
{
	size_t slot_size;
	const uint8_t *base = plat_slot_base(slot, &slot_size);
	const struct img_hdr *h;
	const uint8_t *payload;
	int64_t t0;

	*ms = 0;
	if (base == NULL || slot_size <= IMG_PAYLOAD_OFF) {
		return NULL;
	}
	h = (const struct img_hdr *)base;
	if (memcmp(h->magic, img_slot_magic(slot), 8) != 0) {
		return NULL;
	}
	if (h->len == 0 || h->len > slot_size - IMG_PAYLOAD_OFF) {
		return NULL;
	}

	payload = base + IMG_PAYLOAD_OFF;
	if (verify) {
		t0 = k_uptime_get();
		if (crc32_ieee(payload, h->len) != h->crc) {
			return NULL;
		}
		*ms = (uint32_t)(k_uptime_get() - t0);
	}
	*len = h->len;
	return payload;
}

/* -------------------------------------------------------------------- main */

int main(void)
{
	RVBootImage img;
	size_t ram_size;
	uint32_t klen = 0, rlen = 0, kms = 0, rms = 0;
	const uint8_t *kernel, *rootfs;
	uint64_t insns;
	int64_t t0;
	uint32_t sbi_calls, sbi_unsupported;
	int ret;

	/* Before the first banner byte, so nothing typed at the console is
	 * lost while the CRCs run. */
	plat_console_init();

	us("\r\n\r\nra8d1-tinyemu: rv32 Sv32 Linux host\r\n");
	us("host: cortex-m85 @ ");
	udec(sys_clock_hw_cycles_per_sec() / 1000000U);
	us(" MHz, sdram ");
	(void)plat_guest_ram(&ram_size);
	udec(ram_size / (1024 * 1024));
	us(" MB\r\n");

	/*
	 * Network first, before the image CRCs and long before the guest, so
	 * the address is printed while the console is still quiet. Both calls
	 * are no-ops when networking is off.
	 */
	rvt_net_bringup();
	rvt_telnet_start();
	/* Promiscuous RX on, filter thread up, before the guest boots - its
	 * first DHCP DISCOVER should find a wire already listening. */
	rvt_netbridge_start();

	/*
	 * The image loader, on its own port. Started here rather than after the
	 * slot checks so that a board holding an image it cannot boot is still
	 * reachable to be re-imaged - which is the case where a pusher is worth
	 * the most. It waits for an address of its own accord.
	 */
	rvt_pusher_start();

	/*
	 * Everything from here on either CRCs megabytes out of the OSPI window
	 * or runs guest instructions, and neither blocks nor yields. Zephyr
	 * schedules preemptible threads strictly by priority, so at the default
	 * priority 0 this thread would starve the Ethernet RX thread (2), the
	 * TCP worker (2) and the telnet thread (5) -- for the whole life of the
	 * guest, which is to say forever. Drop below all of them and let the
	 * network stack preempt at will; a few microseconds of stolen guest
	 * time per packet is unmeasurable, a dropped packet is not.
	 *
	 * Dropped before the image check rather than just before the step loop
	 * because a full-slot CRC is seconds of uninterrupted work on its own.
	 */
	rvt_emu_priority_drop();

	memset(&img, 0, sizeof(img));

	kernel = IS_ENABLED(CONFIG_RVT_FORCE_SELFTEST)
			 ? NULL
			 : slot_check(PLAT_SLOT_KERNEL, &klen, &kms, true);
	rootfs = slot_check(PLAT_SLOT_ROOTFS, &rlen, &rms,
			    IS_ENABLED(CONFIG_RVT_CRC_ROOTFS));

	/*
	 * A valid slot is not the same as a bootable kernel. Reject anything
	 * without the RISC-V Image magic rather than jumping into it.
	 */
	if (kernel != NULL) {
		uint32_t magic2 = 0;

		if (klen >= 64) {
			memcpy(&magic2, kernel + RV_IMAGE_MAGIC2_OFF,
			       sizeof(magic2));
		}
		if (magic2 == RV_IMAGE_MAGIC2 && klen >= 64) {
			uint32_t toff = 0;

			memcpy(&toff, kernel + RV_IMAGE_TEXTOFF_OFF,
			       sizeof(toff));
			if (toff == RV_IMAGE_MMODE_TEXT_OFFSET) {
				us("kernel: slot holds ");
				udec(klen);
				us(" B, a valid rv32 Image but an M-mode"
				   " (nommu) build -- that is the rvlinux"
				   " guest, and this machine boots S-mode"
				   " kernels\r\n");
				kernel = NULL;
			}
		}

		if (kernel != NULL && magic2 != RV_IMAGE_MAGIC2) {
			us("kernel: slot holds ");
			udec(klen);
			us(" B with a good CRC, but ");
			if (klen >= 2 && kernel[0] == 0x1f && kernel[1] == 0x8b) {
				/* The rvlinux pusher sends Image.gz, because a
				 * raw 25.9 MB Image does not fit an 8 MB slot.
				 * Nothing here decompresses yet. */
				us("it is gzipped and this app has no"
				   " decompressor");
			} else {
				us("it has no RISC-V Image header -- most"
				   " likely the nommu image from the rvlinux"
				   " app, which this machine cannot boot");
			}
			us("\r\n");
			kernel = NULL;
		}
	}

	if (kernel != NULL) {
		img.kernel = kernel;
		img.kernel_size = klen;
		us("kernel: ");
		udec(klen);
		us(" B, crc ok in ");
		udec(kms);
		us(" ms\r\n");
	} else {
		/*
		 * Nothing bootable in flash. Rather than stop, run the
		 * compiled-in RV32 self-test, which exercises the CPU, the
		 * SBI layer, the generated devicetree and the Sv32 walk and
		 * prints a verdict. On a board nobody has pushed to yet that
		 * is the most useful thing this app can do.
		 */
		us("kernel: no valid image in flash; running the built-in "
		   "self-test instead\r\n");
		img.kernel = testrom;
		img.kernel_size = sizeof(testrom);
		img.cmdline = "";
		rootfs = NULL;
	}

	if (rootfs != NULL && img.kernel != testrom) {
		img.rootfs = rootfs;
		img.rootfs_size = rlen;
		us("rootfs: ");
		udec(rlen / (1024 * 1024));
		us(" MB, read in place from OSPI");
		if (rms != 0) {
			us(", crc ok in ");
			udec(rms);
			us(" ms");
		}
		us("\r\n");
	} else if (img.kernel != testrom) {
		us("rootfs: none; the guest will need a root= it can reach\r\n");
	}

	if (img.cmdline == NULL) {
		img.cmdline = CONFIG_RVT_CMDLINE;
	}

	ret = rv_machine_init(&img);
	if (ret != 0) {
		us("rv_machine_init failed: errno ");
		udec((uint32_t)(-ret));
		us("\r\n");
		/*
		 * No guest will ever run, so tell the pusher the board is
		 * already quiescent. Without this a push would wait out its
		 * halt timeout and refuse - on exactly the board that most
		 * needs a new image.
		 */
		(void)rvt_pusher_guest_halted();
		return 0;
	}

	{
		uint32_t fdt_size = 0;
		uint32_t fdt_addr = rv_machine_fdt(&fdt_size);

		us("guest: ram 0x80000000+");
		udec(ram_size / (1024 * 1024));
		us(" MB, dtb at ");
		uhex(fdt_addr);
		us(" (");
		udec(fdt_size);
		us(" B)");
		if (rv_virtio_blk_sectors() != 0) {
			us(", vda ");
			udec(rv_virtio_blk_sectors());
			us(" sectors");
		}
		us("\r\n--- guest output follows ---\r\n");
	}

	t0 = k_uptime_get();
	ret = rv_machine_run();
	insns = rv_machine_insns();

	/*
	 * Whatever stopped the guest, tell the pusher: it may be blocked
	 * waiting for exactly this, because the rootfs is read in place out of
	 * the flash a push has to erase. It answers true when the stop was its
	 * doing, which is not a poweroff and should not be reported as one.
	 */
	if (rvt_pusher_guest_halted()) {
		us("\r\n--- guest halted for an image push ---\r\n");
	} else {
		us("\r\n--- guest stopped (");
		us(ret == 1 ? "reboot" : "poweroff");
		us(") ---\r\n");
		if (ret == 1) {
			plat_reboot();
		} else {
			plat_poweroff();
		}
	}

	riscv_host_sbi_stats(&sbi_calls, &sbi_unsupported);
	us("insns ");
	udec(insns);
	us(" in ");
	udec((uint64_t)(k_uptime_get() - t0));
	us(" ms, sbi calls ");
	udec(sbi_calls);
	us(" (");
	udec(sbi_unsupported);
	us(" unsupported), blk requests ");
	udec(rv_virtio_blk_requests());
	us("\r\n");
	return 0;
}
