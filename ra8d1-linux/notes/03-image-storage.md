# 03 - Getting the Linux image onto the EK-RA8D1 and into memory

Question: a RV32IMA Linux kernel + rootfs is multi-megabyte. The RA8D1 has 2 MB of
internal flash. Where does the image live, and how does it get into the 64 MB SDRAM
at `0x68000000` at boot?

**Answer: external OSPI NOR flash. It is already wired up, already has a working
Zephyr driver in our tree, is already enabled in the build we have, and is
memory-mapped at `0x90000000` so reading it is a `memcpy`.**

---

## 0. The number that settles everything

The image we already have on disk:

| File | Size |
|---|---|
| `ra8d1-linux/image/Image` (linux 6.1.14 rv32-nommu, buildroot rootfs baked in) | **3,476,752 B (3.32 MiB)** |
| `emulator/mini-rv32ima/sixtyfourmb.dtb` | 1,536 B (and there is a compiled-in copy in `default64mbdtc.h`, so this does not need storing) |

Internal code flash on the R7FA8D1BH is **2016 KiB (1.97 MiB)** total
(`flash0: flash@2000000`, `reg = <0x02000000 DT_SIZE_K(2016)>`).

The image is **1.7x larger than the entire internal flash**, before a single byte
of Zephyr or the emulator. Option 3 is dead on arithmetic alone; see section 4.

---

## 1. RECOMMENDATION

Store the image in the on-board **Infineon S28HL512T 64 MB octo-SPI NOR** at byte
offset **`0x40000`** (256 KiB), i.e. CPU address **`0x90040000`**.

At boot the app does:

```c
memcpy((void *)0x68000000, (const void *)0x90040000, image_len);
memset((uint8_t *)0x68000000 + image_len, 0, 64*1024*1024 - image_len - dtb_reserve);
```

No flash API call is needed for the read. The OSPI window is a normal cacheable
memory region once the driver has initialised the chip into octal DTR mode.

Get the image into flash the first time with a **one-shot UART receiver app** that
`flash_write()`s what it receives. ~5 min at 115200, ~40 s at 921600. One time per
image revision, then every subsequent boot is just the memcpy above.

---

## 2. Path 1: External OSPI flash (RECOMMENDED)

### 2.1 Devicetree

Node is defined in the board DTS, not an overlay, and is already `status = "okay"`:

`zephyr/boards/renesas/ek_ra8d1/ek_ra8d1.dts`

```dts
&ospi0 {
	pinctrl-0 = <&ospi0_default>;
	status = "okay";

	s28hl512t: ospi-nor-flash@90000000 {
		compatible = "renesas,ra-ospi-b-nor";
		protocol-mode = <XSPI_OCTO_MODE>;      /* 8 data lines */
		data-rate = <XSPI_DTR_TRANSFER>;       /* DDR */
		ospi-max-frequency = <DT_FREQ_M(200)>;
		reg = <0x90000000 DT_SIZE_M(64)>;
		write-block-size = <1>;
		status = "okay";
		...
	};
};
```

Controller: `ospi0: ospi@40268000`, `compatible = "renesas,ra-ospi-b"`, in
`zephyr/dts/arm/renesas/ra/ra8/ra8x1.dtsi:694`. Clocked from `&octaspiclk`, which the
board DTS sources from `pll2p` (400 MHz) / 2.

Node paths for `DEVICE_DT_GET`:

```c
#define OSPI_FLASH_NODE DT_NODELABEL(s28hl512t)          /* /soc/ospi@40268000/ospi-nor-flash@90000000 */
const struct device *flash = DEVICE_DT_GET(OSPI_FLASH_NODE);
```

### 2.2 Memory-mapped base address: `0x90000000` (VERIFIED in source)

Two independent confirmations:

1. `modules/hal/renesas/drivers/ra/fsp/src/bsp/mcu/ra8d1/bsp_feature.h:464-465`
   ```c
   #define BSP_FEATURE_OSPI_B_DEVICE_0_START_ADDRESS  (0x80000000UL)  // CS0
   #define BSP_FEATURE_OSPI_B_DEVICE_1_START_ADDRESS  (0x90000000UL)  // CS1
   ```
   The S28HL512T is on **CS1** (`.channel = OSPI_B_DEVICE_NUMBER_1` in the driver),
   hence `0x90000000`. CS0 at `0x80000000` is unpopulated on this board.

2. The Zephyr read implementation is literally a memcpy from that window:
   `zephyr/drivers/flash/flash_renesas_ra_ospi_b.c:560`
   ```c
   memcpy(data, (uint8_t *)(BSP_FEATURE_OSPI_B_DEVICE_1_START_ADDRESS) + offset, len);
   ```

So yes, XIP-style reads work. `flash_read()` and a direct pointer dereference are
the same thing. The emulator could in principle fetch guest instructions straight
from `0x90000000+`, but it must not: the guest writes to its own image (kernel
`.data`, `.bss`, the whole of RAM after the image). The copy into SDRAM is mandatory.
Reading directly from flash is still useful for verification passes (CRC the image
in place without a buffer).

`0x90000000` falls in the ARMv8-M default memory map's `0x80000000-0x9FFFFFFF`
"Normal, write-through cacheable" region, so it is readable and D-cacheable with no
extra MPU work. **Caveat (inferred, not tested): because it is cacheable, a
`flash_write()` followed by a read through the window can return stale data. Invalidate
D-cache over the written range (`sys_cache_data_invd_range()`) after writing.**

### 2.3 Driver and Kconfig

Driver: `zephyr/drivers/flash/flash_renesas_ra_ospi_b.c` (785 lines) + `.h`.

Kconfig: `zephyr/drivers/flash/Kconfig.renesas_ra_ospi`

```
config FLASH_RENESAS_RA_OSPI_B
	bool "Renesas RA Octal-SPI driver"
	default y
	depends on DT_HAS_RENESAS_RA_OSPI_B_NOR_ENABLED
	select FLASH_HAS_DRIVER_ENABLED
	select FLASH_HAS_PAGE_LAYOUT
	select FLASH_HAS_EXPLICIT_ERASE
	select USE_RA_FSP_OSPI_B_NOR_FLASH
	select FLASH_JESD216
	select FLASH_HAS_EX_OP
	select PINCTRL
```

Because it is `default y` and the DT node is enabled, **the only symbol our app has
to set is `CONFIG_FLASH=y`.** Everything else is pulled in automatically. The board
defconfig (`ek_ra8d1_defconfig`) does not set `CONFIG_FLASH`, so we do need that one
line in `prj.conf`.

Confirmed present in the existing CircuitPython build
(`build-renesas_ek_ra8d1/zephyr-cp/zephyr/.config`):

```
CONFIG_FLASH=y
CONFIG_FLASH_RENESAS_RA_OSPI_B=y
CONFIG_USE_RA_FSP_OSPI_B_NOR_FLASH=y
CONFIG_FLASH_HAS_PAGE_LAYOUT=y
CONFIG_FLASH_JESD216=y
```

Full `flash_read()` / `flash_write()` / `flash_erase()` API is implemented
(`DEVICE_API(flash, flash_renesas_ra_ospi_b_api)` at line 641), plus
`page_layout`, `sfdp_read`, `read_jedec_id`, `ex_op`.

Init (`flash_renesas_ra_ospi_b_init`, line 658) brings the chip up in 1S-1S-1S,
then switches it to **SPI_FLASH_PROTOCOL_8D_8D_8D** (octal DDR). After
`POST_KERNEL` init at `CONFIG_FLASH_INIT_PRIORITY=50`, the `0x90000000` window is live.

### 2.4 Erase geometry and the reserved regions

From the DT `pages_layout` node:

| Offsets | Erase unit | Count |
|---|---|---|
| `0x000000` - `0x01FFFF` | 4 KiB | 32 |
| `0x020000` - `0x03FFFF` | 128 KiB | 1 |
| `0x040000` - `0x3FFFFFF` | 256 KiB | 255 |

Total 64 MiB. Two things in the low 256 KiB are already spoken for:

1. **`0x2000` - `0x2FFF` is the OSPI driver's auto-calibration sector.** At every
   init the driver compares that 4 KiB sector against a 16-byte preamble pattern and,
   if it does not match, **erases the sector and rewrites the pattern**
   (`flash_renesas_ra_ospi_b.c:106-131`, `.p_autocalibration_preamble_pattern_addr =
   APP_ADDRESS(SECTOR_THREE)`, `SECTOR_THREE = 2`, so offset `2 * 4096 = 0x2000`).
   Anything stored there is destroyed on boot.

2. **`0x000000` - `0x01FFFF` (first 128 KiB) is the CIRCUITPY filesystem.** See
   section 5 for why. Host-side evidence in `ra8d1-pack/docs/usb-msc-status.md`:
   the published MSC media is `127488` bytes, i.e. 128 KiB minus a couple of blocks.

Hence starting the image at `0x40000`. That also lands it exactly on the boundary
where uniform 256 KiB erase blocks begin, which makes erase-before-write trivial.

**Independent confirmation that `0x40000` is the right answer:** upstream Zephyr's
own `samples/drivers/spi_flash/src/main.c` hardcodes it for this board:

```c
#elif defined(CONFIG_BOARD_EK_RA8M1) || defined(CONFIG_BOARD_EK_RA8D1)
#define SPI_FLASH_TEST_REGION_OFFSET 0x40000
...
#define SPI_FLASH_SECTOR_SIZE 262144
```

The Renesas Zephyr wiki also lists `spi_flash` and `jesd216` as working samples on
EK-RA8D1 with "no additional connection" required, i.e. no DIP switch changes.

### 2.5 Proposed flash layout

```
0x90000000  offset 0x000000  128 KiB   CIRCUITPY FAT16 (leave alone; includes the
                                       driver autocal sector at 0x2000)
0x90020000  offset 0x020000  128 KiB   free (single 128K erase block; scratch)
0x90040000  offset 0x040000  256 B     image header
0x90040100  offset 0x040100  ~3.4 MB   kernel Image payload
            ...                        63.75 MB usable from 0x40000 up
```

Header (keep it in the same 256 KiB erase block as the payload so one erase covers both):

```c
struct ra8_image_hdr {
	uint32_t magic;      /* 'R8LX' */
	uint32_t version;
	uint32_t img_len;
	uint32_t img_crc32;
	uint8_t  pad[240];
};
```

No DTB in flash: `mini-rv32ima` already carries `default64mbdtb` compiled in
(`default64mbdtc.h`), and `mini-rv32ima.c:162-167` copies it to
`ram_amt - sizeof(default64mbdtb) - sizeof(struct MiniRV32IMAState)` and patches the
kernel command line at `+0xc0`. Just link it into the app.

### 2.6 Write throughput (ESTIMATED, not measured)

The driver programs in **64-byte** chunks (`PAGE_SIZE_BYTE 64` in
`flash_renesas_ra_ospi_b.h:25`), which is conservative for the S28HL512T. Timeouts
the driver allows: `TIME_WRITE 1000`, `TIME_ERASE_4K 1000`, `TIME_ERASE_256K 16000`
(ms). With datasheet-typical page-program and sector-erase times, writing 3.4 MB is
roughly 14 erases of 256 KiB (~7 s) plus ~54k page programs (~15 s), so on the order
of **20-30 seconds**. This is not the bottleneck; the host-to-board transfer is.

---

## 3. Path 2: SD card (DEAD - conflicts with SDRAM)

**SDRAM and SD are mutually exclusive on this board.** Do not pursue.

From `zephyr/boards/renesas/ek_ra8d1/doc/index.rst`:

| Use case | SW1-6 GLCD | SW1-7 SDRAM |
|---|---|---|
| SDHC channel 1 | OFF | **OFF** |
| MIPI Graphics Expansion (J58) | ON | **ON** |
| Parallel Graphics Expansion (J57) | ON | **ON** |

And from the EK-RA8D1 v1 User's Manual: *"The SDRAM can be isolated from the MCU bus
by turning SW1-7 off to allow the ports to be used for other purposes."*

So SW1-7 OFF physically disconnects the 512 Mbit IS42S16320F-6BLI from the bus.
SDHC1 requires SW1-7 OFF. SDRAM is non-negotiable for us. Path closed.

Worth recording, because it looks contradictory at first: **there is no direct pin
overlap** between the two in Zephyr's pinctrl. SDHC1 uses P400-P406 and P700
(`sdhc1_default`); SDRAM uses ports 1, 3, 6, 9, 10 (`sdram_default`). The conflict is
board-level (SW1-7 gates the SDRAM device onto the bus and frees those ports for
other headers), not an MCU pinmux collision. That does not help us: the switch still
has to be OFF, and OFF means no SDRAM.

Also note the SD slot is not on-board anyway. `ek_ra8d1.dts` has
`pmod_sd_shield: &sdhc1;`, i.e. it needs a PMOD SD shield. Even if the switches
cooperated we would be adding hardware and a FAT stack to do a job the OSPI flash
does with a memcpy.

---

## 4. Path 3: Embed in the app binary (DEAD - does not fit)

| | |
|---|---|
| Internal code flash, total | 2016 KiB = 2,064,384 B |
| CircuitPython build uses today | ~426 KiB (`text` 335,904 + `rodata` 78,968 + init/device/log areas) |
| A minimal Zephyr + mini-rv32ima would use | roughly 60-120 KiB (CP is not the baseline; it is 90% of that 426 KiB) |
| Best-case space for an image | ~1.9 MiB |
| Image we have | **3.32 MiB** |

Short by 1.4 MiB with a *maximally* generous budget. Not close.

Could a smaller image be built? The boot log
(`ra8d1-linux/notes/host-boot.log`) shows the kernel's own accounting:

```
Memory: 61384K/65532K available (1487K kernel code, 281K rwdata, 157K rodata,
                                 1464K init, 145K bss, 4148K reserved, ...)
```

1487K code + 281K rwdata + 157K rodata + 1464K init is already ~3.3 MB as loaded,
and that is *before* the buildroot initramfs. Stripping the rootfs and the init
sections could theoretically get a bare kernel near 1.9 MB, but it would leave zero
headroom, require a decompressor we would also have to fit, and give up the userspace
that makes the demo a demo. Not worth doing when 64 MB of OSPI is sitting on the same
board doing nothing.

---

## 5. Why CircuitPython does `/delete-node/ partitions;` on `&s28hl512t`

`ports/zephyr-cp/boards/ek_ra8d1.overlay`:

```dts
&s28hl512t {
    /delete-node/ partitions;
};
```

The board DTS declares a `fixed-partitions` node on the OSPI flash with a single
`nor` partition covering all 64 MB. CircuitPython's `supervisor_flash_init()`
(`ports/zephyr-cp/supervisor/flash.c:117-193`) has no `circuitpy_partition`, so it
falls into the dynamic path: it walks `flash_area_foreach()` and **skips any flash
device that is already covered by a flash area** ("covered by flash area", line 145).

With the stock 64 MB `nor` partition present, the OSPI device is covered, so
CircuitPython skips it and ends up with no filesystem at all. Deleting the partitions
node makes the device uncovered, CircuitPython claims it dynamically, and then takes
the **first uniform-page-size region**, which is `32 x 4 KiB = 128 KiB`
(the layout is non-uniform, so the loop at line 168 stops at index 32).

Generated device table for this board
(`build-renesas_ek_ra8d1/.../board/board.c:21`):

```c
const struct device* const flashes[] = { DEVICE_DT_GET(DT_NODELABEL(flash)),
                                         DEVICE_DT_GET(DT_NODELABEL(s28hl512t)) };
```

**Implication for us:** CIRCUITPY occupies OSPI `0x00000-0x1FFFF`. Putting our image
at `0x40000` means the two coexist, and reflashing CircuitPython later does not
require re-uploading the Linux image (and vice versa).

**Side note worth a bug report to the CP side:** the CIRCUITPY FAT region spans
offset `0x2000`, which is exactly the OSPI driver's auto-calibration sector. The
driver will erase that 4 KiB on any boot where the preamble does not match, i.e. it
can eat a piece of the filesystem. Not our problem today, but it explains any
mysterious CIRCUITPY corruption.

---

## 6. How do we WRITE the image into OSPI flash?

### 6.1 probe-rs: NOT possible out of the box

`probe-rs 0.32.0` is installed. Its RA8D1 target has no external flash region:

```
$ probe-rs chip info R7FA8D1BH
Cores (1):
    - main (Armv8m)
Generic: 0x00000000..0x00010000 (64.0 KiB)      ITCM
NVM:     0x02000000..0x021f8000 (2.0 MiB)       internal code flash
Generic: 0x20000000..0x20010000 (64.0 KiB)      DTCM
RAM:     0x22000000..0x220e0000 (896.0 KiB)     SRAM
Generic: 0x27000000..0x27003000 (12.0 KiB)      data flash
```

No `0x90000000` region and no OSPI flash algorithm. `probe-rs download` will refuse
to write there.

Making it work means adding a custom target YAML with an `Nvm` region at
`0x90000000` **plus a compiled ARM flash-algorithm blob** implementing
`Init`/`EraseSector`/`ProgramPage`/`UnInit` against the RA8 OSPI-B peripheral. The
tooling exists (`probe-rs/flash-algorithm-template`, `target-gen`), and it is the
right long-term answer if we end up reflashing images constantly, but it is a day of
work and it duplicates a driver we already have running on the target.

Also for the record: `ports/zephyr-cp/Makefile:45` uses `west flash`, and
`boards/renesas/ek_ra8d1/board.cmake` only registers the `jlink` and `pyocd` runners.
The CP flashing path is the on-board J-Link via west, with probe-rs used for probe
enumeration and RTT. `JLinkExe` directly is off-limits (it force-updates the probe
firmware).

### 6.2 UART loader app: RECOMMENDED

A small Zephyr app that reads a framed byte stream off `uart9` and `flash_write()`s
it. Everything it needs is already in the tree; the only new code is the framing and
a CRC.

Transfer time for 3,476,752 bytes, 8N1 (10 bits/byte on the wire):

| Baud | Effective B/s | Time | Notes |
|---|---|---|---|
| 115200 | 11,520 | **5 min 2 s** | line rate, zero protocol overhead |
| 115200 + XMODEM-1K | ~11,000 | ~5 min 30 s | stop-and-wait ACK per 1 KiB block |
| 921600 | 92,160 | **38 s** | SCI9 handles this fine; just change `current-speed` |

**Do not use the CircuitPython REPL for this.** The observed 9 KiB in ~90 s is
**100 B/s**, which is 0.87% of line rate. That is paste-mode overhead (per-line
Python exec, echo, base64), not the UART. Extrapolated honestly, 3.48 MB at 100 B/s
is **9.7 hours**. Prohibitive. A dedicated raw-binary loader is ~100x faster because
it removes the interpreter from the path entirely.

Recommendation: build the loader with `uart9` bumped to 921600 via a devicetree
overlay in our own app. ~40 s per image revision, and the image persists across
resets and app reflashes.

### 6.3 USB CDC-ACM: faster but riskier

The board's `usbhs` is high-speed and already works for the CircuitPython console
(`cdc_acm_console` in `ports/zephyr-cp/app.overlay`). Bulk CDC would move 3.5 MB in
seconds. But `ra8d1-pack/docs/usb-msc-status.md` documents a real defect in the
**RA8 USB HS bulk-IN data stage** (queued IN transfers never move on the wire). Our
loader is bulk-OUT dominant, which is a different path and may well be fine, but this
is not the thing to bet the first bring-up on. Keep it as an optimisation for later
if 40 s over UART turns out to be annoying.

---

## 7. First concrete implementation step

Extend the existing `ra8d1-linux/zephyr-app` (do not touch `ports/zephyr-cp/`) with a
**read-only OSPI probe**. It needs no board time to write, is safe to run when the
board frees up, and proves the whole read path before we commit to a loader.

1. `prj.conf`: add `CONFIG_FLASH=y`. (That is the only symbol needed; see 2.3.)
2. New `src/ospi.c`:
   - `DEVICE_DT_GET(DT_NODELABEL(s28hl512t))` + `device_is_ready()`
   - `flash_read_jedec_id()` and print it (expect an Infineon/Cypress S28HL512T ID)
   - `flash_get_page_count()` / `flash_get_page_info_by_idx()` for pages 0, 31, 32,
     33 and the last, to confirm the 4K / 128K / 256K layout at runtime
   - CRC32 the 4 KiB at offset `0x40000` **twice**: once via `flash_read()` and once
     via a direct `memcpy` from `(void *)0x90040000`. The two must match. That single
     comparison proves the memory-mapped window is live and that our whole
     "image lives in flash, boot copies it to SDRAM" plan holds.
3. Only then write the UART receiver + `flash_erase()`/`flash_write()` side.

Optional zero-code sanity check when the board is free: build and run upstream
`zephyr/samples/drivers/spi_flash` for `ek_ra8d1`. It already targets offset
`0x40000` with 256 KiB sectors and does erase / write / read-back. **It will erase
256 KiB at `0x40000`**, which is harmless today (nothing is there) but will destroy
our image once we have loaded one.

---

## 8. Verified vs inferred

**Verified in source in this tree:**
- Image size 3,476,752 B; internal flash 2016 KiB. Path 3 does not fit.
- `s28hl512t` node exists, `status = "okay"`, `reg = <0x90000000 DT_SIZE_M(64)>`.
- OSPI CS1 memory-mapped base is `0x90000000` (FSP `bsp_feature.h` for ra8d1).
- Zephyr's `flash_read()` for this device is a memcpy from that window.
- Full flash API implemented; `CONFIG_FLASH_RENESAS_RA_OSPI_B=y` already in the
  CircuitPython build's `.config`; only `CONFIG_FLASH=y` is required from the app.
- Driver rewrites offset `0x2000` (4 KiB) on init when the calibration pattern is absent.
- Erase layout 32x4K / 1x128K / 255x256K; upstream sample uses `0x40000` for this board.
- CircuitPython claims OSPI `0x00000-0x1FFFF` as CIRCUITPY, which is why the overlay
  deletes the `partitions` node.
- Zephyr board doc: SDHC1 needs SW1-7 OFF; MIPI/parallel graphics need SW1-7 ON.
- `probe-rs chip info R7FA8D1BH` has no region at `0x90000000`.

**From vendor docs (not this tree):**
- EK-RA8D1 UM: SW1-7 OFF isolates the SDRAM from the MCU bus. This is what makes the
  SD path genuinely exclusive with SDRAM rather than merely awkward.
- Renesas Zephyr wiki: `spi_flash` / `jesd216` samples run on EK-RA8D1 with no
  additional connections.

**Inferred / estimated, not measured:**
- OSPI program+erase time for 3.4 MB (~20-30 s). Datasheet-typical, not benchmarked.
- Cache-coherency caveat on the memory-mapped window after writes. Follows from the
  ARMv8-M default memory map making `0x80000000-0x9FFFFFFF` cacheable; not observed.
- USB CDC bulk-OUT being unaffected by the documented bulk-IN defect.
- UART times are arithmetic from baud rate, not measured transfers.

**Not verified because the board was in use:** nothing on this list was run on
hardware. Every hardware claim above rests on source in this tree plus the fact that
CircuitPython's CIRCUITPY filesystem demonstrably lives on this exact flash chip and
survives CRC-verified write / reset / re-read cycles (`ra8d1-pack/README.md`), which
is strong second-hand evidence that erase, write and memory-mapped read all work on
this board today.

---

## Reference card

| Thing | Value |
|---|---|
| Flash device node | `DT_NODELABEL(s28hl512t)` = `/soc/ospi@40268000/ospi-nor-flash@90000000` |
| Controller node | `DT_NODELABEL(ospi0)` = `/soc/ospi@40268000`, `renesas,ra-ospi-b` |
| Memory-mapped base (CS1) | `0x90000000`, 64 MB |
| Proposed image offset | `0x40000` -> address `0x90040000` |
| Guest RAM destination | `0x68000000` (`DT_NODELABEL(sdram1)`), 64 MB |
| Required Kconfig | `CONFIG_FLASH=y` (rest is `default y` off the DT node) |
| Auto-enabled | `CONFIG_FLASH_RENESAS_RA_OSPI_B`, `CONFIG_USE_RA_FSP_OSPI_B_NOR_FLASH`, `CONFIG_FLASH_HAS_PAGE_LAYOUT`, `CONFIG_FLASH_JESD216`, `CONFIG_PINCTRL` |
| Driver | `zephyr/drivers/flash/flash_renesas_ra_ospi_b.c` |
| Reserved: driver autocal | offset `0x2000` - `0x2FFF` |
| Reserved: CIRCUITPY | offset `0x0` - `0x1FFFF` |
| Erase unit at `0x40000`+ | 256 KiB |
| Write chunk in driver | 64 B |
