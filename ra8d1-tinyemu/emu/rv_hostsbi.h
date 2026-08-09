/*
 * rv_hostsbi.h - the M-mode firmware, written in host C.
 *
 * The guest kernel boots straight into S-mode with nothing underneath it.
 * Everything an OpenSBI or BBL image would normally provide - the SBI call
 * interface, the `time` CSR, and the initial machine state - is provided
 * here instead, by the host, at native speed and at a cost of zero emulated
 * instructions. See notes/00-port.md for why.
 *
 * This is the only file besides riscv_cpu.c that includes riscv_cpu_priv.h.
 * Keeping that containment is deliberate: the CPU's internals are visible in
 * exactly two places, and the machine layer is not one of them.
 */
#ifndef RV_HOSTSBI_H_
#define RV_HOSTSBI_H_

#include <stdint.h>

#include "riscv_cpu.h"

/*
 * Service a supervisor ecall. Called from riscv_cpu.c's raise_exception2()
 * under CONFIG_HOST_SBI, before the trap is delivered.
 *
 * Always returns TRUE - an unrecognised extension is answered with
 * SBI_ERR_NOT_SUPPORTED rather than declined. Declining would deliver a
 * cause-9 trap to a kernel that has no handler for one, which is a worse
 * failure than an honest error code.
 *
 * On entry s->pc is the ecall instruction; the caller advances it.
 */
BOOL riscv_host_sbi(RISCVCPUState *s);

/*
 * The value of the `time` / `timeh` CSRs (0xc01 / 0xc81), which upstream
 * TinyEMU leaves unimplemented for M-mode firmware to emulate - see the
 * "the 'time' counter is usually emulated" comment at riscv_cpu.c:846.
 * Reads the machine's mtime, which is plat_now_us().
 */
uint64_t riscv_host_rdtime(void);

/*
 * Put a freshly initialised CPU into the state a bootloader hands an S-mode
 * kernel: privilege S, paging off, all traps and interrupts delegated to
 * S-mode, a0 = hartid, a1 = physical address of the devicetree blob.
 */
void riscv_cpu_boot_smode(RISCVCPUState *s, uint32_t pc, uint32_t hartid,
			  uint32_t dtb_paddr);

/* Diagnostics: how many SBI calls have been serviced, by extension. */
void riscv_host_sbi_stats(uint32_t *n_calls, uint32_t *n_unsupported);

#endif /* RV_HOSTSBI_H_ */
