# 02 — Getting an arcade framebuffer onto the EK-RA8D1 panel

**RECOMMENDATION: approach 2, direct GLCDC scanout, with the framebuffer in
CLUT8 as a 448x576 sub-window of the panel. Expected cost ~2 ms per frame,
leaving ~88% of a 60 Hz frame for the emulator. 2x scale, not 3x — 3x does not
fit.**

Everything below is read out of the tree at
`circuitpython/ports/zephyr-cp/{zephyr,modules/hal/renesas}`. The board was not
flashed. **Nothing here is confirmed on hardware.** The last section lists what
is verified in source, what is inferred, and what has to be measured.

---

## 0. The interface the machine layer sees

`src/video.h` is portable C — `stdint.h` and `stdbool.h`, nothing else. No
Zephyr, no FSP, no devicetree, no board constants. The machine layer includes
it and nothing else:

```c
int      plat_video_init(unsigned src_w, unsigned src_h);
uint8_t *plat_get_framebuffer(void);   /* src_w x src_h, 8bpp indices */
unsigned plat_fb_stride(void);
void     plat_set_palette(const uint32_t *rgb888, unsigned start, unsigned n);
void     plat_wait_vsync(void);
void     plat_present(void);
```

The machine renders its **native** raster as palette indices and calls
`plat_present()`. It never learns the panel size, the scale factor, the pixel
format, or that there is a CLUT, a GLCDC or a flip at all. Scaling happens
below the line, inside `plat_present()`.

Board facts come from devicetree, not constants: `PANEL_W`/`PANEL_H` are
`DT_PROP(DT_CHOSEN(zephyr_display), width/height)`, and the scale factor is
computed at runtime as `MIN(PANEL_W / src_w, PANEL_H / src_h)`. A different
panel, or Galaga instead of Pac-Man, needs no change above the line and no
change to the constants below it either.

`src/video.c` is the EK-RA8D1 implementation and the only file that includes
`r_glcdc.h`.

## 1. Geometry: 3x is not possible

The brief said 3x gives 672x864, "slightly taller than the panel". The height is
the smaller problem. **672 > 480.** The panel is 480 columns wide and 3x needs
672 of them, so 3x overflows the width by 40%, not the height by 10 rows.
Cropping 192 columns out of a 224-column playfield takes the maze walls with it.

| scale | size | fits 480x854? |
|---|---|---|
| 1x | 224x288 | yes, uses 16% of the panel area |
| **2x** | **448x576** | **yes: 16 px left/right, 139 px top/bottom** |
| 2.14x | 480x617 | yes, width-limited maximum |
| 3x | 672x864 | no, 192 columns and 10 rows over |

2.14x is the largest uniform scale that fits. It buys 7% more linear size in
exchange for uneven column duplication (some source columns become 2 pixels,
some 3), which on 8x8 tile art and 1-pixel-wide maze lines is very visible. Not
worth it.

**Go with 2x integer, centred at (16, 139).** Pixels stay square, which matches
the source raster; a real Pac-Man CRT was 3:4 with near-square pixels too. The
278 spare rows are useful — they are the natural place for a score/status band
or bezel art, and they cost nothing because the GLCDC never reads them.

## 2. Why direct GLCDC, and what it means

`display_write()` is the wrong shape for this. It is a push API: the caller
hands over pixels and blocks. The GLCDC is not a push device — it is a DMA
master that reads a framebuffer out of SDRAM continuously, forever, with no
per-frame software involvement. A game wants the second model.

So: let Zephyr's driver do the bring-up (GLCDC open, MIPI-DSI host, ILI9806E
panel init, backlight GPIO), then never call `display_write()` again and drive
FSP directly.

That also sidesteps Gotcha 1 by construction. The `k_sem_take(frame_buf_sem,
K_FOREVER)` is at `display_renesas_ra.c:147`, inside `ra_display_write()`. If
`ra_display_write()` is never called, that line is unreachable. We do not need
to know whether the line-detect IRQ fires to be safe from it.

(For what it is worth, the evidence says the IRQ **does** fire.
`R_GLCDC_Start()` is called from exactly one place in the whole tree —
`display_renesas_ra.c:137`, verified by grep — and it is immediately followed by
`vsync_wait = true` and the unbounded `k_sem_take`. CircuitPython's `code.py`
demos reach the glass, which means they got past that wait on their first
write, which means the semaphore was given, which means the line-detect ISR ran.
The 2.9 fps is displayio's ~107 chunked memcpys per refresh, not a hardware
ceiling. That figure does not bound a C implementation.)

### The calls

```c
/* once */
R_GLCDC_Start(ctrl);                                   /* begin scanout      */
R_GLCDC_ClutUpdate(ctrl, &clut_cfg, LAYER_1);          /* load 256 colours   */
R_GLCDC_LayerChange(ctrl, &runtime_cfg, LAYER_1);      /* retarget the layer */

/* per frame */
sys_cache_data_flush_range(buf, VIDEO_FB_BYTES);
R_GLCDC_BufferChange(ctrl, buf, LAYER_1);              /* queue the flip     */
```

Implemented in `src/video.c` / `src/video.h`. Builds clean for
`ek_ra8d1` + `SHIELD=rtkmipilcdb00000be`.

### Getting at the FSP control block

`R_GLCDC_*` need a `glcdc_instance_ctrl_t *`, which the Zephyr driver keeps in
its private `struct display_ra_data`. That struct's **first** member is
`glcdc_instance_ctrl_t display_ctrl` (`display_renesas_ra.c:31-41`), so
`(glcdc_instance_ctrl_t *)dev->data` is the pointer we want. `video_init()`
checks `ctrl->state == DISPLAY_STATE_OPENED` right after the cast, which fails
loudly if that struct is ever reordered upstream.

Slightly grubby, but the alternative — dropping `CONFIG_RENESAS_RA_GLCDC` and
calling `R_GLCDC_Open()` ourselves — means reimplementing the module clock
enable, the IRQ wiring and the whole `display_cfg_t` from devicetree, and
re-solving the documented ordering constraint between the GLCDC, the DSI host
and the panel. Not worth it to avoid one cast.

## 3. Sub-window layer: the single biggest win

`display_layer_t.coordinate` positions the graphics layer anywhere in the
background plane, and `display_input_cfg_t.{hsize,vsize}` set how much the
hardware actually reads. Everything outside is filled with the background
colour by the GLCDC's background plane.

So the framebuffer is **448x576, not 480x854**. The letterbox costs zero bytes
of memory and zero bytes of bandwidth. Verified in
`r_glcdc.c:r_glcdc_graphics_layer_set()` / `r_glcdc_graphics_layer_param_recalculation()`,
which apply the coordinate, clip against the display window, and program
`GR[n].FLM2` (base), `FLM3` (line offset) and `FLM5` (transfer count).

> **The border defaults to white.** `RENESAS_RA_GLCDC_BG_COLOR()` in
> `display_renesas_ra.h:78-86` uses `DT_INST_PROP_OR(n, def_back_color_*, 255)`.
> Without an overlay you get a 448x576 game in a bright white frame. Add:
> ```dts
> &zephyr_lcdif {
> 	def-back-color-red = <0>;
> 	def-back-color-green = <0>;
> 	def-back-color-blue = <0>;
> };
> ```

## 4. Pixel format: CLUT8, and the palette expansion is free

The GLCDC has a **256-entry hardware CLUT per layer**, double-buffered, swapped
on vsync. `DISPLAY_IN_FORMAT_CLUT8` is in the FSP input format enum
(`r_display_api.h:81`) and in the driver's format LUT (`r_glcdc.c:289`).

This is exactly what a palettised arcade board wants. The emulator's tile/sprite
renderer already produces palette indices; we store indices; the GLCDC turns
them into ARGB8888 during scanout. **Palette expansion costs nothing and happens
nowhere in software.** Colour changes (level transitions, the power-pill flash)
are a `R_GLCDC_ClutUpdate()` call, not a reblit.

Zephyr's driver only ever configures RGB565/RGB888/ARGB8888 from devicetree
(`display_renesas_ra.h:16-18`) — CLUT8 is reachable only through
`R_GLCDC_LayerChange()`, i.e. only on the direct path. This is a concrete
capability that approach 1 cannot get to at all.

### Bandwidth, per frame at 60 Hz

| framebuffer | bytes/frame | scanout read | % of measured 123 MB/s |
|---|---|---|---|
| full panel RGB565 | 820,320 | 49.2 MB/s | 40% |
| 448x576 RGB565 | 516,096 | 31.0 MB/s | 25% |
| **448x576 CLUT8** | **258,048** | **15.5 MB/s** | **12.6%** |

Sub-windowing and CLUT8 together cut scanout bandwidth by 3.2x versus pushing
full RGB565 frames.

### Why not CLUT4

Pac-Man is 4bpp, so CLUT4 looks tempting: 129 KB per buffer, 7.8 MB/s scanout.
It fails the alignment rule. The GLCDC requires `hstride * bpp / 8` to be a
multiple of 64 bytes (`r_glcdc.c:26,1003-1008`). At 4bpp, 448 pixels is 224
bytes and 224 % 64 = 32, so the stride would have to be padded to 512 pixels,
and the doubling blit turns into nibble packing. All to save 7.7 MB/s on a bus
with over 100 MB/s spare. **CLUT8 at 448 bytes/line is exactly 7 x 64** — no
padding, no fiddling.

## 5. Scaling is software; the GLCDC has no scaler

Confirmed by absence: grepping `r_glcdc.c` for `scal|resize|zoom` returns
nothing. The GLCDC does positioning, alpha blending, colour correction, CLUT
expansion and dithering. No scaler.

(`lines_repeat_enable` is not one either. Per `r_glcdc.c:1573-1583` it makes the
hardware treat a whole region as a single line and repeat the **pattern** N
times — vertical tiling, not per-line duplication.)

So we pixel-double in software, inside `plat_present()`. `blit_2x()`: a
256-entry `uint16_t` table maps index `i` to `i | i<<8`, two source pixels are
packed into one 32-bit store, and the vertical copy is a `memcpy` of the row
just built. `blit_nx()` handles any other integer scale (correct rather than
fast — it exists so a different raster still renders).

Generated code (checked in the objdump of the linked ELF): the 2x inner loop is
6 instructions per 2 source pixels — 2 `ldrb`, 2 table `ldrh`, `orr`, `str` —
inside a zero-overhead `dls`/`le` loop, 112 iterations per row. Both paths were
verified exactly against a reference implementation on the host, at the real
strides: 0 mismatches out of 258,048 pixels at 2x, 0 out of 580,608 at 3x.

### Cost per frame

| | |
|---|---|
| source pixels | 224 x 288 = 64,512 |
| inner-loop iterations | 32,256 |
| instruction issue @ 480 MHz | ~0.2–0.4 ms |
| **SDRAM write of 252 KiB @ 150 MB/s** | **~1.7 ms** ← the real floor |
| cache clean, 8,064 lines of 32 B | ~0.05 ms |
| **total** | **~2 ms of a 16.7 ms frame (~12%)** |

Total SDRAM traffic is 15.5 MB/s written plus 15.5 MB/s read out of a bus
measured at 150/123. There is a lot of headroom.

If the blit ever does become the bottleneck, the fix is not a faster blit — it
is to delete it, by having the tile/sprite renderer emit 16x16 blocks straight
into the GLCDC buffer instead of 8x8 into an intermediate. That halves the
traffic. Not needed at 12%.

## 5b. On the LVGL 12 fps number

LVGL's 66 ms is not the number we have to beat five times over — it is a
different workload, and the honest comparison makes our case weaker-sounding
and more believable.

| | LVGL benchmark | this path |
|---|---|---|
| pixels touched | 480 x 854 = 410 K | 448 x 576 = 258 K |
| bytes/pixel written | 4 (ARGB8888) | 1 (CLUT8) |
| bytes/frame | ~1.6 MB | 258 KB |
| per-pixel work | rotate, sample, alpha-blend | table lookup, store |

That is **6x less data** through a per-pixel operation that is perhaps an order
of magnitude cheaper. Rotation with alpha compositing is a genuinely hard thing
to do on a Cortex-M85; integer pixel doubling of palette indices is close to
the cheapest thing a CPU can do to memory.

So 12 fps on that benchmark and 60 fps on this one are not in tension. If our
path measures anywhere near LVGL's 66 ms per frame, something is wrong and the
first thing to check is the cache flush (§7), not the arithmetic.

## 6. Double buffering and tearing

Two 448x576 CLUT8 buffers = 516 KB of 64 MB. `R_GLCDC_BufferChange()` writes the
new base into the shadow register and sets `GR[n].VEN.PVEN`; the hardware
latches it at the next vsync and clears `PVEN` itself (`r_glcdc.c`,
`R_GLCDC_BufferChange`). **The flip is atomic at vsync — no tearing, ever.**

`video_wait_flip()` polls `PVEN` and yields. This is a vsync wait that needs
neither the driver's semaphore nor the line-detect callback, so it is immune to
whatever is going on with that IRQ.

`R_GLCDC_LayerChange()` and `R_GLCDC_BufferChange()` both return
`FSP_ERR_INVALID_UPDATE_TIMING` if an update is already pending; `video.c`
retries with a yield rather than treating it as an error.

## 7. Cache

`CONFIG_CPU_RA_DCACHE_WRITEBACK=y` and `CONFIG_DCACHE_LINE_SIZE=32` in the
resulting `.config`. The GLCDC is a bus master and does not snoop the D-cache,
and the framebuffer is in SDRAM. **Every frame must be flushed before the flip**
— `sys_cache_data_flush_range()`, in `video_present()`.

This is Hermes's open lead from `ra8d1-cli.md`, and it is real:
`display_renesas_ra.c` contains no cache maintenance anywhere. The symptom would
be intermittent block corruption, not a blank panel.

The alternative — a non-cacheable MPU region — makes the CPU's 258 KB of writes
per frame go straight to SDRAM uncached, which is much worse than one bulk clean
at the end. Cached plus explicit flush is right.

## 8. Refresh rate — the one number that needs a scope

Timings from `boards/shields/rtkmipilcdb00000be/rtkmipilcdb00000be.overlay`:

```
htotal = 480 + 5 hbp + 72 hfp +  2 hsync = 559
vtotal = 854 + 20 vbp + 17 vfp +  3 vsync = 894
                                            -> 499,746 pixel clocks per frame
```

The dot clock is `LCDCLK / output-clock-divisor`. The shield does not set
`output-clock-divisor`, so it defaults to **8**
(`display_renesas_ra.h:97-98`).

`LCDCLK` is where it gets uncertain. `ek_ra8d1.dts:201` has
`&lcdclk { clocks = <&pll>; div = <2>; }`, and `&pll` is xtal 20 MHz / 2 x 96 =
**960 MHz** VCO with `pllp` = 480 MHz (both read out of the generated
`zephyr.dts`). Taken literally that gives LCDCLK = 480 MHz, dot clock 60 MHz,
and **120.06 Hz** refresh. But 480 MHz LCDCLK is above what I believe the RA8D1
allows, and if the CGC actually selects PLL1P then LCDCLK = 240 MHz, dot clock
30.0 MHz, and refresh = **60.03 Hz**.

30.0 MHz landing on 60.03 Hz to three digits is not a coincidence — these
timings were chosen for a 30 MHz dot clock. I did not find where Zephyr programs
`LCDCKDIVCR`, so I am calling this **inferred**: most likely 60.03 Hz.

**It does not block anything either way.** `output-clock-divisor` accepts
1–9, 12, 16, 24, 32, so once the real LCDCLK is known we pick the divisor that
lands on 30 MHz. And 120 Hz would be fine too — present each emulator frame
twice, with lower latency.

Optional refinement: Pac-Man's real rate is 60.606 Hz (6.144 MHz over 384x264).
At a 30.0 MHz dot clock, retuning the porches to htotal 550 / vtotal 900 gives
495,000 clocks = **60.606 Hz exactly**, and then the emulator needs no frame
pacing at all. Caveat: the DSI video-mode parameters in the shield overlay
(`timing = <1183 11 26 40>`, `video-mode-delay`) are derived from the horizontal
timing, so they would need rechecking. Leave this alone for v1.

## 9. Dave2D: not available, and not needed

Two independent reasons, the first one fatal.

**It does not build.** The D/AVE 2D library is a binary blob, not source.
`modules/hal/renesas/drivers/tes/dave2d/` contains 5 headers and **zero `.c`
files**; `drivers/ra/CMakeLists.txt:127` links
`zephyr/blobs/dave2d/libdave2d.a`, and that directory holds only two licence
files. Building with `CONFIG_RENESAS_DRW=y` fails:

```
ninja: error: '.../modules/hal/renesas/zephyr/blobs/dave2d/libdave2d.a',
needed by 'zephyr/zephyr_pre0.elf', missing and no known rule to make it
```

Fixable with `west blobs fetch hal_renesas`, which pulls it from
`github.com/renesas/libdave2d`. That is a deliberate decision about vendoring a
binary blob, not something to do casually.

**And it would not help.** Renesas' "cuts CPU load by half to two-thirds" is
about vector/UI rendering — paths, gradients, anti-aliased shapes — where the
CPU cost is per-primitive. Ours is a flat memory-to-memory expansion. `d2_blitcopy()`
does scale (separate `srcwidth/srcheight` and `dstwidth/dstheight`, header line
625), but the DRW writes the same 258 KB to the same SDRAM over the same bus.
It would offload ~0.3 ms of instruction issue and leave the ~1.7 ms of SDRAM
writes exactly where they are. Worse, Dave2D's framebuffer formats are RGB565 /
ARGB8888, not CLUT8 — using it means giving up the free hardware CLUT and
doubling the scanout read back to 31 MB/s.

Revisit only if the display ends up doing rotation, smooth scaling, or
alpha-blended overlays.

## 10. Configuration

`prj.conf`:
```
CONFIG_DISPLAY=y
CONFIG_MIPI_DSI=y
CONFIG_RENESAS_RA_GLCDC=y
CONFIG_RENESAS_RA_GLCDC_FB_NUM=1
CONFIG_I2C=y                  # the GT911 touch controller shares the DSI reset line
CONFIG_MEMC=y                 # SDRAM controller
CONFIG_ICACHE=y
CONFIG_DCACHE=y
CONFIG_CACHE_MANAGEMENT=y     # sys_cache_data_flush_range()
CONFIG_SPEED_OPTIMIZATIONS=y
```

`FB_NUM=1` deliberately, not 0. It costs 820 KB of SDRAM for a driver
framebuffer we never draw into, but it means `R_GLCDC_Start()` has a real,
allocated, blackened buffer to scan for the frame or two before our
`LayerChange` latches. With `FB_NUM=0` the driver's `input[0].p_base` points at
a zero-length array and the hardware would scan 820 KB of arbitrary SDRAM on
the way up. 820 KB of 64 MB is not worth arguing about.

Build:
```
west build -b ek_ra8d1 <app> -- -DSHIELD=rtkmipilcdb00000be
```
The shield is **mandatory**. The panel, the DSI host and `chosen zephyr,display`
all come from `rtkmipilcdb00000be.overlay`; the bare board has no display nodes
enabled.

Flash with `ra8d1-linux/rvlinux/flash.sh` (probe-rs, option-setting sections
stripped). **Not done — the board is in use.**

---

## Verified vs inferred

**Verified by reading source in this tree:**
- 224*3 = 672 > 480. Panel is 480x854 (`rtkmipilcdb00000be.overlay:34-35,43-44`).
- Panel timings, htotal 559 / vtotal 894 (same file, `display-timings`).
- GLCDC has no scaler (no `scal|resize|zoom` anywhere in `r_glcdc.c`).
- CLUT8/4/1 are supported input formats (`r_display_api.h:81-83`, `r_glcdc.c:289`).
- CLUT is 256 entries, double-buffered, swapped on vsync (`R_GLCDC_ClutUpdate`).
- Layer coordinate and hsize/vsize sub-windowing (`r_glcdc_graphics_layer_set`).
- 64-byte alignment on base address and byte stride (`r_glcdc.c:26,1003-1008`);
  448 bytes = 7 x 64 satisfies it.
- Layer width must be even (`r_glcdc.c:1129`); 448 is.
- `BufferChange` sets `PVEN`, hardware clears it at vsync.
- Line-detect IRQ fires at `vbp + vactive + 2`, i.e. just past the last visible
  line (`r_glcdc.c:421-423`).
- `R_GLCDC_Start()` has exactly one caller in the tree, `display_renesas_ra.c:137`.
- FSP parameter checking is compiled **out** for RA (`BSP_CFG_PARAM_CHECKING_ENABLE 0`),
  so `LayerChange`'s "must be DISPLAYING" precondition is not enforced — but the
  `PVEN`/`VEN` timing checks are outside the `#if` and still apply.
- Background colour defaults to white (`display_renesas_ra.h:78-86`).
- Write-back D-cache, 32-byte lines, and no cache maintenance anywhere in the
  Zephyr display driver.
- Dave2D blob is absent; enabling `CONFIG_RENESAS_DRW` fails to link. Reproduced.
- `src/video.c` compiles and links for `ek_ra8d1` + shield, and derives panel
  geometry from devicetree.
- Both blit paths are exact at the real strides (host reference check:
  0/258,048 wrong at 2x, 0/580,608 at 3x).

**Inferred, not proven:**
- Refresh is 60.03 Hz (dot clock 30 MHz). Alternative reading gives 120.06 Hz.
  See §8.
- The line-detect IRQ does fire, argued from CircuitPython reaching the glass.
  We do not depend on it.
- The ~2 ms/frame blit cost. Instruction counts are from the real disassembly;
  the SDRAM term uses the 150 MB/s figure measured by another workstream, not
  measured for this access pattern.
- `(glcdc_instance_ctrl_t *)dev->data` — correct by struct layout, checked at
  runtime by `video_init()`, but never executed.

**Must be measured on hardware:**
1. Actual refresh rate, and therefore the right `output-clock-divisor`.
2. That `R_GLCDC_LayerChange()` to CLUT8 with a sub-window actually latches, and
   the border comes up black.
3. Sustained frame rate with the blit running, and the real SDRAM cost.
4. That the cache flush is sufficient — corruption here looks like blocky
   garbage, not a blank screen.
