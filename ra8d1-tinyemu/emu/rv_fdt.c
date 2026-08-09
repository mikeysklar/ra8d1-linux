/*
 * rv_fdt.c - flattened devicetree writer and this machine's tree.
 *
 * Layout produced (DTB v17):
 *
 *   +0x00  header, 40 bytes
 *   +0x28  memory reservation block, one terminating zero entry
 *   +0x38  structure block
 *   ...    strings block
 *
 * Everything in a DTB is big-endian regardless of the target, hence the
 * put_be32() on every store.
 */

#include <stdint.h>
#include <string.h>

#include "rv_fdt.h"
#include "rv_machine.h"

#define FDT_MAGIC        0xd00dfeedu
#define FDT_VERSION      17u
#define FDT_COMP_VERSION 16u

#define FDT_BEGIN_NODE 1u
#define FDT_END_NODE   2u
#define FDT_PROP       3u
#define FDT_END        9u

#define FDT_HDR_SIZE    40u
#define FDT_RSVMAP_SIZE 16u
#define FDT_STRUCT_OFF  (FDT_HDR_SIZE + FDT_RSVMAP_SIZE)

/* The strings block for this tree is ~300 bytes; 512 leaves room to add a
 * device without thinking about it, and overflow is reported, not ignored. */
#define FDT_STRINGS_MAX 512

typedef struct {
	uint8_t *buf;
	size_t buf_size;
	size_t pos;          /* write cursor into buf, struct block */
	char strings[FDT_STRINGS_MAX];
	size_t strings_len;
	int overflow;
} FDTState;

/* --------------------------------------------------------------- plumbing */

static void fdt_put_be32(uint8_t *d, uint32_t v)
{
	d[0] = (uint8_t)(v >> 24);
	d[1] = (uint8_t)(v >> 16);
	d[2] = (uint8_t)(v >> 8);
	d[3] = (uint8_t)v;
}

static void fdt_u32(FDTState *s, uint32_t v)
{
	if (s->pos + 4 > s->buf_size) {
		s->overflow = 1;
		return;
	}
	fdt_put_be32(s->buf + s->pos, v);
	s->pos += 4;
}

static void fdt_data(FDTState *s, const void *data, size_t len)
{
	size_t padded = (len + 3) & ~(size_t)3;

	if (s->pos + padded > s->buf_size) {
		s->overflow = 1;
		return;
	}
	memcpy(s->buf + s->pos, data, len);
	memset(s->buf + s->pos + len, 0, padded - len);
	s->pos += padded;
}

/* Intern a property name, returning its offset in the strings block. */
static uint32_t fdt_str(FDTState *s, const char *name)
{
	size_t i = 0;
	size_t len = strlen(name) + 1;

	while (i < s->strings_len) {
		if (strcmp(s->strings + i, name) == 0) {
			return (uint32_t)i;
		}
		i += strlen(s->strings + i) + 1;
	}
	if (s->strings_len + len > sizeof(s->strings)) {
		s->overflow = 1;
		return 0;
	}
	memcpy(s->strings + s->strings_len, name, len);
	i = s->strings_len;
	s->strings_len += len;
	return (uint32_t)i;
}

static void fdt_begin_node(FDTState *s, const char *name)
{
	fdt_u32(s, FDT_BEGIN_NODE);
	fdt_data(s, name, strlen(name) + 1);
}

/* "name@addr", formatted without stdio. */
static void fdt_begin_node_num(FDTState *s, const char *name, uint32_t addr)
{
	char buf[40];
	size_t n = 0;
	int shift;
	int leading = 1;

	while (name[n] != '\0' && n < sizeof(buf) - 12) {
		buf[n] = name[n];
		n++;
	}
	buf[n++] = '@';
	for (shift = 28; shift >= 0; shift -= 4) {
		uint32_t d = (addr >> shift) & 0xf;

		if (d != 0 || !leading || shift == 0) {
			leading = 0;
			buf[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
		}
	}
	buf[n] = '\0';
	fdt_begin_node(s, buf);
}

static void fdt_end_node(FDTState *s)
{
	fdt_u32(s, FDT_END_NODE);
}

static void fdt_prop(FDTState *s, const char *name, const void *data,
		     size_t len)
{
	fdt_u32(s, FDT_PROP);
	fdt_u32(s, (uint32_t)len);
	fdt_u32(s, fdt_str(s, name));
	fdt_data(s, data, len);
}

static void fdt_prop_empty(FDTState *s, const char *name)
{
	fdt_prop(s, name, NULL, 0);
}

static void fdt_prop_str(FDTState *s, const char *name, const char *val)
{
	fdt_prop(s, name, val, strlen(val) + 1);
}

static void fdt_prop_u32(FDTState *s, const char *name, uint32_t val)
{
	uint8_t b[4];

	fdt_put_be32(b, val);
	fdt_prop(s, name, b, sizeof(b));
}

/* A property that is a list of big-endian cells. */
static void fdt_prop_cells(FDTState *s, const char *name,
			   const uint32_t *cells, size_t n)
{
	size_t i;

	fdt_u32(s, FDT_PROP);
	fdt_u32(s, (uint32_t)(n * 4));
	fdt_u32(s, fdt_str(s, name));
	for (i = 0; i < n; i++) {
		fdt_u32(s, cells[i]);
	}
}

/*
 * A reg property under #address-cells = <2>, #size-cells = <2>. Both halves
 * of this machine's addresses fit in 32 bits, so the high cells are zero, but
 * they still have to be present or the parse is off by two cells.
 */
static void fdt_prop_reg64(FDTState *s, uint32_t addr, uint32_t size)
{
	uint32_t cells[4] = { 0, addr, 0, size };

	fdt_prop_cells(s, "reg", cells, 4);
}

/* ------------------------------------------------------------- the machine */

size_t rv_build_fdt(uint8_t *buf, size_t buf_size, const RVFdtParams *p)
{
	FDTState st;
	FDTState *s = &st;
	size_t struct_size;
	size_t total;
	uint32_t cells[4];
	uint32_t intc_phandle = 1;
	uint32_t plic_phandle = 2;
	uint32_t syscon_phandle = 3;

	if (buf_size < FDT_STRUCT_OFF + 64) {
		return 0;
	}
	memset(s, 0, sizeof(*s));
	s->buf = buf;
	s->buf_size = buf_size;
	s->pos = FDT_STRUCT_OFF;

	fdt_begin_node(s, "");
	fdt_prop_u32(s, "#address-cells", 2);
	fdt_prop_u32(s, "#size-cells", 2);
	fdt_prop_str(s, "compatible", "ra8d1,tinyemu");
	fdt_prop_str(s, "model", "ra8d1-tinyemu");

	/* ------------------------------------------------------------ cpus */
	fdt_begin_node(s, "cpus");
	fdt_prop_u32(s, "#address-cells", 1);
	fdt_prop_u32(s, "#size-cells", 0);
	fdt_prop_u32(s, "timebase-frequency", p->timebase_hz);

	fdt_begin_node_num(s, "cpu", 0);
	fdt_prop_str(s, "device_type", "cpu");
	fdt_prop_u32(s, "reg", 0);
	fdt_prop_str(s, "status", "okay");
	fdt_prop_str(s, "compatible", "riscv");
	fdt_prop_str(s, "riscv,isa", p->isa);
	if (p->mmu_type != NULL) {
		fdt_prop_str(s, "mmu-type", p->mmu_type);
	}
	/*
	 * clock-frequency is what the guest divides to get its BogoMIPS. It is
	 * not the host's 480 MHz and not a measured figure; Linux only uses it
	 * for the printed delay calibration on this arch, and the real rate
	 * depends on what the interpreter is executing. Left at TinyEMU's own
	 * value so a guest booted here and on a desktop TinyEMU reports the
	 * same number and nobody reads a difference into it.
	 */
	fdt_prop_u32(s, "clock-frequency", 2000000000u);

	fdt_begin_node(s, "interrupt-controller");
	fdt_prop_u32(s, "#interrupt-cells", 1);
	fdt_prop_empty(s, "interrupt-controller");
	fdt_prop_str(s, "compatible", "riscv,cpu-intc");
	fdt_prop_u32(s, "phandle", intc_phandle);
	fdt_end_node(s); /* interrupt-controller */

	fdt_end_node(s); /* cpu */
	fdt_end_node(s); /* cpus */

	/* ---------------------------------------------------------- memory */
	fdt_begin_node_num(s, "memory", p->ram_base);
	fdt_prop_str(s, "device_type", "memory");
	fdt_prop_reg64(s, p->ram_base, p->ram_size);
	fdt_end_node(s);

	/* ------------------------------------------------------------- soc */
	fdt_begin_node(s, "soc");
	fdt_prop_u32(s, "#address-cells", 2);
	fdt_prop_u32(s, "#size-cells", 2);
	fdt_prop_str(s, "compatible", "simple-bus");
	fdt_prop_empty(s, "ranges");

	/*
	 * CLINT. Declared with the M-mode software and timer interrupts, which
	 * is what the binding requires, even though this machine never
	 * delivers either: with an S-mode kernel the timer arrives via SBI and
	 * STIP. Linux's clint driver is not even built for an SBI-timer
	 * kernel; the node is here so the address range is described rather
	 * than looking unclaimed.
	 */
	fdt_begin_node_num(s, "clint", RV_CLINT_BASE);
	fdt_prop_str(s, "compatible", "riscv,clint0");
	cells[0] = intc_phandle;
	cells[1] = 3;  /* M software interrupt */
	cells[2] = intc_phandle;
	cells[3] = 7;  /* M timer interrupt */
	fdt_prop_cells(s, "interrupts-extended", cells, 4);
	fdt_prop_reg64(s, RV_CLINT_BASE, RV_CLINT_SIZE);
	fdt_end_node(s);

	/*
	 * PLIC, one context. Upstream TinyEMU lists the S-mode external
	 * interrupt first and the M-mode one second; only the first is listed
	 * here, because this machine never raises MEIP - there is no M-mode
	 * handler to take it. That also fixes the context index at 0, which is
	 * what the hart registers at PLIC + 0x200000 decode to.
	 */
	fdt_begin_node_num(s, "plic", RV_PLIC_BASE);
	fdt_prop_u32(s, "#interrupt-cells", 1);
	fdt_prop_empty(s, "interrupt-controller");
	fdt_prop_str(s, "compatible", "riscv,plic0");
	fdt_prop_u32(s, "riscv,ndev", 31);
	fdt_prop_reg64(s, RV_PLIC_BASE, RV_PLIC_SIZE);
	cells[0] = intc_phandle;
	cells[1] = 9;  /* S-mode external interrupt */
	fdt_prop_cells(s, "interrupts-extended", cells, 2);
	fdt_prop_u32(s, "phandle", plic_phandle);
	fdt_end_node(s);

	/* 8250, byte-wide registers, no shift - the model in rv_machine.c. */
	fdt_begin_node_num(s, "serial", RV_UART_BASE);
	fdt_prop_str(s, "compatible", "ns16550a");
	fdt_prop_reg64(s, RV_UART_BASE, RV_UART_SIZE);
	fdt_prop_u32(s, "clock-frequency", 1843200);
	fdt_prop_u32(s, "reg-shift", 0);
	fdt_prop_u32(s, "reg-io-width", 1);
	fdt_prop_u32(s, "interrupt-parent", plic_phandle);
	fdt_prop_u32(s, "interrupts", RV_UART_IRQ);
	fdt_end_node(s);

	/*
	 * virtio-mmio, at the same address QEMU's `virt` machine uses for its
	 * first slot. The guest kernel is a virt build, so keeping this one
	 * address familiar costs nothing and removes a difference from any
	 * boot log we might ever diff against.
	 *
	 * Emitted only when a rootfs was supplied. A virtio-mmio node with no
	 * device behind it is not harmless: the driver probes the magic value,
	 * reads zero and logs an error on every boot.
	 */
	if (p->has_virtio_blk) {
		fdt_begin_node_num(s, "virtio", RV_VIRTIO_BASE);
		fdt_prop_str(s, "compatible", "virtio,mmio");
		fdt_prop_reg64(s, RV_VIRTIO_BASE, RV_VIRTIO_SIZE);
		fdt_prop_u32(s, "interrupt-parent", plic_phandle);
		fdt_prop_u32(s, "interrupts", RV_VIRTIO_IRQ);
		fdt_end_node(s);
	}

	/*
	 * Poweroff and reboot, through the generic syscon bindings. This is
	 * the same 0x5555 / 0x7777 protocol mini-rv32ima used, so a guest
	 * image built for that port shuts down here unchanged.
	 */
	fdt_begin_node_num(s, "syscon", RV_SYSCON_BASE);
	fdt_prop_str(s, "compatible", "syscon");
	fdt_prop_reg64(s, RV_SYSCON_BASE, RV_SYSCON_SIZE);
	fdt_prop_u32(s, "phandle", syscon_phandle);
	fdt_end_node(s);

	fdt_end_node(s); /* soc */

	/*
	 * The two syscon consumers sit at the root, not under /soc. They have
	 * no reg of their own - they are just a value to poke into the syscon
	 * above - and dtc's simple_bus_reg check rightly complains about a
	 * regless node on a simple-bus.
	 */
	fdt_begin_node(s, "poweroff");
	fdt_prop_str(s, "compatible", "syscon-poweroff");
	fdt_prop_u32(s, "regmap", syscon_phandle);
	fdt_prop_u32(s, "offset", 0);
	fdt_prop_u32(s, "value", 0x5555);
	fdt_end_node(s);

	fdt_begin_node(s, "reboot");
	fdt_prop_str(s, "compatible", "syscon-reboot");
	fdt_prop_u32(s, "regmap", syscon_phandle);
	fdt_prop_u32(s, "offset", 0);
	fdt_prop_u32(s, "value", 0x7777);
	fdt_end_node(s);

	/*
	 * Deliberately not described: the paravirt I/O bridge at 0x11200000.
	 * A guest that does not know about it never touches the page, so the
	 * stock image boots here unchanged; the guest driver in
	 * ra8d1-linux/guest/pv-io.c binds by fixed address, not by node. See
	 * ra8d1-linux/notes/05-paravirt-io.md section 1.
	 */

	/* ---------------------------------------------------------- chosen */
	fdt_begin_node(s, "chosen");
	fdt_prop_str(s, "bootargs", p->cmdline ? p->cmdline : "");
	fdt_prop_str(s, "stdout-path", "/soc/serial@10000000");
	if (p->initrd_end > p->initrd_start) {
		fdt_prop_u32(s, "linux,initrd-start", p->initrd_start);
		fdt_prop_u32(s, "linux,initrd-end", p->initrd_end);
	}
	fdt_end_node(s);

	fdt_end_node(s); /* root */
	fdt_u32(s, FDT_END);

	if (s->overflow) {
		return 0;
	}

	struct_size = s->pos - FDT_STRUCT_OFF;
	total = s->pos + s->strings_len;
	if (total > buf_size) {
		return 0;
	}
	memcpy(buf + s->pos, s->strings, s->strings_len);

	memset(buf + FDT_HDR_SIZE, 0, FDT_RSVMAP_SIZE);
	fdt_put_be32(buf + 0,  FDT_MAGIC);
	fdt_put_be32(buf + 4,  (uint32_t)total);
	fdt_put_be32(buf + 8,  FDT_STRUCT_OFF);
	fdt_put_be32(buf + 12, (uint32_t)s->pos);        /* off_dt_strings */
	fdt_put_be32(buf + 16, FDT_HDR_SIZE);            /* off_mem_rsvmap */
	fdt_put_be32(buf + 20, FDT_VERSION);
	fdt_put_be32(buf + 24, FDT_COMP_VERSION);
	fdt_put_be32(buf + 28, 0);                       /* boot_cpuid_phys */
	fdt_put_be32(buf + 32, (uint32_t)s->strings_len);
	fdt_put_be32(buf + 36, (uint32_t)struct_size);

	return total;
}
