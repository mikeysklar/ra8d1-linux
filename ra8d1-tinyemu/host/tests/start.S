/*
 * start.S - entry and trap handler for the RV32 smoke test payload.
 *
 * Entered in S-mode at 0x80000000 with a0 = hartid and a1 = the devicetree
 * address, which is exactly the state emu/rv_hostsbi.c's riscv_cpu_boot_smode
 * leaves the CPU in and exactly what a Linux Image expects.
 */

	.section .text.init, "ax"
	.globl _start
_start:
	la	sp, stack_top
	la	t0, g_hartid
	sw	a0, 0(t0)
	la	t0, g_dtb
	sw	a1, 0(t0)
	call	smain
	/* smain does not return; if it somehow does, stop rather than run
	 * off into whatever follows. */
1:	j	1b

/*
 * Trap handler.
 *
 * Records scause/stval, counts the trap, and returns. For an exception it
 * steps sepc past the faulting instruction; every fault this payload provokes
 * is a 4-byte lw, so the fixed +4 is correct here and would not be in general.
 * For an interrupt it masks the supervisor timer instead, which is what stops
 * the same tick being delivered again the moment sret re-enables interrupts.
 *
 * sscratch holds the trap stack; smain sets it before installing stvec.
 */
	.section .text, "ax"
	.align 4
	.globl trap_entry
trap_entry:
	csrrw	sp, sscratch, sp
	addi	sp, sp, -16
	sw	t0, 0(sp)
	sw	t1, 4(sp)
	sw	t2, 8(sp)

	csrr	t0, scause
	la	t1, g_scause
	sw	t0, 0(t1)
	csrr	t2, stval
	la	t1, g_stval
	sw	t2, 0(t1)
	la	t1, g_traps
	lw	t2, 0(t1)
	addi	t2, t2, 1
	sw	t2, 0(t1)

	bltz	t0, 1f			/* scause bit 31 set: interrupt */

	csrr	t1, sepc
	addi	t1, t1, 4
	csrw	sepc, t1
	j	2f

1:	li	t1, (1 << 5)		/* SIE.STIE */
	csrc	sie, t1

2:	lw	t0, 0(sp)
	lw	t1, 4(sp)
	lw	t2, 8(sp)
	addi	sp, sp, 16
	csrrw	sp, sscratch, sp
	sret

	.section .bss, "aw", @nobits
	.align 12
	.globl trap_stack
trap_stack:
	.skip 4096
	.globl trap_stack_top
trap_stack_top:

	.globl stack
stack:
	.skip 8192
	.globl stack_top
stack_top:
