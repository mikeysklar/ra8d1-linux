/*
 * video.c - EK-RA8D1 implementation of the platform video interface.
 *
 * Everything board-specific lives below this line: the panel geometry comes
 * from devicetree, and the GLCDC is driven through FSP directly.
 *
 * The Zephyr display driver brings the GLCDC, the MIPI-DSI host and the
 * ILI9806E panel up for us. We then stop using its display_write() API
 * entirely:
 *
 *   R_GLCDC_Start        once, to begin scanout
 *   R_GLCDC_ClutUpdate   to load the palette into the hardware CLUT
 *   R_GLCDC_LayerChange  once, to retarget layer 1 at our CLUT8 sub-window
 *   R_GLCDC_BufferChange per frame, to flip
 *
 * display_write() is never called, so the k_sem_take(frame_buf_sem, K_FOREVER)
 * at display_renesas_ra.c:147 is never reached. That is the whole reason for
 * going around the Zephyr display API rather than through it.
 *
 * The GLCDC scans out of SDRAM continuously and asynchronously. Per frame we
 * make exactly one FSP call, and only to swap which buffer it reads.
 */

#include "video.h"

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/devicetree_regions.h>
#include <zephyr/logging/log.h>

#include <string.h>

#include <r_glcdc.h>

LOG_MODULE_REGISTER(video, LOG_LEVEL_INF);

#define GLCDC_NODE DT_CHOSEN(zephyr_display)
#define SDRAM_NODE DT_NODELABEL(sdram1)

/* Board facts, from devicetree rather than constants. */
#define PANEL_W DT_PROP(GLCDC_NODE, width)
#define PANEL_H DT_PROP(GLCDC_NODE, height)

BUILD_ASSERT(DT_NODE_HAS_COMPAT(GLCDC_NODE, renesas_ra_glcdc),
	     "chosen zephyr,display must be the GLCDC (build with SHIELD=rtkmipilcdb00000be)");

/*
 * The GLCDC requires the framebuffer base address and the byte stride to both
 * be multiples of 64 (r_glcdc.c:26, 1003-1008).
 */
#define GLCDC_ALIGN 64
#define ALIGN64(x)  (((x) + (GLCDC_ALIGN - 1)) & ~(GLCDC_ALIGN - 1))

/*
 * Worst case sizing for the two scanout buffers: a full-panel-width layer with
 * a 64-byte-aligned stride. At 480x854 CLUT8 that is 512 * 854 = 437,248 bytes
 * each. The real layer is smaller; this is just the static reservation, and
 * 64 MB of SDRAM makes the slack irrelevant.
 */
#define SCANOUT_MAX_BYTES (ALIGN64(PANEL_W) * PANEL_H)

/* Likewise for the machine's own 8bpp surface. */
#define SRC_MAX_BYTES (ALIGN64(PANEL_W) * PANEL_H)

Z_GENERIC_SECTION(LINKER_DT_NODE_REGION_NAME(SDRAM_NODE))
static uint8_t __aligned(GLCDC_ALIGN) scanout[2][SCANOUT_MAX_BYTES];

Z_GENERIC_SECTION(LINKER_DT_NODE_REGION_NAME(SDRAM_NODE))
static uint8_t __aligned(GLCDC_ALIGN) surface[SRC_MAX_BYTES];

/*
 * i -> i,i as a little-endian halfword. Doubling a pixel becomes one table
 * read and one store instead of a shift and two stores.
 */
static uint16_t dup2[256];

static struct {
	glcdc_instance_ctrl_t *glcdc;
	unsigned src_w, src_h;
	unsigned src_stride;
	unsigned scale;
	unsigned dst_w, dst_h;
	unsigned dst_stride;
	uint8_t back;
	uint32_t frames;
	bool ready;
} vid;

/*
 * struct display_ra_data is private to the Zephyr driver, but its first member
 * is the FSP control block:
 *
 *   struct display_ra_data {
 *           glcdc_instance_ctrl_t display_ctrl;    <- offset 0
 *           display_cfg_t display_fsp_cfg;
 *           ...
 *   };
 *
 * so dev->data is a valid glcdc_instance_ctrl_t *. plat_video_init() checks
 * the state field straight afterwards, which fails loudly if that struct is
 * ever reordered upstream.
 */
static glcdc_instance_ctrl_t *glcdc_ctrl_of(const struct device *dev)
{
	return (glcdc_instance_ctrl_t *)dev->data;
}

/* The GLCDC clears PVEN once the shadow registers have been latched at vsync. */
static inline bool flip_pending(void)
{
	return R_GLCDC->GR[DISPLAY_FRAME_LAYER_1].VEN_b.PVEN != 0U;
}

/* --- scaling ------------------------------------------------------------ */

/*
 * 2x is the case that matters (224x288 on a 480x854 panel), so it gets a
 * dedicated path: two source pixels are table-doubled into halfwords, packed
 * into one 32-bit store, and the second output row is a memcpy of the first.
 */
static void blit_2x(const uint8_t *src, uint8_t *dst)
{
	for (unsigned y = 0; y < vid.src_h; y++) {
		const uint8_t *s = src + (size_t)y * vid.src_stride;
		uint32_t *d = (uint32_t *)dst;

		for (unsigned x = 0; x < vid.src_w; x += 2) {
			*d++ = (uint32_t)dup2[s[x]] | ((uint32_t)dup2[s[x + 1]] << 16);
		}

		memcpy(dst + vid.dst_stride, dst, vid.dst_w);
		dst += 2 * vid.dst_stride;
	}
}

/* Any integer scale, including 1x. Correct rather than fast. */
static void blit_nx(const uint8_t *src, uint8_t *dst)
{
	const unsigned n = vid.scale;

	for (unsigned y = 0; y < vid.src_h; y++) {
		const uint8_t *s = src + (size_t)y * vid.src_stride;
		uint8_t *d = dst;

		for (unsigned x = 0; x < vid.src_w; x++) {
			memset(d, s[x], n);
			d += n;
		}

		for (unsigned r = 1; r < n; r++) {
			memcpy(dst + (size_t)r * vid.dst_stride, dst, vid.dst_w);
		}

		dst += (size_t)n * vid.dst_stride;
	}
}

/* --- interface ---------------------------------------------------------- */

int plat_video_init(unsigned src_w, unsigned src_h)
{
	display_runtime_cfg_t rt;
	const struct device *dev;
	fsp_err_t err;

	if (src_w == 0 || src_h == 0) {
		return -EINVAL;
	}

	/* Largest integer scale that fits both axes. */
	unsigned scale = MIN(PANEL_W / src_w, PANEL_H / src_h);

	if (scale == 0) {
		LOG_ERR("%ux%u does not fit a %ux%u panel even at 1:1", src_w, src_h,
			(unsigned)PANEL_W, (unsigned)PANEL_H);
		return -ERANGE;
	}

	vid.src_w = src_w;
	vid.src_h = src_h;
	vid.src_stride = ALIGN64(src_w);
	vid.scale = scale;
	vid.dst_w = src_w * scale;
	vid.dst_h = src_h * scale;
	vid.dst_stride = ALIGN64(vid.dst_w);

	if ((size_t)vid.src_stride * src_h > sizeof(surface) ||
	    (size_t)vid.dst_stride * vid.dst_h > sizeof(scanout[0])) {
		return -ENOMEM;
	}

	/* GLCDC rejects an odd layer width (r_glcdc.c:1129). */
	if (vid.dst_w % 2) {
		LOG_ERR("scaled width %u is odd; GLCDC needs it even", vid.dst_w);
		return -EINVAL;
	}

	for (unsigned i = 0; i < 256; i++) {
		dup2[i] = (uint16_t)(i | (i << 8));
	}

	dev = DEVICE_DT_GET(GLCDC_NODE);
	if (!device_is_ready(dev)) {
		LOG_ERR("GLCDC device not ready");
		return -ENODEV;
	}

	vid.glcdc = glcdc_ctrl_of(dev);
	if (vid.glcdc->state != DISPLAY_STATE_OPENED) {
		LOG_ERR("GLCDC state %d, expected OPENED(1) - dev->data cast is wrong",
			(int)vid.glcdc->state);
		return -EINVAL;
	}

	/*
	 * The driver's own framebuffer (CONFIG_RENESAS_RA_GLCDC_FB_NUM=1) is
	 * what the hardware scans for the frame or two before our LayerChange
	 * is latched. Black it out so boot does not flash garbage.
	 */
	uint8_t *dt_fb = (uint8_t *)display_get_framebuffer(dev);
	const size_t dt_fb_len = (size_t)PANEL_W * PANEL_H * 2;

	if (dt_fb != NULL) {
		memset(dt_fb, 0, dt_fb_len);
		sys_cache_data_flush_range(dt_fb, dt_fb_len);
	}

	memset(surface, 0, sizeof(surface));
	memset(scanout, 0, sizeof(scanout));
	sys_cache_data_flush_range(scanout, sizeof(scanout));

	/* Scanout has to be running before LayerChange has a vsync to latch on. */
	err = R_GLCDC_Start((display_ctrl_t *)vid.glcdc);
	if (err != FSP_SUCCESS) {
		LOG_ERR("R_GLCDC_Start failed (%d)", (int)err);
		return -EIO;
	}

	/*
	 * Retarget layer 1 at our CLUT8 buffer, sized and positioned so the
	 * hardware reads only the active area and paints the rest with the
	 * background colour. Set def-back-color-{red,green,blue} = 0 in the
	 * devicetree or that border comes out white.
	 */
	rt.input = (display_input_cfg_t){
		.p_base = (uint32_t *)scanout[0],
		.hsize = (uint16_t)vid.dst_w,
		.vsize = (uint16_t)vid.dst_h,
		.hstride = vid.dst_stride, /* in pixels, despite the FSP comment */
		.format = DISPLAY_IN_FORMAT_CLUT8,
		.line_descending_enable = false,
		.lines_repeat_enable = false,
		.lines_repeat_times = 0,
	};
	rt.layer = (display_layer_t){
		.coordinate = {.x = (int16_t)((PANEL_W - vid.dst_w) / 2),
			       .y = (int16_t)((PANEL_H - vid.dst_h) / 2)},
		.bg_color = {.byte = {.a = 0xFF, .r = 0, .g = 0, .b = 0}},
		.fade_control = DISPLAY_FADE_CONTROL_NONE,
		.fade_speed = 0,
	};

	do {
		err = R_GLCDC_LayerChange((display_ctrl_t *)vid.glcdc, &rt,
					  DISPLAY_FRAME_LAYER_1);
		if (err == FSP_ERR_INVALID_UPDATE_TIMING) {
			k_msleep(1);
		}
	} while (err == FSP_ERR_INVALID_UPDATE_TIMING);

	if (err != FSP_SUCCESS) {
		LOG_ERR("R_GLCDC_LayerChange failed (%d)", (int)err);
		return -EIO;
	}

	vid.back = 1;
	vid.frames = 0;
	vid.ready = true;

	LOG_INF("video: %ux%u x%u -> %ux%u CLUT8 at (%u,%u) on %ux%u panel", src_w, src_h, scale,
		vid.dst_w, vid.dst_h, (PANEL_W - vid.dst_w) / 2, (PANEL_H - vid.dst_h) / 2,
		(unsigned)PANEL_W, (unsigned)PANEL_H);
	LOG_INF("video: %u KiB surface + 2 x %u KiB scanout in SDRAM",
		(unsigned)(vid.src_stride * src_h / 1024),
		(unsigned)(vid.dst_stride * vid.dst_h / 1024));

	return 0;
}

uint8_t *plat_get_framebuffer(void)
{
	return surface;
}

unsigned plat_fb_stride(void)
{
	return vid.src_stride;
}

unsigned plat_fb_width(void)
{
	return vid.src_w;
}

unsigned plat_fb_height(void)
{
	return vid.src_h;
}

unsigned plat_scale(void)
{
	return vid.scale;
}

void plat_set_palette(const uint32_t *rgb888, unsigned start, unsigned count)
{
	/* The CLUT wants ARGB8888; the caller should not have to care. */
	static uint32_t argb[256];
	display_clut_cfg_t cfg;

	if (!vid.ready || start >= 256) {
		return;
	}

	count = MIN(count, 256 - start);

	for (unsigned i = 0; i < count; i++) {
		argb[i] = 0xFF000000u | (rgb888[i] & 0x00FFFFFFu);
	}

	cfg.p_base = argb;
	cfg.start = (uint16_t)start;
	cfg.size = (uint16_t)count;

	(void)R_GLCDC_ClutUpdate((display_ctrl_t *)vid.glcdc, &cfg, DISPLAY_FRAME_LAYER_1);
}

void plat_wait_vsync(void)
{
	while (vid.ready && flip_pending()) {
		k_yield();
	}
}

void plat_present(void)
{
	uint8_t *dst;
	fsp_err_t err;

	if (!vid.ready) {
		return;
	}

	dst = scanout[vid.back];

	if (vid.scale == 2) {
		blit_2x(surface, dst);
	} else {
		blit_nx(surface, dst);
	}

	/*
	 * The GLCDC is a bus master and does not see the D-cache. Without this
	 * the panel shows whatever made it out of the write buffer, which looks
	 * like intermittent block corruption rather than a blank screen.
	 */
	sys_cache_data_flush_range(dst, (size_t)vid.dst_stride * vid.dst_h);

	do {
		err = R_GLCDC_BufferChange((display_ctrl_t *)vid.glcdc, dst,
					   DISPLAY_FRAME_LAYER_1);
		if (err == FSP_ERR_INVALID_UPDATE_TIMING) {
			k_yield();
		}
	} while (err == FSP_ERR_INVALID_UPDATE_TIMING);

	vid.back ^= 1;
	vid.frames++;
}

uint32_t plat_frame_count(void)
{
	return vid.frames;
}
