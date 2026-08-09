/*
 * Minimal standalone Zephyr application for the Renesas EK-RA8D1.
 *
 * Host scaffolding for a RISC-V emulator. Proves three things:
 *   1. our own C code runs on the board and can talk to the console,
 *   2. all 64 MB of external SDRAM is addressable and retains data,
 *   3. the CPU is running at 480 MHz and we have a usable monotonic timer.
 *
 * The SDRAM numbers are the point of the exercise: emulated Linux is
 * memory-bandwidth-bound, so read/write/copy throughput here predicts how
 * fast the emulator can possibly go.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>
#include <zephyr/cache.h>

#include <string.h>
#include <stdint.h>

#include <soc.h> /* Renesas FSP bsp_api.h: R_FSP_SystemClockHzGet() */

/* ------------------------------------------------------------------ */
/* SDRAM geometry straight out of devicetree                          */
/* ------------------------------------------------------------------ */

#define SDRAM_NODE DT_NODELABEL(sdram1)
BUILD_ASSERT(DT_NODE_HAS_STATUS(SDRAM_NODE, okay), "sdram1 node is not enabled");

#define SDRAM_ADDR ((uintptr_t)DT_REG_ADDR(SDRAM_NODE))
#define SDRAM_SIZE ((size_t)DT_REG_SIZE(SDRAM_NODE))

#define CHUNK_SIZE (1024U * 1024U) /* report progress and time per 1 MiB */
#define MAX_ERRORS_REPORTED 8

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

/*
 * Bijective pattern: every word in the region gets a distinct value derived
 * from its index, so a stuck address line shows up as a data mismatch rather
 * than silently aliasing onto itself.
 */
static inline uint32_t pattern_for(uint32_t idx, uint32_t salt)
{
	return (idx * 0x9E3779B1u) ^ (idx >> 7) ^ salt;
}

/* Throughput in KB/s (integer math, no FP in the console path). */
static uint32_t kbps(size_t bytes, int64_t ms)
{
	if (ms <= 0) {
		return 0;
	}
	return (uint32_t)(((uint64_t)bytes * 1000U) / ((uint64_t)ms * 1024U));
}

static void print_rate(const char *label, size_t bytes, int64_t ms)
{
	uint32_t kb = kbps(bytes, ms);

	printk("  %-22s %6u KiB in %5lld ms  ->  %4u.%02u MiB/s\n", label,
	       (unsigned int)(bytes / 1024U), (long long)ms, kb / 1024U,
	       ((kb % 1024U) * 100U) / 1024U);
}

static void cache_sync(void)
{
#if defined(CONFIG_CACHE_MANAGEMENT) && defined(CONFIG_DCACHE)
	sys_cache_data_flush_and_invd_all();
#endif
}

/* ------------------------------------------------------------------ */
/* SDRAM tests                                                        */
/* ------------------------------------------------------------------ */

static unsigned int g_errors;

static void report_error(volatile uint32_t *addr, uint32_t expect, uint32_t got)
{
	if (g_errors < MAX_ERRORS_REPORTED) {
		printk("\n  MISMATCH at %p: expected %08x, read %08x (xor %08x)\n",
		       (void *)addr, expect, got, expect ^ got);
	} else if (g_errors == MAX_ERRORS_REPORTED) {
		printk("\n  ... further mismatches suppressed\n");
	}
	g_errors++;
}

/*
 * Full-region write pass, then full-region verify pass, both timed.
 * The cache is flushed and invalidated between the passes so the verify
 * really goes out to the SDRAM parts instead of hitting dirty cache lines.
 */
static void sdram_write_verify(uint32_t salt)
{
	volatile uint32_t *base = (volatile uint32_t *)SDRAM_ADDR;
	const size_t words = SDRAM_SIZE / sizeof(uint32_t);
	const size_t chunk_words = CHUNK_SIZE / sizeof(uint32_t);
	int64_t t0, t_write, t_verify;

	printk("  writing  %u MiB", (unsigned int)(SDRAM_SIZE / (1024U * 1024U)));
	t0 = k_uptime_get();
	for (size_t i = 0; i < words; i++) {
		base[i] = pattern_for((uint32_t)i, salt);
		if ((i & (chunk_words - 1)) == (chunk_words - 1)) {
			printk(".");
		}
	}
	cache_sync();
	t_write = k_uptime_get() - t0;
	printk("\n");

	printk("  verifying %u MiB", (unsigned int)(SDRAM_SIZE / (1024U * 1024U)));
	t0 = k_uptime_get();
	for (size_t i = 0; i < words; i++) {
		uint32_t got = base[i];
		uint32_t expect = pattern_for((uint32_t)i, salt);

		if (got != expect) {
			report_error(&base[i], expect, got);
		}
		if ((i & (chunk_words - 1)) == (chunk_words - 1)) {
			printk(".");
		}
	}
	t_verify = k_uptime_get() - t0;
	printk("\n");

	print_rate("volatile u32 write", SDRAM_SIZE, t_write);
	print_rate("volatile u32 verify", SDRAM_SIZE, t_verify);
}

/*
 * Bandwidth pass without the volatile qualifier, so the compiler is free to
 * emit the wide load/store sequences a real workload would get. This is the
 * number that matters for the emulator.
 */
static void sdram_bandwidth(void)
{
	uint32_t *base = (uint32_t *)SDRAM_ADDR;
	const size_t words = SDRAM_SIZE / sizeof(uint32_t);
	int64_t t0, dt;
	uint32_t acc = 0;

	/* sequential write */
	cache_sync();
	t0 = k_uptime_get();
	for (size_t i = 0; i < words; i++) {
		base[i] = (uint32_t)i;
	}
	cache_sync();
	dt = k_uptime_get() - t0;
	print_rate("seq u32 store", SDRAM_SIZE, dt);

	/* sequential read */
	t0 = k_uptime_get();
	for (size_t i = 0; i < words; i++) {
		acc = (acc << 1) + base[i]; /* order-sensitive, will not cancel out */
	}
	dt = k_uptime_get() - t0;
	print_rate("seq u32 load", SDRAM_SIZE, dt);

	/* memset over the whole region */
	cache_sync();
	t0 = k_uptime_get();
	memset(base, 0x5A, SDRAM_SIZE);
	cache_sync();
	dt = k_uptime_get() - t0;
	print_rate("memset", SDRAM_SIZE, dt);

	/* memcpy, lower half -> upper half (32 MiB moved, 64 MiB of traffic) */
	cache_sync();
	t0 = k_uptime_get();
	memcpy((uint8_t *)base + SDRAM_SIZE / 2, base, SDRAM_SIZE / 2);
	cache_sync();
	dt = k_uptime_get() - t0;
	print_rate("memcpy 32 MiB", SDRAM_SIZE / 2, dt);

	/*
	 * Cache-hostile pass: one word touched per 32-byte cache line, which is
	 * roughly what an emulator's guest-memory access pattern looks like when
	 * it is chasing pointers rather than streaming.
	 */
	{
		const size_t stride = 32 / sizeof(uint32_t); /* one word per cache line */
		const size_t lines = words / stride;

		cache_sync();
		t0 = k_uptime_get();
		for (size_t i = 0; i < words; i += stride) {
			acc = (acc << 1) + base[i]; /* order-sensitive, will not cancel out */
		}
		dt = k_uptime_get() - t0;
		printk("  %-22s %6u lines in %5lld ms  ->  %u Klines/s\n", "1 word / 32B line",
		       (unsigned int)lines, (long long)dt,
		       dt ? (unsigned int)((uint64_t)lines / (uint64_t)dt) : 0U);
	}

	printk("  (checksum %08x, printed so the loops are not optimised away)\n", acc);
}

/*
 * Dump how the MPU actually types the SDRAM window.
 *
 * This decides whether unaligned accesses are legal at all: the Cortex-M85
 * permits them only against Normal memory. Against Device memory an unaligned
 * LDR/STR is an UNALIGNED UsageFault regardless of CCR.UNALIGN_TRP. The guest
 * emulator issues unaligned 32-bit accesses, so this has to be Normal.
 */
static void report_memory_attrs(void)
{
	uint32_t ctrl = MPU->CTRL;
	uint32_t nregions = (MPU->TYPE >> 8) & 0xFFU;
	uint32_t saved_rnr = MPU->RNR;
	bool covered = false;

	printk("Memory typing for 0x%08lx\n", (unsigned long)SDRAM_ADDR);
	printk("  MPU_CTRL   0x%08x (ENABLE=%u PRIVDEFENA=%u), %u regions\n", (unsigned int)ctrl,
	       (unsigned int)(ctrl & 1U), (unsigned int)((ctrl >> 2) & 1U), (unsigned int)nregions);
	printk("  SCB_CCR    0x%08x (UNALIGN_TRP=%u)\n", (unsigned int)SCB->CCR,
	       (unsigned int)((SCB->CCR & SCB_CCR_UNALIGN_TRP_Msk) >> SCB_CCR_UNALIGN_TRP_Pos));

	for (uint32_t i = 0; i < nregions; i++) {
		MPU->RNR = i;
		uint32_t rbar = MPU->RBAR;
		uint32_t rlar = MPU->RLAR;

		if ((rlar & MPU_RLAR_EN_Msk) == 0U) {
			continue;
		}

		uint32_t base = rbar & MPU_RBAR_BASE_Msk;
		uint32_t limit = (rlar & MPU_RLAR_LIMIT_Msk) | 0x1FU;

		if (SDRAM_ADDR < base || SDRAM_ADDR > limit) {
			continue;
		}

		uint32_t idx = (rlar & MPU_RLAR_AttrIndx_Msk) >> MPU_RLAR_AttrIndx_Pos;
		uint32_t mair = (idx < 4U) ? MPU->MAIR[0] : MPU->MAIR[1];
		uint32_t attr = (mair >> ((idx & 3U) * 8U)) & 0xFFU;

		covered = true;
		printk("  region %u covers it: 0x%08x-0x%08x AttrIndx=%u MAIR=0x%02x -> %s\n",
		       (unsigned int)i, (unsigned int)base, (unsigned int)limit, (unsigned int)idx,
		       (unsigned int)attr,
		       ((attr & 0xF0U) == 0U) ? "DEVICE (unaligned will fault!)" : "NORMAL");
	}
	MPU->RNR = saved_rnr;

	if (!covered) {
		printk("  no MPU region covers it; falls through to the default background\n"
		       "  map, where 0x60000000-0x7fffffff is Normal WBWA. Unaligned OK.\n");
	}
}

/*
 * Unaligned 32-bit access, which is what the guest emulator will do.
 *
 * The packed struct is the idiom that makes GCC emit a single unaligned
 * LDR/STR rather than a byte-assembly sequence, so this really does exercise
 * the hardware path. Verified in the disassembly, see the notes.
 */
struct unaligned_u32 {
	uint32_t v;
} __packed;

static inline uint32_t rd32u(const void *p)
{
	return ((const struct unaligned_u32 *)p)->v;
}

static inline void wr32u(void *p, uint32_t v)
{
	((struct unaligned_u32 *)p)->v = v;
}

static void sdram_unaligned_access(void)
{
	uint8_t *base = (uint8_t *)SDRAM_ADDR;
	unsigned int fails = 0;
	int64_t t0, dt;
	uint32_t acc = 0;

	/* Every byte offset, including the ones that straddle a 32-byte cache
	 * line boundary and a 4 KB SDRAM page boundary.
	 */
	for (unsigned int off = 1; off < 4; off++) {
		for (unsigned int i = 0; i < 4096; i++) {
			uint8_t *p = base + 8192 + off + i * 4;

			wr32u(p, pattern_for(i, 0xC0FFEE00u + off));
		}
		cache_sync();
		for (unsigned int i = 0; i < 4096; i++) {
			uint8_t *p = base + 8192 + off + i * 4;

			if (rd32u(p) != pattern_for(i, 0xC0FFEE00u + off)) {
				fails++;
			}
		}
		printk("  offset +%u: %s\n", off, fails ? "FAIL" : "ok");
	}

	/* Straddle the 32-byte cache line boundary specifically. */
	for (unsigned int i = 0; i < 1024; i++) {
		uint8_t *p = base + 65536 + i * 32 + 30; /* spans two lines */

		wr32u(p, 0xA5A50000u + i);
	}
	cache_sync();
	for (unsigned int i = 0; i < 1024; i++) {
		uint8_t *p = base + 65536 + i * 32 + 30;

		if (rd32u(p) != 0xA5A50000u + i) {
			fails++;
		}
	}
	printk("  cache-line-straddling (+30 of every 32): %s\n", fails ? "FAIL" : "ok");

	/* Throughput of unaligned reads, since that is the emulator's hot path. */
	{
		const size_t n = 4U * 1024U * 1024U; /* 4 M unaligned reads over 16 MiB */

		cache_sync();
		t0 = k_uptime_get();
		for (size_t i = 0; i < n; i++) {
			acc = (acc << 1) + rd32u(base + 1 + i * 4);
		}
		dt = k_uptime_get() - t0;
		printk("  unaligned u32 load: %u reads in %lld ms -> %u.%02u MiB/s (sum %08x)\n",
		       (unsigned int)n, (long long)dt, kbps(n * 4U, dt) / 1024U,
		       ((kbps(n * 4U, dt) % 1024U) * 100U) / 1024U, acc);
	}

	printk("  unaligned access: %s (%u failures)\n", fails ? "FAIL" : "ok", fails);
	g_errors += fails;
}

/* Byte and halfword access sanity: the SDRAM bus is 16 bit wide, so partial
 * writes go through the byte-enable path and are worth proving separately.
 */
static void sdram_narrow_access(void)
{
	volatile uint8_t *b = (volatile uint8_t *)SDRAM_ADDR;
	volatile uint16_t *h = (volatile uint16_t *)(SDRAM_ADDR + 4096);
	unsigned int fails = 0;

	for (unsigned int i = 0; i < 256; i++) {
		b[i] = (uint8_t)(i ^ 0xA5);
	}
	cache_sync();
	for (unsigned int i = 0; i < 256; i++) {
		if (b[i] != (uint8_t)(i ^ 0xA5)) {
			fails++;
		}
	}

	for (unsigned int i = 0; i < 256; i++) {
		h[i] = (uint16_t)(i * 0x0101u + 0x1234u);
	}
	cache_sync();
	for (unsigned int i = 0; i < 256; i++) {
		if (h[i] != (uint16_t)(i * 0x0101u + 0x1234u)) {
			fails++;
		}
	}

	printk("  byte/halfword access: %s (%u failures)\n", fails ? "FAIL" : "ok", fails);
	g_errors += fails;
}

/* Data must survive with only auto-refresh keeping it alive. */
static void sdram_retention(void)
{
	volatile uint32_t *base = (volatile uint32_t *)SDRAM_ADDR;
	const size_t words = SDRAM_SIZE / sizeof(uint32_t);
	const size_t step = words / 4096; /* 4096 probes spread over the region */
	unsigned int fails = 0;

	for (size_t i = 0; i < words; i += step) {
		base[i] = pattern_for((uint32_t)i, 0xDEADBEEFu);
	}
	cache_sync();

	printk("  holding 2 s with refresh only...\n");
	k_sleep(K_MSEC(2000));
	cache_sync();

	for (size_t i = 0; i < words; i += step) {
		if (base[i] != pattern_for((uint32_t)i, 0xDEADBEEFu)) {
			fails++;
		}
	}
	printk("  retention after 2 s: %s (%u/%u probes bad)\n", fails ? "FAIL" : "ok", fails,
	       (unsigned int)(words / step));
	g_errors += fails;
}

/* ------------------------------------------------------------------ */
/* clock / timer                                                      */
/* ------------------------------------------------------------------ */

static void report_clocks(void)
{
	/* CPUCLK is the core clock. ICLK is the system bus and BCLK feeds the
	 * external bus (and therefore the SDRAM controller), so both are worth
	 * printing next to the bandwidth numbers. All three come from the FSP
	 * reading the live SCKDIVCR/SCKDIVCR2 dividers, not from Kconfig.
	 */
	uint32_t cpuclk = R_FSP_SystemClockHzGet(FSP_PRIV_CLOCK_CPUCLK);
	uint32_t iclk = R_FSP_SystemClockHzGet(FSP_PRIV_CLOCK_ICLK);
	uint32_t bclk = R_FSP_SystemClockHzGet(FSP_PRIV_CLOCK_BCLK);

	printk("Clocks\n");
	printk("  CPUCLK (core)                       %u Hz\n", (unsigned int)cpuclk);
	printk("  ICLK   (system bus)                 %u Hz\n", (unsigned int)iclk);
	printk("  BCLK   (external bus / SDRAM)       %u Hz\n", (unsigned int)bclk);
	printk("  SystemCoreClock (CMSIS)             %u Hz\n", (unsigned int)SystemCoreClock);
	printk("  CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC  %u Hz\n",
	       (unsigned int)sys_clock_hw_cycles_per_sec());

	printk("  -> CPU is %u.%03u MHz: %s\n", cpuclk / 1000000U, (cpuclk % 1000000U) / 1000U,
	       (cpuclk == 480000000U) ? "480 MHz CONFIRMED" : "NOT 480 MHz");

	/* Independent sanity check that the kernel cycle counter and the ms
	 * uptime clock agree, i.e. the timebase we will use is trustworthy.
	 */
	{
		uint32_t c0 = k_cycle_get_32();
		int64_t m0 = k_uptime_get();

		k_busy_wait(500000); /* 500 ms */

		uint32_t dc = k_cycle_get_32() - c0;
		int64_t dm = k_uptime_get() - m0;

		printk("  busy_wait(500 ms): %lld ms elapsed, %u kernel cycles"
		       " (implies %u Hz)\n",
		       (long long)dm, (unsigned int)dc,
		       dm ? (unsigned int)(((uint64_t)dc * 1000U) / (uint64_t)dm) : 0U);
	}
}

/* ------------------------------------------------------------------ */

int main(void)
{
	printk("\n");
	printk("=====================================================\n");
	printk(" EK-RA8D1 bare Zephyr skeleton\n");
	printk(" board=%s  soc=%s\n", CONFIG_BOARD, CONFIG_SOC);
	printk(" built %s %s\n", __DATE__, __TIME__);
	printk("=====================================================\n\n");

	report_clocks();

	printk("\nMonotonic timer\n");
	printk("  k_uptime_get()   %lld ms\n", (long long)k_uptime_get());
	printk("  k_cycle_get_32() %u\n", (unsigned int)k_cycle_get_32());
	printk("  ticks/sec        %u\n", (unsigned int)CONFIG_SYS_CLOCK_TICKS_PER_SEC);

	printk("\nSDRAM (devicetree node " DT_NODE_PATH(SDRAM_NODE) ")\n");
	printk("  base 0x%08lx  size %u MiB (%u bytes)\n", (unsigned long)SDRAM_ADDR,
	       (unsigned int)(SDRAM_SIZE / (1024U * 1024U)), (unsigned int)SDRAM_SIZE);
#if defined(CONFIG_DCACHE)
	printk("  D-cache enabled\n");
#else
	printk("  D-cache DISABLED\n");
#endif

	printk("\n");
	report_memory_attrs();

	printk("\n[1] narrow access\n");
	sdram_narrow_access();

	printk("\n[1b] unaligned 32-bit access (the emulator's access pattern)\n");
	sdram_unaligned_access();

	printk("\n[2] full-region write + verify (pass 1)\n");
	sdram_write_verify(0x00000000u);

	printk("\n[3] full-region write + verify (pass 2, inverted salt)\n");
	sdram_write_verify(0xFFFFFFFFu);

	printk("\n[4] retention\n");
	sdram_retention();

	printk("\n[5] bandwidth\n");
	sdram_bandwidth();

	printk("\n=====================================================\n");
	if (g_errors == 0) {
		printk(" RESULT: all %u MiB of SDRAM verified, 0 errors\n",
		       (unsigned int)(SDRAM_SIZE / (1024U * 1024U)));
	} else {
		printk(" RESULT: FAILED, %u errors\n", g_errors);
	}
	printk("=====================================================\n");

	while (1) {
		k_sleep(K_SECONDS(10));
		printk("alive, uptime %lld ms\n", (long long)k_uptime_get());
	}
	return 0;
}
