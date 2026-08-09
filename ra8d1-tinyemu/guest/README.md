# Trimmed guest kernel

A Linux 6.1.44 rv32 Sv32 kernel cut down to what `ra8d1-tinyemu`'s machine
layer actually provides, so it fits the board's 8 MB flash slot.

## The three variants, measured separately

Both changes were built and booted on their own so it is clear which did what.
The second column is the question that matters for flash; the fourth is the one
that matters for the guest, since it only has 64 MB.

| variant | Image | fits 8 MB slot? | RAM to guest | reserved | symbols |
|---|---:|---|---:|---:|---:|
| **A** stock guest kernel | 25,923,072 | no | 39,204K | 26,332K | 1178 |
| **B** A + `STRICT_KERNEL_RWX=n` | 10,727,424 | **no** | 54,044K | 11,492K | 1164 |
| **C** B + driver trim (shipped) | **6,543,776** | **yes**, 1.8 MB spare | **58,216K** | **7,320K** | 724 |

- Padding fix alone: **-15,195,648 B, 58.6% of the stock Image**, and +14,840K
  of guest RAM — for one config symbol and no features lost. But at 10.23 MB it
  is **still 2.3 MB over the slot**, so it does not remove the need for the trim.
- Driver trim on top: **-4,183,648 B**, a further 39% of what was left, plus
  another +4,172K of RAM.
- Together: 3.96x smaller and +19 MB to the guest.

The headline is that these are not alternatives. The padding fix is the cheaper
and larger of the two and should be done first by anyone repeating this, but on
this board it does not on its own reach a bootable configuration.

## Where most of it went, which was not where I expected

**57% of the original Image was zeros.** `CONFIG_STRICT_KERNEL_RWX` aligns
`.text`, `.init.text`, `.rodata` and `.data` to Sv32 4 MB megapage boundaries so
each can carry its own page permissions. On a kernel whose `.text` is 7.6 MB
that lands the sections at 0, 8 MB, 16 MB and 24 MB, and the Image is the span:

```
section       size        start        ends
.head.text    7,832       0xc0000000   0xc0001e98
.text         7,579,850   0xc0002000   0xc073c8ca
.init.text    230,402     0xc0800000   0xc0838402     <- 7.8 MB gap before this
.rodata       1,776,520   0xc1000000   0xc11b1b88     <- 6.2 MB gap before this
.data         658,656     0xc1800000   0xc18a0ce0

vmlinux sections total  11,067,213
Image on disk           25,923,072
padding                 14,855,859   (57%)
```

Turning that off is the single biggest lever and costs only kernel hardening,
which is not a property this guest is relying on. Config trimming did the rest:
the real content went from 11.07 MB to 6.78 MB, 39% off.

That ordering matters for anyone repeating this. Cutting 468 config symbols
without also turning off `STRICT_KERNEL_RWX` would have removed about 4 MB of
content and left a ~21 MB Image, still nowhere near the slot. Measured the other
way round as well — variant B above — the padding fix alone lands at 10.23 MB,
also short. Both are needed; neither is sufficient.

The padding also sits *inside* the reserved `_start.._end` span, which is why
removing it returns 14.8 MB of SDRAM to the guest and not just file bytes.

## What was cut

468 symbols removed, 14 added, 1178 -> 724. By area:

| area | symbols | why |
|---|---:|---|
| network drivers (`NET_VENDOR_*`) | 70 | no network device on this machine |
| DRM / framebuffer | 32 | no display |
| USB | 21 | no USB |
| WLAN / wireless / BT | 20+ | none of it exists |
| PCI | 18 | the devicetree has no PCI host |
| netfilter / IPv6 | 16 | nothing routes |
| NFS / 9p / SUNRPC | 12 | rootfs is a local block device |
| RTC | 12 | time comes from the SBI timer |
| debug (`DEBUG_VM`, `DEBUG_VM_PGTABLE`, `SCHED_DEBUG`, ...) | 12 | not a debug build |
| MMC / SCSI / ATA | 18 | the only block device is virtio |
| EFI | 8 | there is no firmware |
| KVM | 4 | this guest does not nest |
| misc (input, hwmon, thermal, watchdog, regulator, IIO, media, PWM, SPI, ...) | rest | no such hardware |

The rule was the machine layer's own devicetree: 8250 at `0x10000000`, CLINT at
`0x11000000`, syscon at `0x11100000`, paravirt I/O at `0x11200000`, virtio-mmio
at `0x10001000`, PLIC at `0x40100000`, RAM at `0x80000000`. Anything that cannot
be reached from those was a candidate.

## What was kept, and verified after `olddefconfig` rather than assumed

`olddefconfig` can silently drop a symbol whose dependency was just removed, so
every requirement is asserted and then re-read from the resulting `.config`:

| symbol | why |
|---|---|
| `VIRTIO`, `VIRTIO_MMIO`, `VIRTIO_BLK` | the rootfs block device |
| `EXT4_FS`, `EXT4_USE_FOR_EXT2` | the rootfs |
| `EROFS_FS` | added; ext2 stays the default, EROFS is now available |
| `I2C`, `I2C_CHARDEV` | Blinka |
| `GPIOLIB`, `GPIO_CDEV`, `GPIO_SYSFS` | Blinka — `GPIO_SYSFS` is the one people miss |
| `SERIAL_8250`, `SERIAL_8250_CONSOLE` | the console |
| `FPU` | the userspace is ilp32d hard float |
| `MODULES` | `guest-runtime` builds `pv-io.c` as a module |
| `BLK_DEV_INITRD` | cheap, and a fallback boot path |
| `VIRTIO_NET` | added 2026-08-09 for ssh; see below |

## `VIRTIO_NET` added, 2026-08-09

The trim originally dropped `VIRTIO_NET` on the grounds that the machine has no
network device. It is back, because `08-guest-net-mmu.md` settles on virtio-net
as the guest transport and dropbear needs an address to bind (`notes/ssh.md`).

| | Image |
|---|---:|
| trim as shipped 2026-08-08 | 6,543,840 |
| + `CONFIG_VIRTIO_NET=y` | **6,577,272** |
| delta | **+33,432 B** |

Still 1.73 MB under the 8 MB slot. `olddefconfig` changed nothing else -
`NET_FAILOVER` and `DIMLIB`, which `VIRTIO_NET` selects, were already `y`, and
`PACKET`/`INET`/`IP_PNP_DHCP` were never trimmed. Booted under QEMU with
`-device virtio-net-device`: `eth0` binds `virtio_net`, `udhcpc` takes the
10.0.2.15 lease, ping to the gateway is 1.2-2.0 ms.

## Reproducing

`trim-config.sh` is the whole recipe. It builds **out of tree in
`/br/mmu-trim`** and never writes to `/br/mmu`, `/br/mmu-boot`, `/br/mmu-pv` or
`/br/mmu-kernel-fragment`, which another agent owns.

```sh
docker exec br sh /tmp/trim-setup.sh    # extract pristine 6.1.44 into /br/mmu-trim
docker exec br sh /tmp/trim-config.sh   # base config + the trim, then olddefconfig
docker exec br sh -c 'cd /br/mmu-trim/linux-6.1.44 && \
    ARCH=riscv CROSS_COMPILE=/br/mmu/host/bin/riscv32-linux- make -j8 Image'
```

This is a kernel-only build against the existing Buildroot toolchain, not a
Buildroot rebuild: minutes instead of the 45 that recompiling a glibc toolchain
would cost, and it cannot disturb anyone else's tree.

`linux-6.1.44-trim.config` is the resulting config, kept so the build is
reproducible without rerunning the script.

## Verified

Booted under `host/tinyemu-host` with `rootfs.ext2` on virtio-blk:

```
Memory: 58216K/65536K available
virtio_blk virtio0: [vda] 122880 512-byte logical blocks (62.9 MB/60.0 MiB)
EXT4-fs (vda): mounting ext2 file system using the ext4 subsystem
VFS: Mounted root (ext2 filesystem) readonly on device 254:0.
Freeing unused kernel image (initmem) memory: 184K

buildroot login: root
# python3 -c "import ctypes,sys;print(sys.version.split()[0]);print(ctypes.CDLL(None))"
3.11.6
<CDLL 'None', handle 9473bad0 at 0x940bbdd0>
# ls /sys/class/gpio
export    unexport
```

`/sys/class/gpio` present confirms `GPIO_SYSFS` survived. `/dev/i2c-*` is
absent, which is correct and not a regression: `I2C_CHARDEV` only creates a
node when an adapter registers, and this kernel has no I2C adapter until
`pv-io.c` is built into it.

The header declares `text_offset = 0x400000`, so the app's M-mode guard
(`notes/00-port.md` §8a) accepts it, unlike the nommu image.

**Not yet booted on the board.** It now fits the slot, which is what was
blocking that.
