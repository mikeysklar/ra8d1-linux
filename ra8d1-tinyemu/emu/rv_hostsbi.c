/*
 * rv_hostsbi.c - SBI and initial machine state, implemented on the host.
 *
 * See rv_hostsbi.h for the contract and notes/00-port.md section "SBI" for
 * why this exists instead of an OpenSBI blob.
 *
 * Spec implemented: SBI v1.0 (report 0x01000000 from BASE/get_spec_version)
 * plus the whole of the v0.1 legacy set, which is what a kernel built with
 * CONFIG_RISCV_SBI_V01 uses for early console output.
 *
 * Everything here runs on one hart. That is not a simplification that will
 * bite later: the emulator is a single interpreter loop, the devicetree
 * declares one cpu, and the IPI and RFENCE extensions are correct no-ops in
 * that configuration rather than stubs pretending to be correct.
 */

#include <stdint.h>
#include <string.h>

#include "riscv_cpu_priv.h"
#include "rv_hostsbi.h"
#include "rv_machine.h"
#include "rv_platform.h"

/* ------------------------------------------------------------- SBI numbers */

#define SBI_SUCCESS                0
#define SBI_ERR_FAILED            (-1)
#define SBI_ERR_NOT_SUPPORTED     (-2)
#define SBI_ERR_INVALID_PARAM     (-3)
#define SBI_ERR_DENIED            (-4)
#define SBI_ERR_INVALID_ADDRESS   (-5)

/* v0.1 legacy extensions: the extension id is the function. */
#define SBI_EXT_0_1_SET_TIMER              0x00
#define SBI_EXT_0_1_CONSOLE_PUTCHAR        0x01
#define SBI_EXT_0_1_CONSOLE_GETCHAR        0x02
#define SBI_EXT_0_1_CLEAR_IPI              0x03
#define SBI_EXT_0_1_SEND_IPI               0x04
#define SBI_EXT_0_1_REMOTE_FENCE_I         0x05
#define SBI_EXT_0_1_REMOTE_SFENCE_VMA      0x06
#define SBI_EXT_0_1_REMOTE_SFENCE_VMA_ASID 0x07
#define SBI_EXT_0_1_SHUTDOWN               0x08

/* v0.2+ extensions, identified by their ASCII names. */
#define SBI_EXT_BASE   0x10
#define SBI_EXT_TIME   0x54494D45  /* "TIME" */
#define SBI_EXT_IPI    0x00735049  /* "sPI" */
#define SBI_EXT_RFNC   0x52464E43  /* "RFNC" */
#define SBI_EXT_SRST   0x53525354  /* "SRST" */
#define SBI_EXT_DBCN   0x4442434E  /* "DBCN" */

#define SBI_BASE_GET_SPEC_VERSION 0
#define SBI_BASE_GET_IMPL_ID      1
#define SBI_BASE_GET_IMPL_VERSION 2
#define SBI_BASE_PROBE_EXT        3
#define SBI_BASE_GET_MVENDORID    4
#define SBI_BASE_GET_MARCHID      5
#define SBI_BASE_GET_MIMPID       6

#define SBI_SPEC_VERSION  ((1u << 24) | 0u)   /* v1.0 */

/*
 * Implementation id. The registered list in the SBI spec covers BBL, OpenSBI,
 * KVM and friends; this is none of them, and claiming to be OpenSBI would put
 * a false line in every guest's boot log. Linux only prints this value, so an
 * unregistered but recognisable one is the honest choice.
 */
#define SBI_IMPL_ID       0x52413844u  /* 'RA8D' */
#define SBI_IMPL_VERSION  1u

/* Register indices. a0..a7 are x10..x17. */
#define A0 10
#define A1 11
#define A2 12
#define A3 13
#define A6 16
#define A7 17

static uint32_t sbi_n_calls;
static uint32_t sbi_n_unsupported;

/* ---------------------------------------------------------------- helpers */

/*
 * The 32-bit halves of a 64-bit SBI argument. On RV32 the caller passes a
 * value in a register pair; on RV64 it is one register and the high half is
 * unused. MAX_XLEN is 32 in this build, so the pair form is the only one, but
 * the split is written out rather than assumed so that it stays obvious.
 */
static uint64_t sbi_arg64(RISCVCPUState *s, int lo_reg, int hi_reg)
{
#if MAX_XLEN == 32
	return (uint64_t)s->reg[lo_reg] | ((uint64_t)s->reg[hi_reg] << 32);
#else
	(void)hi_reg;
	return s->reg[lo_reg];
#endif
}

static BOOL sbi_ext_supported(uint32_t ext)
{
	switch (ext) {
	case SBI_EXT_0_1_SET_TIMER:
	case SBI_EXT_0_1_CONSOLE_PUTCHAR:
	case SBI_EXT_0_1_CONSOLE_GETCHAR:
	case SBI_EXT_0_1_CLEAR_IPI:
	case SBI_EXT_0_1_SEND_IPI:
	case SBI_EXT_0_1_REMOTE_FENCE_I:
	case SBI_EXT_0_1_REMOTE_SFENCE_VMA:
	case SBI_EXT_0_1_REMOTE_SFENCE_VMA_ASID:
	case SBI_EXT_0_1_SHUTDOWN:
	case SBI_EXT_BASE:
	case SBI_EXT_TIME:
	case SBI_EXT_IPI:
	case SBI_EXT_RFNC:
	case SBI_EXT_SRST:
	case SBI_EXT_DBCN:
		return TRUE;
	default:
		/*
		 * Notably absent: HSM. Advertising it would oblige us to start
		 * secondary harts, and there is exactly one. A kernel that
		 * probes for it gets 0 and uses the spin-table path, which for
		 * a single cpu node does nothing at all.
		 */
		return FALSE;
	}
}

/* -------------------------------------------------------- the v0.2+ groups */

static long sbi_base(RISCVCPUState *s, uint32_t fid, target_ulong *val)
{
	switch (fid) {
	case SBI_BASE_GET_SPEC_VERSION:
		*val = SBI_SPEC_VERSION;
		return SBI_SUCCESS;
	case SBI_BASE_GET_IMPL_ID:
		*val = SBI_IMPL_ID;
		return SBI_SUCCESS;
	case SBI_BASE_GET_IMPL_VERSION:
		*val = SBI_IMPL_VERSION;
		return SBI_SUCCESS;
	case SBI_BASE_PROBE_EXT:
		*val = sbi_ext_supported((uint32_t)s->reg[A0]) ? 1 : 0;
		return SBI_SUCCESS;
	case SBI_BASE_GET_MVENDORID:
	case SBI_BASE_GET_MARCHID:
	case SBI_BASE_GET_MIMPID:
		/* All three are legitimately zero: "not implemented". */
		*val = 0;
		return SBI_SUCCESS;
	default:
		return SBI_ERR_NOT_SUPPORTED;
	}
}

/*
 * Debug console (DBCN). Addresses here are guest *physical*, not virtual, so
 * no page walk is involved - the whole point of the extension is that it
 * works before the kernel has a console driver or a stable mapping.
 */
static long sbi_dbcn(RISCVCPUState *s, uint32_t fid, target_ulong *val)
{
	uint64_t base;
	uint32_t len;
	uint8_t *p;
	uint32_t i;
	int c;

	switch (fid) {
	case 0: /* console_write(num_bytes, base_lo, base_hi) */
		len = (uint32_t)s->reg[A0];
		base = sbi_arg64(s, A1, A2);
		if (len == 0) {
			*val = 0;
			return SBI_SUCCESS;
		}
		p = rv_guest_phys_ptr(base, len);
		if (p == NULL) {
			return SBI_ERR_INVALID_ADDRESS;
		}
		for (i = 0; i < len; i++) {
			plat_putc((char)p[i]);
		}
		*val = len;
		return SBI_SUCCESS;

	case 1: /* console_read(num_bytes, base_lo, base_hi) */
		len = (uint32_t)s->reg[A0];
		base = sbi_arg64(s, A1, A2);
		if (len == 0) {
			*val = 0;
			return SBI_SUCCESS;
		}
		p = rv_guest_phys_ptr(base, len);
		if (p == NULL) {
			return SBI_ERR_INVALID_ADDRESS;
		}
		for (i = 0; i < len; i++) {
			c = plat_getc();
			if (c < 0) {
				break;
			}
			p[i] = (uint8_t)c;
		}
		*val = i;
		return SBI_SUCCESS;

	case 2: /* console_write_byte(byte) */
		plat_putc((char)(s->reg[A0] & 0xff));
		*val = 0;
		return SBI_SUCCESS;

	default:
		return SBI_ERR_NOT_SUPPORTED;
	}
}

/* --------------------------------------------------------------- dispatch */

BOOL riscv_host_sbi(RISCVCPUState *s)
{
	uint32_t ext = (uint32_t)s->reg[A7];
	uint32_t fid = (uint32_t)s->reg[A6];
	target_ulong val = 0;
	long err = SBI_SUCCESS;
	int c;

	sbi_n_calls++;

	switch (ext) {
	/* ------------------------------------------------ v0.1 legacy set */
	case SBI_EXT_0_1_SET_TIMER:
		rv_set_timer(sbi_arg64(s, A0, A1));
		s->reg[A0] = 0;
		return TRUE;

	case SBI_EXT_0_1_CONSOLE_PUTCHAR:
		plat_putc((char)(s->reg[A0] & 0xff));
		s->reg[A0] = 0;
		return TRUE;

	case SBI_EXT_0_1_CONSOLE_GETCHAR:
		c = plat_getc();
		/* Legacy getchar returns the character, or -1 (as SBI_ERR_FAILED
		 * happens to be) when nothing is waiting. */
		s->reg[A0] = (target_ulong)(target_long)c;
		return TRUE;

	case SBI_EXT_0_1_CLEAR_IPI:
		rv_set_ipi(false);
		s->reg[A0] = 0;
		return TRUE;

	case SBI_EXT_0_1_SEND_IPI:
		/* The only hart in the mask can be this one. */
		rv_set_ipi(true);
		s->reg[A0] = 0;
		return TRUE;

	case SBI_EXT_0_1_REMOTE_FENCE_I:
	case SBI_EXT_0_1_REMOTE_SFENCE_VMA:
	case SBI_EXT_0_1_REMOTE_SFENCE_VMA_ASID:
		/* Remote, on a machine with no remote. The calling hart has
		 * already fenced itself before it got here. */
		s->reg[A0] = 0;
		return TRUE;

	case SBI_EXT_0_1_SHUTDOWN:
		rv_request_poweroff();
		s->reg[A0] = 0;
		return TRUE;

	/* ----------------------------------------------------- v0.2 and up */
	case SBI_EXT_BASE:
		err = sbi_base(s, fid, &val);
		break;

	case SBI_EXT_TIME:
		if (fid == 0) {
			rv_set_timer(sbi_arg64(s, A0, A1));
		} else {
			err = SBI_ERR_NOT_SUPPORTED;
		}
		break;

	case SBI_EXT_IPI:
		if (fid == 0) {
			rv_set_ipi(true);
		} else {
			err = SBI_ERR_NOT_SUPPORTED;
		}
		break;

	case SBI_EXT_RFNC:
		/* fid 0..6: fence.i and the sfence.vma family, all remote. */
		if (fid > 6) {
			err = SBI_ERR_NOT_SUPPORTED;
		}
		break;

	case SBI_EXT_SRST:
		if (fid != 0) {
			err = SBI_ERR_NOT_SUPPORTED;
			break;
		}
		/* a0: 0 shutdown, 1 cold reboot, 2 warm reboot. */
		if (s->reg[A0] == 0) {
			rv_request_poweroff();
		} else if (s->reg[A0] <= 2) {
			rv_request_reboot();
		} else {
			err = SBI_ERR_INVALID_PARAM;
			break;
		}
		/* A successful system_reset does not return to the guest, so
		 * there is nothing to report; the run loop will stop. */
		break;

	case SBI_EXT_DBCN:
		err = sbi_dbcn(s, fid, &val);
		break;

	default:
		sbi_n_unsupported++;
		err = SBI_ERR_NOT_SUPPORTED;
		break;
	}

	s->reg[A0] = (target_ulong)(target_long)err;
	s->reg[A1] = val;
	return TRUE;
}

uint64_t riscv_host_rdtime(void)
{
	return rv_mtime();
}

void riscv_host_sbi_stats(uint32_t *n_calls, uint32_t *n_unsupported)
{
	if (n_calls) {
		*n_calls = sbi_n_calls;
	}
	if (n_unsupported) {
		*n_unsupported = sbi_n_unsupported;
	}
}

/* ------------------------------------------------------------- boot state */

void riscv_cpu_boot_smode(RISCVCPUState *s, uint32_t pc, uint32_t hartid,
			  uint32_t dtb_paddr)
{
	/*
	 * Privilege is assigned rather than passed through set_priv(), which
	 * is static to riscv_cpu.c. That is safe here and only here: set_priv
	 * does two things, flush the TLB and recompute cur_xlen, and at this
	 * point the TLB is empty (tlb_init ran in riscv_cpu_init) while
	 * cur_xlen is fixed at 32 in a MAX_XLEN == 32 build - the recompute is
	 * inside #if MAX_XLEN >= 64. Call this only on a freshly initialised
	 * CPU.
	 */
	s->priv = PRV_S;
	s->pc = pc;

	/*
	 * Delegate every trap that can arise below M-mode to S-mode. Without
	 * this a page fault would be taken to mtvec, which is 0, and the guest
	 * would fetch from address 0 instead of running its own handler.
	 *
	 * Excluded: cause 9 (ecall from S) and 11 (ecall from M). Cause 9 is
	 * ours - riscv_host_sbi() intercepts it before delegation is even
	 * considered - and leaving it undelegated means a bug in that intercept
	 * shows up as an immediate stop rather than as a confusing trap in the
	 * kernel's own handler.
	 */
	s->medeleg = 0xffffu & ~((1u << 9) | (1u << 10) | (1u << 11));
	s->mideleg = MIP_SSIP | MIP_STIP | MIP_SEIP;

	/*
	 * Let S-mode and below read cycle/time/instret. Bit 1 (TM) is the one
	 * that matters: Linux reads `time` through rdtime in the kernel and in
	 * the vDSO, and without this every such read is an illegal instruction.
	 * Upstream's COUNTEREN_MASK omits bit 1; see the patch note in
	 * notes/00-port.md.
	 */
	s->mcounteren = 0x7;

	/* Interrupts arrive only once the kernel enables them. */
	s->mstatus &= ~(target_ulong)(MSTATUS_SIE | MSTATUS_MIE);
	s->satp = 0;

	/* The RISC-V Linux boot protocol: a0 = hartid, a1 = dtb address. */
	memset(s->reg, 0, sizeof(s->reg));
	s->reg[A0] = hartid;
	s->reg[A1] = dtb_paddr;
	s->mhartid = hartid;
}
