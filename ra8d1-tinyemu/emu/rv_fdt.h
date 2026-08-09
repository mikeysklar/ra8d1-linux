/*
 * rv_fdt.h - generate the guest's devicetree at boot.
 *
 * The blob is built in RAM every boot rather than stored alongside the
 * kernel, because everything in it that can vary - how much SDRAM the board
 * actually reported, where the initrd landed, what the kernel command line
 * is - is only known at that point. It also removes an entire class of
 * failure the mini-rv32ima port had to live with, where a stale compiled-in
 * DTB silently disagreed with the machine (see ra8d1-linux/notes/03 and the
 * ramsweep logs).
 *
 * Writer scope: enough of DTB version 17 to describe this machine. Nodes,
 * string/u32/u64/cell-array/empty properties, and string dedup. No overlays,
 * no phandle fixups beyond the two this machine needs, no reading.
 */
#ifndef RV_FDT_H_
#define RV_FDT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint32_t ram_base;
	uint32_t ram_size;
	uint32_t timebase_hz;
	const char *isa;        /* e.g. "rv32ima" */
	const char *mmu_type;   /* e.g. "riscv,sv32", or NULL for a nommu guest */
	const char *cmdline;    /* may be NULL */
	uint32_t initrd_start;  /* both 0 for no initrd */
	uint32_t initrd_end;
	bool has_virtio_blk;    /* emit the virtio-mmio node */
	bool has_virtio_net;    /* emit the second virtio-mmio node (net) */
} RVFdtParams;

/*
 * Write a flattened devicetree into `buf`.
 *
 * Returns the number of bytes written, or 0 if the blob did not fit - which
 * is checked, not assumed, because the buffer lives in guest RAM and an
 * overrun would corrupt the kernel that was just loaded next to it.
 */
size_t rv_build_fdt(uint8_t *buf, size_t buf_size, const RVFdtParams *p);

#endif /* RV_FDT_H_ */
