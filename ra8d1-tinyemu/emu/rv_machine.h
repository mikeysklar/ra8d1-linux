/*
 * rv_machine.h - the emulated board.
 *
 * TinyEMU's own riscv_machine.c is deliberately not used: it assumes a
 * hosted environment, a bios blob, VirtIO block/net/input devices and a
 * framebuffer, none of which apply here. This is a much smaller machine
 * built to the same interface the CPU expects, keeping the device layout the
 * existing mini-rv32ima port already established so the paravirt I/O bridge
 * and its guest driver carry over unchanged.
 *
 * Guest physical memory map (see notes/00-port.md for the reasoning):
 *
 *   0x10000000  8250 UART, 8 registers          console
 *   0x11000000  CLINT                           msip / mtimecmp / mtime
 *   0x11100000  syscon                          poweroff and reboot
 *   0x11200000  paravirt I/O                    real board I2C and GPIO
 *   0x40100000  PLIC                            one hart, 32 sources
 *   0x80000000  RAM                             up to 64 MB
 */
#ifndef RV_MACHINE_H_
#define RV_MACHINE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------- memory map */

#define RV_UART_BASE   0x10000000u
#define RV_UART_SIZE   0x00000100u
#define RV_CLINT_BASE  0x11000000u
#define RV_CLINT_SIZE  0x00010000u
#define RV_SYSCON_BASE 0x11100000u
#define RV_SYSCON_SIZE 0x00001000u
#define RV_PVIO_BASE   0x11200000u
#define RV_PVIO_SIZE   0x00001000u
#define RV_VIRTIO_BASE 0x10001000u
#define RV_VIRTIO_SIZE 0x00001000u
#define RV_PLIC_BASE   0x40100000u
#define RV_PLIC_SIZE   0x00400000u
#define RV_RAM_BASE    0x80000000u

/* PLIC sources. Source 0 does not exist. */
#define RV_UART_IRQ    1
#define RV_VIRTIO_IRQ  2

/* ------------------------------------------------------------------- boot */

typedef struct {
	const uint8_t *kernel;   /* flat Image, entered in S-mode at RV_RAM_BASE */
	size_t kernel_size;
	const uint8_t *initrd;   /* optional, may be NULL */
	size_t initrd_size;
	/*
	 * Root filesystem, exposed as a read-only virtio-blk device. Unlike
	 * the two above this is NOT copied into guest RAM - the pointer is
	 * handed to the block device and read in place, which is the only
	 * reason a 55 MB rootfs is possible on a board with 64 MB of SDRAM.
	 * NULL leaves the machine with no block device at all.
	 */
	const uint8_t *rootfs;
	size_t rootfs_size;
	const char *cmdline;     /* NULL for the built-in default */
} RVBootImage;

/*
 * Build the machine, copy the image into guest RAM, generate the devicetree
 * and leave the CPU ready to execute the kernel's first instruction.
 * Returns 0, or a negative errno: -ENOMEM if the image does not fit the RAM
 * the platform provided, -EINVAL for a malformed request.
 */
int rv_machine_init(const RVBootImage *img);

/*
 * Run until the guest powers off or reboots. Returns 0 for poweroff, 1 for a
 * reboot request; the caller decides what to do about either.
 *
 * The loop is cooperative: it services the console and the timer between
 * instruction slices, never blocks, and never yields on its own. On Zephyr
 * that matters - see the priority note in src/main.c.
 */
int rv_machine_run(void);

/*
 * As rv_machine_run(), but give up and return 2 after `insn_limit` retired
 * instructions or `us_limit` microseconds of wall time. Either may be 0,
 * meaning no bound in that dimension. For test payloads: a payload that
 * misbehaves usually loops rather than stopping, and a bound turns that from
 * a hang into a result.
 *
 * Both bounds exist because neither alone is enough. An instruction bound
 * cannot stop a guest sitting in WFI - it retires nothing, so the count never
 * advances - and a time bound alone makes a test's verdict depend on how busy
 * the host is.
 */
int rv_machine_run_bounded(uint64_t insn_limit, uint64_t us_limit);

/* Instructions retired since reset. Used for the MIPS figure on the console. */
uint64_t rv_machine_insns(void);

/*
 * Where the generated devicetree ended up, and how long it is. Valid after
 * rv_machine_init(); returns 0 before it. The host harness uses this to write
 * the blob out for dtc to check, which is the only way to know the generator
 * emits something a real parser accepts rather than something that merely
 * looks right.
 */
uint32_t rv_machine_fdt(uint32_t *size);

/* -------------------------------------------- services used by rv_hostsbi.c
 *
 * Not for general use. These exist so the SBI implementation can reach the
 * machine's timer, console and memory without including riscv_cpu_priv.h in
 * yet another file.
 */

/* The machine's mtime: monotonic, RV_TIMEBASE_HZ ticks per second. */
uint64_t rv_mtime(void);

/* Arm (or, with next == UINT64_MAX, disarm) the S-mode timer interrupt.
 * Clears any already-pending STIP, as SBI set_timer is specified to. */
void rv_set_timer(uint64_t next);

/* Raise or clear the S-mode software interrupt (SBI send_ipi / clear_ipi). */
void rv_set_ipi(bool level);

/* Drive a PLIC source. Used by rv_virtio.c, which owns source RV_VIRTIO_IRQ
 * and must not know how the interrupt controller is wired. */
void rv_plic_set_irq(int irq, bool level);

/*
 * A host pointer to `len` bytes of guest *physical* memory, or NULL if the
 * range is not entirely inside RAM. Used by the SBI debug-console extension,
 * which passes physical addresses.
 */
uint8_t *rv_guest_phys_ptr(uint64_t paddr, size_t len);

/* The guest asked to stop. Ends rv_machine_run() at the next slice boundary. */
void rv_request_poweroff(void);
void rv_request_reboot(void);

#endif /* RV_MACHINE_H_ */
