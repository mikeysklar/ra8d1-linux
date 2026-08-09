# Custom rv32 nommu guest image: buildroot, telnetd, networking, Python

Date: 2026-08-07
Goal: replace `image/Image` (cnlohr's prebuilt 3,476,752 B kernel+initramfs) with
one we control, containing BusyBox `telnetd`, a TCP/IP stack with at least one
network device, and Python 3.

Everything below was run. Where something is quoted from a config or a source
file, the file and line are given so it can be re-checked.

**Status: IN PROGRESS — results sections are marked as such and will be filled
in once the build completes. The analysis sections are final.**

---

## 0. Build environment: Buildroot cannot run on this Mac

Buildroot requires Linux and a case-sensitive filesystem. This host is neither:

```
$ touch /tmp/CaseTest.txt && ls /tmp/casetest.txt
/tmp/casetest.txt              # case-insensitive APFS
$ which docker podman colima lima qemu-system-riscv32
(all: not found)
```

So the first real work was standing up a Linux build environment:

```sh
brew install colima docker
colima start --cpu 6 --memory 5 --disk 60 --vm-type vz --mount-type virtiofs
docker volume create br
docker run -d --name br -v br:/br debian:12 sleep infinity
```

Note the memory figure. The first attempt used `--memory 10` and Virtualization.
framework rejected it:

```
Error Domain=VZErrorDomain Code=2 "memorySize is greater than maximumAllowedMemorySize"
```

`hw.memsize` on this Mac is **8 GB**, so the VM gets 5 GB and 6 of 8 cores. That
is enough for buildroot but it is the reason builds here are not fast.

The build tree lives on a docker volume inside the VM (`/br`), not on a
virtiofs mount of the Mac filesystem — both for speed and because the
case-insensitivity would break the kernel source tree.

---

## 1. Starting point: cnlohr's configs were already on disk

`emulator/configs/` (from the `cnlohr/mini-rv32ima` clone) contains the exact
known-good set, which is a much better starting point than a defconfig:

| file | what it is |
| --- | --- |
| `buildroot_config` | 287 set options; rv32ima, nommu, uClibc, bFLT, initramfs |
| `custom_kernel_config` | Linux 6.8-rc1 config for `riscv-minimal-nommu` |
| `busybox_config` | the userland |
| `uclibc_config` | libc config |

and `emulator/Makefile` shows how they are meant to be used: cloned against
**`cnlohr/buildroot`** (a fork of buildroot 2023.11-git carrying regymm's nommu
patches), not upstream buildroot.

The shape this produces is exactly the shape we need and is worth stating
explicitly, because it is what makes the target-side load a single memcpy:

```
BR2_TARGET_ROOTFS_INITRAMFS=y      # rootfs.cpio embedded into the kernel
BR2_LINUX_KERNEL_IMAGE=y           # output is one flat `Image`
BR2_BINFMT_FLAT=y                  # bFLT userland, no ELF loader, no MMU
BR2_TARGET_ROOTFS_CPIO_NONE=y      # no separate rootfs artifact
```

Reproduce with:

```sh
git clone https://github.com/cnlohr/buildroot --recurse-submodules --depth 1
# then image/buildroot/setup.sh, which stages the configs and layers our
# fragments on top
```

---

## 2. What we changed, and why

Rather than editing cnlohr's configs in place, the changes are kept as
fragments and an overlay so the delta is legible. All of this lives in
`image/buildroot/`:

```
image/buildroot/
  setup.sh                          stages configs + fragments, runs olddefconfig
  build-cpython.sh                  the CPython cross-build attempt (see §5)
  configs/kernel_fragment           networking, both transports
  configs/busybox_fragment          telnetd and net utilities
  overlay/etc/inittab               adds network bring-up to cnlohr's inittab
  overlay/etc/init.d/rc.net         brings up interfaces, starts telnetd
  overlay/etc/fstab                 unchanged from cnlohr's
  patches/linux/0001-raise-max-page-order-for-nommu.patch
```

### 2a. BusyBox: telnetd

`CONFIG_TELNETD` and `CONFIG_FEATURE_TELNETD_STANDALONE`, plus `telnet`,
`ifconfig`, `route`, `ip`, `ping`, `netstat`, `arp`, `nc`, `slattach`,
`udhcpc`, and `vi`.

telnetd rather than dropbear was the right call for a second reason beyond
size: **it is nommu-safe by construction**. `networking/telnetd.c` spawns the
login shell with

```c
pid = vfork(); /* NOMMU-friendly */
```

and multiplexes all sessions in a single process with `poll()`, so it never
needs `fork()` — which does not exist on nommu. It does need ptys, and both
sides are already present: `CONFIG_UNIX98_PTYS=y` in the kernel config,
`UCLIBC_HAS_PTY=y` / `UCLIBC_HAS_GETPT=y` in the uClibc config, and `devpts` in
the fstab.

### 2b. Kernel: networking, both candidate transports

The base config has `# CONFIG_NET is not set` — networking is entirely absent,
not merely lacking a driver. Added `NET`, `PACKET`, `UNIX`, `INET`, `IP_PNP`,
and, as asked, **both** transports:

- **virtio-net**: `CONFIG_VIRTIO_NET=y`. `CONFIG_VIRTIO_MMIO=y` and
  `CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES=y` were *already* on in cnlohr's config.
  That second one is worth flagging to whoever designs the transport: it means
  a virtio device can be declared on the kernel command line
  (`virtio_mmio.device=<size>@<addr>:<irq>`) instead of by adding a DT node, so
  the emulator's compiled-in DTB may not need to change at all.
- **SLIP**: `CONFIG_SLIP`, `CONFIG_SLHC`, `CONFIG_SLIP_COMPRESSED`,
  `CONFIG_SLIP_SMART`. Also bumped `CONFIG_SERIAL_8250_NR_UARTS` to 2 so a
  second UART can exist. Note this is inert on its own: the emulator's DTB
  (`default64mbdtc.h`) declares only `uart@10000000`, so a second port needs a
  DT node added emulator-side.
- `CONFIG_TUN=y` as a cheap third option. IPv6, netfilter, bridging, wireless,
  and the ethernet driver zoo are all off.

### 2c. Kernel: the contiguous-allocation ceiling

This is the interesting one, and it is the lever on the 11 MB cliff measured in
`01-emulator-and-image.md` §7.

On nommu, every process needs one physically **contiguous** block for
text+data+bss+stack, so the buddy allocator's largest possible block is a hard
ceiling on binary size. cnlohr's README says as much:

> You *MUST* build your kernel with `MAX_ORDER` set to >12 in
> `buildroot/output/build/linux-5.19/include/linux/mmzone.h` if you are
> building your own image.

The obvious move is `CONFIG_ARCH_FORCE_MAX_ORDER=13` in a config fragment.
**That does not work, and fails silently.** Only some architectures define that
symbol in Kconfig; checked against v6.8-rc1, arm64/arm/powerpc do and
**riscv does not**:

```
$ curl -s .../v6.8-rc1/arch/riscv/Kconfig | grep ARCH_FORCE_MAX_ORDER
(no output)
```

With no Kconfig symbol, `olddefconfig` drops the line and the resulting kernel
still has the default. The limit therefore has to be changed at the source, in
the fallback in `include/linux/mmzone.h` (renamed `MAX_ORDER` ->
`MAX_PAGE_ORDER` in 6.8):

```c
 #ifndef CONFIG_ARCH_FORCE_MAX_ORDER
-#define MAX_PAGE_ORDER 10        /* 2^10 pages = 4 MiB max block */
+#define MAX_PAGE_ORDER 13        /* 2^13 pages = 32 MiB max block */
 #else
```

carried as `patches/linux/0001-raise-max-page-order-for-nommu.patch`.

### 2d. uClibc: wchar and locale turned on

`BR2_TOOLCHAIN_BUILDROOT_WCHAR=y` and `BR2_TOOLCHAIN_BUILDROOT_LOCALE=y`.
Nothing in the BusyBox userland needs these; CPython does, and turning them on
later would mean rebuilding the entire toolchain. They were enabled before the
toolchain was built so the Python attempt costs no extra build.
`BR2_ENABLE_LOCALE_PURGE=y` with whitelist `"C en_US"` was already set in
cnlohr's config, which keeps the generated locale data small.

---

## 3. Python 3 on rv32 nommu: the honest assessment

*(This section is final. It is the answer to "flag risk early".)*

The risk is worse than "Python is a big binary". **Buildroot's `python3` package
cannot be selected for this target at all**, and neither can `micropython`.
From `package/python3/Config.in` in the actual tree:

```
config BR2_PACKAGE_PYTHON3
	depends on BR2_USE_WCHAR
	# uses fork()
	depends on BR2_USE_MMU
	depends on BR2_TOOLCHAIN_HAS_THREADS   # libffi
	depends on !BR2_STATIC_LIBS
```

and `package/micropython/Config.in`:

```
config BR2_PACKAGE_MICROPYTHON
	depends on BR2_TOOLCHAIN_HAS_THREADS
	depends on !BR2_STATIC_LIBS
```

Our config has `BR2_STATIC_LIBS=y`, and that is **not a preference** — rv32
nommu has no shared-library support at all. Nor is `HAS_NO_THREADS=y` in
cnlohr's uClibc config a preference. Both fall out of uClibc-ng 1.0.44's own
Kconfig, where the entire threading *choice* is empty for this target:

```
config UCLIBC_HAS_LINUXTHREADS
	depends on !TARGET_aarch64 && !TARGET_riscv64 && \
		   !TARGET_riscv32 && !TARGET_metag      # <-- excluded by name

config UCLIBC_HAS_THREADS_NATIVE                     # NPTL
	depends on ... && (ARCH_USE_MMU || TARGET_arm)   # <-- we are nommu, not arm
```

and correspondingly `libpthread/nptl/sysdeps/` contains a `riscv64` directory
and **no riscv32 one**. So on riscv32 nommu, uClibc-ng offers *no* threading
implementation whatsoever; `HAS_NO_THREADS` is the only remaining option in the
choice. `pthread_create` does not exist.

CPython 3.7+ removed `--without-threads` and hard-errors without a threading
implementation (`Python/thread.c`):

```c
#else
#   error "Require native threads. See https://bugs.python.org/issue31370"
#endif
```

**There is one escape**, and it is why this is worth attempting rather than
declaring dead: CPython 3.11 added a stub pthread implementation for WASI.
`Python/thread.c` selects `thread_pthread_stubs.h` when `HAVE_PTHREAD_STUBS` is
defined, and `configure.ac` defines it when `posix_threads=stub` — which is
currently reachable only via `AS_CASE([$ac_sys_system], [WASI], ...)`. Widening
that case is a one-line patch.

So the plan is a hand-rolled static CPython cross-build
(`image/buildroot/build-cpython.sh`), not a package. The remaining risks, in
order of how likely they are to kill it:

1. **Contiguity.** Under bFLT the whole binary is one allocation. A static
   CPython is several MB. This is what the MAX_PAGE_ORDER patch is aimed at.
2. **Stack.** bFLT fixes the stack size at link time and elf2flt's default is
   tiny; passed `-Wl,-elf2flt=-s1048576`. That stack is part of the same
   contiguous allocation.
3. **No `fork()`.** Does not affect interpreter startup, but `os.fork`,
   `subprocess` and `multiprocessing` will not work.
4. **Everything static.** No dlopen means every extension module must be built
   into the binary; CPython does this automatically once configure fails to
   find dlopen.

**RESULT: TBD — see §5.**

---

## 4. Does it build and boot?

**TBD.**

---

## 5. Python attempt

**TBD.**

---

## 6. RAM floor

**TBD.**

---

## 7. Verification method

`notes/host-boot-driver2.py`, a generalisation of the earlier
`host-boot-driver.py`. A pty is still mandatory (the emulator's `IsKBHit()`
latches EOF on a read-only pipe). Two things it does that the original did not:

- handles both a getty `login:` prompt (stock image) and the auto-login the
  image we build uses;
- brackets every command with `echo MARKnn_rc=$?`, so "the command produced no
  output" is distinguishable from "the command never ran". This matters
  because of the 10 MB false pass documented in `01-emulator-and-image.md`:
  the stock image prints a prompt and *then* fails to exec anything. Checking
  for a prompt alone reports success there.

Validated against the stock image before use, which also gives the baseline:

```
=== RAM 0x4000000 (67108864 bytes) | image image/Image (3476752 bytes) ===
  boot to shell : 0.83s
  uname -a                           rc=0
  busybox telnetd --help             rc=127     <- telnetd: applet not found
  python3 --version                  rc=127     <- No such file or directory
```
