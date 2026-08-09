/*
 * video.h - the platform video interface.
 *
 * This header is deliberately portable: stdint/stdbool only, no Zephyr, no
 * FSP, no devicetree, no board constants. The machine layer includes this and
 * nothing else to get pixels onto a screen.
 *
 * The contract is:
 *
 *   - the machine renders its NATIVE raster (224x288 for a vertical arcade
 *     board) as 8-bit palette indices into the buffer plat_get_framebuffer()
 *     hands back;
 *   - plat_present() scales it, gets it to the display, and returns;
 *   - the machine never learns the panel size, the scale factor, the pixel
 *     format or how the flip happens.
 *
 * Scale and panel geometry are derived at runtime from devicetree inside the
 * implementation, not hardcoded here. A different panel, or a game with a
 * different raster, needs no change above this line.
 *
 * See notes/02-video.md for why the implementation looks the way it does.
 */

#ifndef ARCADE_VIDEO_H_
#define ARCADE_VIDEO_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Set up the display for a src_w x src_h 8bpp indexed raster. Picks the
 * largest integer scale that fits the panel and centres the result.
 *
 * Must run from a thread, after the display/MIPI-DSI/panel drivers have
 * initialised - not from an init hook.
 *
 * Returns 0, or negative errno. -ERANGE means the raster does not fit the
 * panel even at 1:1.
 */
int plat_video_init(unsigned src_w, unsigned src_h);

/*
 * The machine's drawing surface: src_w x src_h bytes of palette index, one
 * byte per pixel, plat_fb_stride() bytes per row. Stable for the lifetime of
 * the program - it does not flip, so the machine can keep a pointer and can
 * leave static background tiles in place between frames.
 */
uint8_t *plat_get_framebuffer(void);
unsigned plat_fb_stride(void);

/* Geometry, for callers that want to report or centre something. */
unsigned plat_fb_width(void);
unsigned plat_fb_height(void);
unsigned plat_scale(void);

/*
 * Load palette entries. Colours are 0x00RRGGBB (alpha is supplied by the
 * platform). Cheap, and safe to call while displaying: the hardware
 * double-buffers the palette and swaps it on the next vsync.
 */
void plat_set_palette(const uint32_t *rgb888, unsigned start, unsigned count);

/*
 * Block until the display is finished with the buffer we are about to draw
 * into. Call this at the top of a frame, before touching the framebuffer.
 */
void plat_wait_vsync(void);

/*
 * Scale the framebuffer to the display and show it. Returns as soon as the
 * flip is queued; the swap itself happens at the next vsync, so it is atomic
 * and cannot tear.
 */
void plat_present(void);

/* Frames presented since plat_video_init(). */
uint32_t plat_frame_count(void);

#endif /* ARCADE_VIDEO_H_ */
