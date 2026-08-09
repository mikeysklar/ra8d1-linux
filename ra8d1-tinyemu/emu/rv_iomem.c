/*
 * rv_iomem.c - allocation-free replacement for TinyEMU's iomem.c.
 *
 * riscv_cpu.c reaches the guest's physical address space entirely through
 * iomem.h: get_phys_mem_range(), pr->phys_mem, pr->read_func/write_func, and
 * map->flush_tlb_write_range(). That interface is kept exactly; only the
 * implementation behind it changes.
 *
 * Three things differ from upstream:
 *
 *   1. The PhysMemoryMap is a single .bss object rather than a mallocz(), and
 *      phys_mem_map_end() does not free it. There is one machine.
 *   2. RAM is registered against a host pointer the caller already owns
 *      (rv_register_ram_ptr) instead of being allocated. On the board that
 *      pointer is the external SDRAM window; there is no allocator that could
 *      have produced 64 MB.
 *   3. Dirty-bit tracking is dropped. Upstream uses it only to tell a display
 *      or framebuffer device which guest pages changed; nothing here does
 *      that. The hooks remain so riscv_cpu.c's calls still compile, and
 *      DEVRAM_FLAG_DIRTY_BITS is rejected rather than silently ignored.
 *
 * MIT, as the rest of TinyEMU: this file is derived from iomem.c.
 * Copyright (c) 2016-2017 Fabrice Bellard.
 */

#include <stdint.h>
#include <string.h>

#include "cutils.h"
#include "iomem.h"

static PhysMemoryMap rv_phys_mem_map;

static PhysMemoryRange *rv_register_ram(PhysMemoryMap *s, uint64_t addr,
					uint64_t size, int devram_flags);
static void rv_free_ram(PhysMemoryMap *s, PhysMemoryRange *pr);
static const uint32_t *rv_get_dirty_bits(PhysMemoryMap *map,
					 PhysMemoryRange *pr);
static void rv_set_addr(PhysMemoryMap *map, PhysMemoryRange *pr,
			uint64_t addr, BOOL enabled);

PhysMemoryMap *phys_mem_map_init(void)
{
	PhysMemoryMap *s = &rv_phys_mem_map;

	memset(s, 0, sizeof(*s));
	s->register_ram = rv_register_ram;
	s->free_ram = rv_free_ram;
	s->get_dirty_bits = rv_get_dirty_bits;
	s->set_ram_addr = rv_set_addr;
	return s;
}

void phys_mem_map_end(PhysMemoryMap *s)
{
	s->n_phys_mem_range = 0;
}

/* return NULL if not found */
PhysMemoryRange *get_phys_mem_range(PhysMemoryMap *s, uint64_t paddr)
{
	PhysMemoryRange *pr;
	int i;

	for (i = 0; i < s->n_phys_mem_range; i++) {
		pr = &s->phys_mem_range[i];
		if (paddr >= pr->addr && paddr < pr->addr + pr->size) {
			return pr;
		}
	}
	return NULL;
}

PhysMemoryRange *register_ram_entry(PhysMemoryMap *s, uint64_t addr,
				    uint64_t size, int devram_flags)
{
	PhysMemoryRange *pr;

	if (s->n_phys_mem_range >= PHYS_MEM_RANGE_MAX) {
		return NULL;
	}
	if (size == 0 || (size & (DEVRAM_PAGE_SIZE - 1)) != 0) {
		return NULL;
	}
	pr = &s->phys_mem_range[s->n_phys_mem_range++];
	memset(pr, 0, sizeof(*pr));
	pr->map = s;
	pr->is_ram = TRUE;
	pr->devram_flags = devram_flags & ~DEVRAM_FLAG_DISABLED;
	pr->addr = addr;
	pr->org_size = size;
	pr->size = (devram_flags & DEVRAM_FLAG_DISABLED) ? 0 : size;
	return pr;
}

/*
 * The extension that makes this file work on a microcontroller: attach guest
 * RAM to memory the caller already has. `host_ptr` must remain valid for the
 * life of the machine and must be writable unless DEVRAM_FLAG_ROM is set.
 */
PhysMemoryRange *rv_register_ram_ptr(PhysMemoryMap *s, uint64_t addr,
				     uint64_t size, uint8_t *host_ptr,
				     int devram_flags)
{
	PhysMemoryRange *pr;

	if (host_ptr == NULL || (devram_flags & DEVRAM_FLAG_DIRTY_BITS)) {
		return NULL;
	}
	pr = register_ram_entry(s, addr, size, devram_flags);
	if (pr != NULL) {
		pr->phys_mem = host_ptr;
	}
	return pr;
}

/*
 * cpu_register_ram() routes here. There is no allocator that could satisfy it
 * on this target, so it always fails; callers must use rv_register_ram_ptr().
 * Nothing in riscv_cpu.c calls it - only a machine layer would - so this is a
 * guard against a future mistake, not a live path.
 */
static PhysMemoryRange *rv_register_ram(PhysMemoryMap *s, uint64_t addr,
					uint64_t size, int devram_flags)
{
	(void)s;
	(void)addr;
	(void)size;
	(void)devram_flags;
	return NULL;
}

static void rv_free_ram(PhysMemoryMap *s, PhysMemoryRange *pr)
{
	(void)s;
	pr->phys_mem = NULL;
}

static const uint32_t *rv_get_dirty_bits(PhysMemoryMap *map,
					 PhysMemoryRange *pr)
{
	(void)map;
	(void)pr;
	return NULL;
}

void phys_mem_reset_dirty_bit(PhysMemoryRange *pr, size_t offset)
{
	(void)pr;
	(void)offset;
}

PhysMemoryRange *cpu_register_device(PhysMemoryMap *s, uint64_t addr,
				     uint64_t size, void *opaque,
				     DeviceReadFunc *read_func,
				     DeviceWriteFunc *write_func,
				     int devio_flags)
{
	PhysMemoryRange *pr;

	if (s->n_phys_mem_range >= PHYS_MEM_RANGE_MAX || size > 0xffffffffu) {
		return NULL;
	}
	pr = &s->phys_mem_range[s->n_phys_mem_range++];
	memset(pr, 0, sizeof(*pr));
	pr->map = s;
	pr->addr = addr;
	pr->org_size = size;
	pr->size = (devio_flags & DEVIO_DISABLED) ? 0 : size;
	pr->is_ram = FALSE;
	pr->opaque = opaque;
	pr->read_func = read_func;
	pr->write_func = write_func;
	pr->devio_flags = devio_flags;
	return pr;
}

static void rv_set_addr(PhysMemoryMap *map, PhysMemoryRange *pr,
			uint64_t addr, BOOL enabled)
{
	if (enabled) {
		if (pr->size == 0 || pr->addr != addr) {
			if (pr->is_ram) {
				map->flush_tlb_write_range(map->opaque,
							   pr->phys_mem,
							   pr->org_size);
			}
			pr->addr = addr;
			pr->size = pr->org_size;
		}
	} else if (pr->size != 0) {
		if (pr->is_ram) {
			map->flush_tlb_write_range(map->opaque, pr->phys_mem,
						   pr->org_size);
		}
		pr->addr = 0;
		pr->size = 0;
	}
}

void phys_mem_set_addr(PhysMemoryRange *pr, uint64_t addr, BOOL enabled)
{
	PhysMemoryMap *map = pr->map;

	if (!pr->is_ram) {
		rv_set_addr(map, pr, addr, enabled);
	} else {
		map->set_ram_addr(map, pr, addr, enabled);
	}
}

/* return NULL if no valid RAM page. The access can only be done in the page */
uint8_t *phys_mem_get_ram_ptr(PhysMemoryMap *map, uint64_t paddr, BOOL is_rw)
{
	PhysMemoryRange *pr = get_phys_mem_range(map, paddr);
	uintptr_t offset;

	(void)is_rw;
	if (!pr || !pr->is_ram) {
		return NULL;
	}
	offset = (uintptr_t)(paddr - pr->addr);
	return pr->phys_mem + offset;
}

void irq_init(IRQSignal *irq, SetIRQFunc *set_irq, void *opaque, int irq_num)
{
	irq->set_irq = set_irq;
	irq->opaque = opaque;
	irq->irq_num = irq_num;
}
