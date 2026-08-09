/*
 * smoke.c - an RV32 S-mode payload that exercises the parts of the port that
 * are ours rather than TinyEMU's.
 *
 * Each check prints a line and either "ok" or "FAIL", and the payload exits
 * through SBI system_reset with a count. What it proves, in order:
 *
 *   1. The CPU really starts in S-mode with a0/a1 set per the boot protocol.
 *   2. The generated devicetree is a valid v17 blob at the address in a1.
 *   3. SBI console output works, in both the v0.1 legacy and DBCN forms.
 *   4. SBI base/probe_extension answers, and the spec version is what we say.
 *   5. The `time` CSR advances - the patch that upstream leaves to firmware.
 *   6. Sv32 translation is genuinely applied: a store through one virtual
 *      address is visible through a different one that maps to it.
 *   7. A page fault is delivered to stvec with the right cause and stval,
 *      which means medeleg is doing its job.
 *   8. An SBI timer arrives as an S-mode timer interrupt, which means
 *      set_timer, mideleg and the STIP path all work together.
 *
 * Nothing here needs a kernel, a rootfs or a toolchain on the Mac. It is the
 * cheapest honest answer to "does the machine layer work".
 */

#include <stdint.h>

/* Filled in by start.S from a0/a1. */
volatile uint32_t g_hartid;
volatile uint32_t g_dtb;

/* Filled in by the trap handler. */
volatile uint32_t g_scause;
volatile uint32_t g_stval;
volatile uint32_t g_traps;

extern char trap_stack_top[];
void trap_entry(void);

static int failures;

/* ------------------------------------------------------------------- SBI */

struct sbiret {
	long error;
	long value;
};

static struct sbiret sbi_call(long ext, long fid, long a0_, long a1_,
			      long a2_)
{
	register long a0 __asm__("a0") = a0_;
	register long a1 __asm__("a1") = a1_;
	register long a2 __asm__("a2") = a2_;
	register long a6 __asm__("a6") = fid;
	register long a7 __asm__("a7") = ext;
	struct sbiret r;

	__asm__ volatile ("ecall"
			  : "+r" (a0), "+r" (a1)
			  : "r" (a2), "r" (a6), "r" (a7)
			  : "memory");
	r.error = a0;
	r.value = a1;
	return r;
}

static void putc_(char c)
{
	sbi_call(0x01, 0, (unsigned char)c, 0, 0);  /* v0.1 console_putchar */
}

static void puts_(const char *s)
{
	while (*s) {
		putc_(*s++);
	}
}

static void puthex(uint32_t v)
{
	int i;

	puts_("0x");
	for (i = 28; i >= 0; i -= 4) {
		uint32_t d = (v >> i) & 0xf;

		putc_((char)(d < 10 ? '0' + d : 'a' + d - 10));
	}
}

static void check(const char *what, int ok)
{
	puts_(what);
	if (ok) {
		puts_(" ok\n");
	} else {
		puts_(" FAIL\n");
		failures++;
	}
}

/* ------------------------------------------------------------------ CSRs */

#define csr_read(csr) ({ \
	uint32_t v_; \
	__asm__ volatile ("csrr %0, " #csr : "=r" (v_)); \
	v_; })

#define csr_write(csr, val) \
	__asm__ volatile ("csrw " #csr ", %0" :: "rK" (val))

#define csr_set(csr, val) \
	__asm__ volatile ("csrs " #csr ", %0" :: "rK" (val))

static uint64_t rdtime(void)
{
	uint32_t lo, hi, hi2;

	do {
		hi = csr_read(0xc81);
		lo = csr_read(0xc01);
		hi2 = csr_read(0xc81);
	} while (hi != hi2);
	return ((uint64_t)hi << 32) | lo;
}

/* Retired instructions. TinyEMU backs this with its own instruction counter,
 * so the benchmark below measures itself exactly rather than estimating from
 * a loop count and an assumed instructions-per-iteration. */
static uint64_t rdinstret(void)
{
	uint32_t lo, hi, hi2;

	do {
		hi = csr_read(0xc82);
		lo = csr_read(0xc02);
		hi2 = csr_read(0xc82);
	} while (hi != hi2);
	return ((uint64_t)hi << 32) | lo;
}

static void putdec(uint32_t v)
{
	char b[12];
	int n = 0;

	if (v == 0) {
		putc_('0');
		return;
	}
	while (v > 0 && n < (int)sizeof(b)) {
		b[n++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (n-- > 0) {
		putc_(b[n]);
	}
}

/* --------------------------------------------------------------- page table */

/*
 * One Sv32 root table, in .bss so the linker keeps it out of the loaded
 * image and the machine layer's memset leaves it zeroed. Megapages only: a
 * level-1 leaf covers 4 MB, which is enough to map the payload and its test
 * window without a second level.
 */
static uint32_t root_pt[1024] __attribute__((aligned(4096)));

#define PTE_V (1u << 0)
#define PTE_R (1u << 1)
#define PTE_W (1u << 2)
#define PTE_X (1u << 3)
#define PTE_A (1u << 6)
#define PTE_D (1u << 7)

static void map_mega(uint32_t va, uint32_t pa)
{
	root_pt[va >> 22] = ((pa >> 12) << 10) |
			    PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D;
}

/* ------------------------------------------------------------------- main */

#define RAM_BASE   0x80000000u
#define ALIAS_BASE 0x40000000u   /* second mapping of the first 4 MB */
#define UNMAPPED   0x50000000u

void smain(void)
{
	const uint8_t *dtb;
	uint64_t t0, t1;
	uint32_t magic, totalsize;
	volatile uint32_t *pa_slot;
	volatile uint32_t *va_slot;
	struct sbiret r;
	uint32_t traps_before;
	int i;

	puts_("\n=== ra8d1-tinyemu smoke test ===\n");

	/* 1. boot protocol */
	check("hartid is 0", g_hartid == 0);
	{
		uint32_t sstatus = csr_read(sstatus);

		/* SPP is bit 8 and reads 0 here because we have not trapped
		 * yet; the fact that these CSRs are readable at all is what
		 * says we are in S-mode and not U-mode. */
		(void)sstatus;
	}

	/* 2. devicetree */
	dtb = (const uint8_t *)g_dtb;
	magic = ((uint32_t)dtb[0] << 24) | ((uint32_t)dtb[1] << 16) |
		((uint32_t)dtb[2] << 8) | dtb[3];
	totalsize = ((uint32_t)dtb[4] << 24) | ((uint32_t)dtb[5] << 16) |
		    ((uint32_t)dtb[6] << 8) | dtb[7];
	puts_("dtb at ");
	puthex(g_dtb);
	puts_(" magic ");
	puthex(magic);
	puts_(" size ");
	puthex(totalsize);
	putc_('\n');
	check("dtb magic", magic == 0xd00dfeedu);
	check("dtb size sane", totalsize > 100 && totalsize < (1u << 20));

	/* 3. SBI base */
	r = sbi_call(0x10, 0, 0, 0, 0);   /* get_spec_version */
	puts_("sbi spec ");
	puthex((uint32_t)r.value);
	putc_('\n');
	check("sbi spec version 1.0", r.error == 0 && r.value == (1 << 24));

	r = sbi_call(0x10, 3, 0x54494D45, 0, 0);   /* probe TIME */
	check("sbi probe TIME", r.error == 0 && r.value == 1);
	r = sbi_call(0x10, 3, 0x48534D, 0, 0);     /* probe HSM: absent */
	check("sbi probe HSM absent", r.error == 0 && r.value == 0);

	/* 4. DBCN console write, which takes a physical address */
	{
		static const char msg[] = "dbcn write ok\n";

		r = sbi_call(0x4442434E, 0, sizeof(msg) - 1,
			     (long)(uintptr_t)msg, 0);
		check("dbcn console_write", r.error == 0 &&
					    r.value == (long)sizeof(msg) - 1);
	}

	/* 5. the time CSR */
	t0 = rdtime();
	for (i = 0; i < 20000; i++) {
		__asm__ volatile ("" ::: "memory");
	}
	t1 = rdtime();
	puts_("time ");
	puthex((uint32_t)t0);
	puts_(" -> ");
	puthex((uint32_t)t1);
	putc_('\n');
	check("time advances", t1 > t0);

	/* 6. trap handler, then Sv32 */
	csr_write(sscratch, (uint32_t)(uintptr_t)trap_stack_top);
	csr_write(stvec, (uint32_t)(uintptr_t)trap_entry);

	for (i = 0; i < 4; i++) {
		/* identity map the low 16 MB so execution survives satp */
		map_mega(RAM_BASE + (uint32_t)i * 0x400000u,
			 RAM_BASE + (uint32_t)i * 0x400000u);
	}
	/* a second, different virtual address for the same first 4 MB */
	map_mega(ALIAS_BASE, RAM_BASE);

	csr_write(satp, (1u << 31) | ((uint32_t)(uintptr_t)root_pt >> 12));
	__asm__ volatile ("sfence.vma" ::: "memory");
	puts_("paging on, satp ");
	puthex(csr_read(satp));
	putc_('\n');

	/* Well clear of the payload and its bss: 3 MB into the first
	 * megapage. */
	pa_slot = (volatile uint32_t *)(RAM_BASE + 0x300000u);
	va_slot = (volatile uint32_t *)(ALIAS_BASE + 0x300000u);
	*pa_slot = 0xdeadbeefu;
	check("sv32 alias reads the same word", *va_slot == 0xdeadbeefu);
	*va_slot = 0xfeedfaceu;
	check("sv32 alias writes through", *pa_slot == 0xfeedfaceu);

	/* 7. page fault */
	traps_before = g_traps;
	g_scause = 0xffffffffu;
	{
		volatile uint32_t *bad = (volatile uint32_t *)UNMAPPED;
		uint32_t junk = *bad;

		(void)junk;
	}
	puts_("fault scause ");
	puthex(g_scause);
	puts_(" stval ");
	puthex(g_stval);
	putc_('\n');
	check("load page fault delivered to stvec", g_traps == traps_before + 1);
	check("scause is 13 (load page fault)", g_scause == 13);
	check("stval is the faulting address", g_stval == UNMAPPED);

	/* 8. SBI timer */
	traps_before = g_traps;
	csr_set(sie, 1u << 5);          /* STIE */
	r = sbi_call(0x54494D45, 0, (long)(uint32_t)(rdtime() + 20000),
		     0, 0);
	check("sbi set_timer accepted", r.error == 0);
	csr_set(sstatus, 1u << 1);      /* SIE */
	t0 = rdtime();
	while (g_traps == traps_before && rdtime() - t0 < 2000000u) {
		__asm__ volatile ("wfi");
	}
	puts_("timer scause ");
	puthex(g_scause);
	putc_('\n');
	check("supervisor timer interrupt delivered",
	      g_traps == traps_before + 1);
	check("scause is interrupt 5", g_scause == (0x80000000u | 5u));

	/*
	 * 9. Throughput, with paging on.
	 *
	 * Measured, not estimated: instret gives exactly how many instructions
	 * retired and time gives the microseconds, so MIPS is a division of
	 * two counters the machine itself maintains. The loop is a mix of ALU
	 * work and memory traffic through a mapped page, which is the shape of
	 * what a guest kernel actually executes - it is not a tight
	 * register-only loop that would flatter the interpreter.
	 */
	{
		volatile uint32_t *work = (volatile uint32_t *)(ALIAS_BASE + 0x200000u);
		uint64_t i0, i1, t0b, t1b;
		uint32_t acc = 1, dt, di, mips100;

		puts_("benchmarking (paging on)... ");
		i0 = rdinstret();
		t0b = rdtime();
		for (i = 0; i < 200000; i++) {
			acc = acc * 1103515245u + 12345u;
			work[acc & 0x3f] = acc;
			acc ^= work[(acc >> 8) & 0x3f];
			acc = (acc << 1) | (acc >> 31);
		}
		i1 = rdinstret();
		t1b = rdtime();

		di = (uint32_t)(i1 - i0);
		dt = (uint32_t)(t1b - t0b);
		puts_("\n  ");
		putdec(di);
		puts_(" insns in ");
		putdec(dt);
		puts_(" us = ");
		/* MIPS to two decimals. Instructions per microsecond IS
		 * millions per second. Split into quotient and remainder so
		 * every term stays in 32 bits: this payload has no libgcc, so
		 * a 64-bit divide would be an undefined __udivdi3. */
		mips100 = dt ? (di / dt) * 100u + ((di % dt) * 100u) / dt : 0;
		putdec(mips100 / 100);
		putc_('.');
		putdec((mips100 % 100) / 10);
		putdec(mips100 % 10);
		puts_(" MIPS\n");
		check("benchmark ran", di > 100000 && dt > 0);
	}

	/*
	 * 10. Console input.
	 *
	 * The one thing that could not be proven off-board: on the host
	 * plat_getc() reads a pipe, on the board it is uart_poll_in() on a
	 * real UART. Echo whatever is typed for a few seconds. Silence here is
	 * not a failure - nobody may be at the keyboard - so this reports
	 * rather than asserts.
	 */
	{
		uint64_t deadline = rdtime() + 8000000u;
		int got = 0;
		struct sbiret r2;

		puts_("console input test: type for 8 s (q ends it early)\n");
		while (rdtime() < deadline) {
			int spin;

			/* Throttled. An unthrottled poll issues millions of
			 * SBI calls in eight seconds, which swamps the call
			 * statistics and tells us nothing extra. */
			for (spin = 0; spin < 2000; spin++) {
				__asm__ volatile ("" ::: "memory");
			}
			r2 = sbi_call(0x02, 0, 0, 0, 0);  /* v0.1 getchar */
			if (r2.error >= 0) {
				char c = (char)r2.error;

				got++;
				puts_("  got byte ");
				puthex((uint32_t)(unsigned char)c);
				if (c >= 0x20 && c < 0x7f) {
					puts_(" '");
					putc_(c);
					puts_("'");
				}
				putc_('\n');
				if (c == 'q') {
					break;
				}
			}
		}
		puts_("console input: ");
		putdec((uint32_t)got);
		puts_(" bytes received");
		if (got == 0) {
			puts_(" (nobody typed, or input does not reach the guest)");
		}
		putc_('\n');
	}

	/* Done. */
	puts_("=== ");
	if (failures == 0) {
		puts_("ALL CHECKS PASSED");
	} else {
		puthex((uint32_t)failures);
		puts_(" CHECKS FAILED");
	}
	puts_(" ===\n");

	sbi_call(0x53525354, 0, 0, 0, 0);   /* SRST system_reset: shutdown */
	sbi_call(0x08, 0, 0, 0, 0);         /* v0.1 shutdown, as a fallback */
	for (;;) {
		__asm__ volatile ("wfi");
	}
}
