/*
 * platform_zephyr.c - the Zephyr implementation of src/platform.h.
 *
 * This is the ONLY file in the project that is allowed to know what board it
 * is running on, and even here the board facts come from devicetree rather
 * than from constants:
 *
 *   console        DT_CHOSEN(zephyr_console)
 *   ROM slot       the "arcade-rom" fixed-partition: device, offset and size
 *   XIP window     DT_REG_ADDR of the flash node the partition lives on
 *   erase/write    the flash driver's own page layout and write-block size
 *
 * Porting to another Zephyr board should mean writing a devicetree overlay
 * that declares an "arcade-rom" partition, not editing this file. If the
 * board's flash is not memory-mapped, plat_rom_base() falls back to reading
 * the image into RAM and callers cannot tell.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/cache.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <errno.h>

#include "platform.h"

/* ------------------------------------------------------------------- board */

#define ROM_PARTITION       arcade_rom_partition

/* DT_MTD_FROM_FIXED_PARTITION walks partition -> partitions -> flash node,
 * so the device, its XIP window and the slot bounds all come from the same
 * devicetree entry and cannot drift apart. */
#define ROM_FLASH_NODE      DT_MTD_FROM_FIXED_PARTITION(DT_NODELABEL(ROM_PARTITION))
#define ROM_FLASH_DEV       DEVICE_DT_GET(ROM_FLASH_NODE)
#define ROM_OFFSET          DT_REG_ADDR(DT_NODELABEL(ROM_PARTITION))
#define ROM_SIZE            DT_REG_SIZE(DT_NODELABEL(ROM_PARTITION))

/* The flash node's own reg gives the memory-mapped (XIP) base and the part
 * size. On this board that is 0x90000000 and 64 MB, but taking it from the
 * node means a different part or a different chip select needs no edit. */
#define FLASH_XIP_BASE      DT_REG_ADDR(ROM_FLASH_NODE)

/* Largest erase block the part advertises, used to round the erase up. */
#define ROM_ERASE_SZ        (256U * 1024U)

/* Small flash_write() calls fail on this NOR, so writes are buffered into
 * whole aligned blocks. See ra8d1-linux/ra8d-bringup.md. */
#define ROM_WRITE_BLK       4096U

/*
 * Devicetree sanity, checked at build time so a bad overlay is a compile
 * error rather than a confusing runtime failure. These are invariants, not
 * addresses: asserting specific values here would just move the board
 * constants back into C, which is what the devicetree is for.
 */
BUILD_ASSERT(ROM_OFFSET % ROM_ERASE_SZ == 0,
	     "arcade-rom partition must start on an erase-block boundary");
BUILD_ASSERT(ROM_SIZE % ROM_ERASE_SZ == 0,
	     "arcade-rom partition must be a whole number of erase blocks");
BUILD_ASSERT(ROM_SIZE >= 64 * 1024,
	     "arcade-rom partition is too small to hold a ROM image");
BUILD_ASSERT(ROM_ERASE_SZ % ROM_WRITE_BLK == 0,
	     "write block must divide the erase block");

static const struct device *const console_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static const struct device *const rom_flash = ROM_FLASH_DEV;

/* ----------------------------------------------------------------- console */

void plat_putc(char c)
{
	uart_poll_out(console_dev, c);
}

void plat_puts(const char *s)
{
	while (*s) {
		uart_poll_out(console_dev, *s++);
	}
}

int plat_getc(void)
{
	unsigned char c;

	return (uart_poll_in(console_dev, &c) == 0) ? (int) c : -1;
}

int plat_getc_timeout(uint32_t timeout_ms)
{
	int64_t end = k_uptime_get() + timeout_ms;
	int c;

	do {
		c = plat_getc();
		if (c >= 0) {
			return c;
		}
		k_busy_wait(50);
	} while (k_uptime_get() < end);

	return -1;
}

/* ------------------------------------------------------------------ timing */

uint64_t plat_now_us(void)
{
	return (uint64_t) k_ticks_to_us_floor64(k_uptime_ticks());
}

void plat_sleep_until_us(uint64_t deadline_us)
{
	for (;;) {
		uint64_t now = plat_now_us();

		if (now >= deadline_us) {
			return;
		}
		/* Long waits go to the scheduler; the last stretch spins so the
		 * frame does not overshoot by a whole tick. */
		if (deadline_us - now > 2000) {
			k_sleep(K_USEC(deadline_us - now - 1000));
		} else {
			k_busy_wait(20);
		}
	}
}

/* ------------------------------------------------------------------- input */

/*
 * Nothing is wired up yet. The board's two user buttons are the obvious
 * first move, and USB HID after that; both belong here rather than in the
 * machine layer. Returning 0 is a valid configuration - the machine simply
 * sees an idle cabinet.
 */
uint32_t plat_poll_input(void)
{
	return 0;
}

/* ---------------------------------------------------------------- rom store */

const uint8_t *plat_rom_base(size_t *size)
{
	if (size) {
		*size = ROM_SIZE;
	}

	/* This part is memory-mapped, so hand back a pointer into the XIP
	 * window and let the emulator decode in place. The window is cacheable,
	 * so drop any lines that predate a write we may have just done. */
	const uint8_t *p = (const uint8_t *) (FLASH_XIP_BASE + ROM_OFFSET);

	sys_cache_data_invd_range((void *) p, ROM_SIZE);
	return p;
}

/* Write state. The block buffer exists because the NOR rejects partial
 * writes, and callers should not have to care. */
static uint8_t wr_blk[ROM_WRITE_BLK];
static uint8_t wr_first[ROM_WRITE_BLK];
static size_t wr_fill;      /* bytes buffered in wr_blk */
static size_t wr_off;       /* bytes committed so far */
static size_t wr_total;
static bool wr_have_first;
static bool wr_active;

int plat_rom_write_begin(size_t len)
{
	if (!device_is_ready(rom_flash)) {
		return -ENODEV;
	}
	if (len == 0 || len > ROM_SIZE) {
		return -EINVAL;
	}

	size_t need = ROUND_UP(len, ROM_ERASE_SZ);

	if (flash_erase(rom_flash, ROM_OFFSET, need) != 0) {
		return -EIO;
	}

	wr_fill = 0;
	wr_off = 0;
	wr_total = len;
	wr_have_first = false;
	wr_active = true;
	return 0;
}

/* Commit one full block. The very first block holds the container magic and
 * is held back until the end, so a transfer that dies halfway never leaves
 * something that parses as a valid image. */
static int commit_block(void)
{
	int rc = 0;

	if (wr_off == 0) {
		memcpy(wr_first, wr_blk, ROM_WRITE_BLK);
		wr_have_first = true;
	} else {
		rc = flash_write(rom_flash, ROM_OFFSET + wr_off, wr_blk,
				 ROM_WRITE_BLK);
	}

	wr_off += ROM_WRITE_BLK;
	wr_fill = 0;
	memset(wr_blk, 0xFF, sizeof(wr_blk));
	return rc ? -EIO : 0;
}

int plat_rom_write(const uint8_t *data, size_t len)
{
	if (!wr_active) {
		return -EINVAL;
	}

	while (len) {
		size_t n = MIN(len, ROM_WRITE_BLK - wr_fill);

		memcpy(wr_blk + wr_fill, data, n);
		wr_fill += n;
		data += n;
		len -= n;

		if (wr_fill == ROM_WRITE_BLK) {
			int rc = commit_block();

			if (rc) {
				wr_active = false;
				return rc;
			}
		}
	}
	return 0;
}

int plat_rom_write_end(void)
{
	int rc = 0;

	if (!wr_active) {
		return -EINVAL;
	}

	/* Flush a partial tail, padded with erased bytes. */
	if (wr_fill) {
		memset(wr_blk + wr_fill, 0xFF, ROM_WRITE_BLK - wr_fill);
		rc = commit_block();
	}

	/* Now the magic, last. */
	if (rc == 0 && wr_have_first) {
		if (flash_write(rom_flash, ROM_OFFSET, wr_first,
				ROM_WRITE_BLK) != 0) {
			rc = -EIO;
		}
	}

	sys_cache_data_invd_range((void *) (FLASH_XIP_BASE + ROM_OFFSET),
				  ROUND_UP(wr_total, ROM_ERASE_SZ));
	wr_active = false;
	return rc;
}
