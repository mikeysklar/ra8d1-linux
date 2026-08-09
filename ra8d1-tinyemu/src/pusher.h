/*
 * pusher.h - the RA8LDR TCP image loader, and the flash access it needs.
 *
 * The board serves the same push protocol the rvlinux app on this board
 * already serves, on the same port, so ra8d1-linux/rvlinux/tools/pushimage.py
 * drives it unmodified. That tool is the reason the protocol is not being
 * improved: it is tested, it is what the operator's notes describe, and a
 * gratuitous change here would be a change to the one part of this system that
 * is known to work.
 *
 * The one behavioural difference from rvlinux is forced by this app: rvlinux
 * copies its guest out of flash at boot and never reads the window again, so a
 * push can land under a running guest. Here the rootfs is read IN PLACE out of
 * the memory-mapped OSPI window by virtio-blk for as long as the guest runs,
 * and an OSPI-B device cannot serve memory-mapped reads while a program or
 * erase is in progress. So a push stops the guest first and the board reboots
 * into the new image afterwards. See src/pusher.c for the full argument and
 * notes/pusher.md for what was considered instead.
 *
 * Everything degrades to an inline no-op when the pusher is compiled out, so
 * src/main.c carries no #ifdefs - the same arrangement as telnet.h.
 */
#ifndef RVT_PUSHER_H_
#define RVT_PUSHER_H_

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------- flash for writing
 *
 * Implemented in src/platform_zephyr.c, which is the only file in this
 * application allowed to know what board this is. Reads go through the
 * memory-mapped window (plat_slot_base) and need none of this; erasing and
 * programming need the driver, and the driver needs offsets rather than
 * pointers.
 *
 * Declared unconditionally so that turning the pusher off does not silently
 * change what platform_zephyr.c provides.
 */
struct device;

/* The NOR the image slots live in, taken from the partition's parent node. */
const struct device *plat_flash_dev(void);

/*
 * A slot's offset in that device, and - through *writable - how much of it a
 * push may actually use. That is NOT always the devicetree partition size: see
 * the erase reserve in platform_zephyr.c. Returns UINT32_MAX for a bad slot.
 */
uint32_t plat_slot_offset(int slot, uint32_t *writable);

/* Erase granularity of the NOR, in bytes. */
uint32_t plat_flash_erase_size(void);

#ifdef CONFIG_RVT_PUSHER

/* Start the listener thread. Returns immediately; the thread waits for an
 * address of its own accord, exactly as the telnet thread does. */
void rvt_pusher_start(void);

/*
 * Called by main() once the emulator's run loop has returned, whatever stopped
 * it. Releases a push that is waiting for the guest to stop.
 *
 * Returns true if this app stopped the guest for a push, which is main()'s
 * cue to describe it as such rather than reporting a guest poweroff that did
 * not happen.
 */
bool rvt_pusher_guest_halted(void);

#else
static inline void rvt_pusher_start(void) { }
static inline bool rvt_pusher_guest_halted(void) { return false; }
#endif /* CONFIG_RVT_PUSHER */

#endif /* RVT_PUSHER_H_ */
