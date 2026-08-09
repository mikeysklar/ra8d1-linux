set -e
K=/br/mmu-trim/linux-6.1.44
cd $K
cp /br/mmu-trim/config.base .config

# olddefconfig probes the compiler, so it MUST see the cross toolchain. Run it
# with the host gcc and it silently rewrites a dozen unrelated symbols to match
# Debian's gcc instead of Buildroot's 12.3.0 - CC_VERSION_TEXT, GCC_VERSION,
# TOOLCHAIN_HAS_ZICBOM/RISCV_ISA_ZICBOM, RISCV_DMA_NONCOHERENT and the whole
# ARCH_HAS_SYNC_DMA_* block, CC_HAVE_STACKPROTECTOR_TLS, GCC_PLUGINS. You get a
# different kernel with no error message. Measured 2026-08-09.
export PATH=/br/mmu/host/bin:$PATH
export ARCH=riscv CROSS_COMPILE=riscv32-linux-

d() { for s in "$@"; do ./scripts/config --file .config -d "$s"; done; }
e() { for s in "$@"; do ./scripts/config --file .config -e "$s"; done; }

# ---- 1. the padding. 57% of the Image, and the single biggest lever.
# STRICT_KERNEL_RWX aligns .text/.init/.rodata/.data to Sv32 4 MB megapage
# boundaries so each can get its own permissions. That costs 14.9 MB of zeros.
d STRICT_KERNEL_RWX STRICT_MODULE_RWX

# ---- 2. debug and instrumentation
d DEBUG_KERNEL DEBUG_VM DEBUG_VM_PGTABLE DEBUG_LIST DEBUG_MISC SCHED_DEBUG \
  FRAME_POINTER STACKTRACE SLUB_DEBUG DEBUG_FS DEBUG_MEMORY_INIT \
  DEBUG_SPINLOCK DEBUG_MUTEXES DEBUG_ATOMIC_SLEEP FTRACE KPROBES \
  RUNTIME_TESTING_MENU CRASH_DUMP KEXEC PROC_KCORE PROC_VMCORE \
  DETECT_HUNG_TASK WQ_WATCHDOG SOFTLOCKUP_DETECTOR

# ---- 3. buses and device classes this machine does not have.
# The devicetree lists exactly: 8250, CLINT, syscon, paravirt, PLIC,
# virtio-mmio, RAM. Nothing below is reachable.
d PCI PCI_HOST_GENERIC USB_SUPPORT SCSI ATA MMC MTD NVME_CORE \
  DRM FB DRM_VIRTIO_GPU VIRTIO_CONSOLE VIRTIO_INPUT VIRTIO_BALLOON \
  SOUND SND HID HID_SUPPORT INPUT VT VGA_CONSOLE DUMMY_CONSOLE \
  HWMON THERMAL WATCHDOG POWER_SUPPLY REGULATOR IIO MEDIA_SUPPORT \
  RTC_CLASS PTP_1588_CLOCK DMADEVICES PPS LEDS_CLASS CPU_FREQ EFI \
  MAILBOX RESET_CONTROLLER PWM SPI W1 NVMEM

# ---- 4. networking: keep the core (AF_UNIX, AF_PACKET, and a socket() that
# works) plus virtio-net (§7 below), drop every physical-NIC driver and every
# exotic protocol. The only network device this machine can ever have is the
# virtio-mmio one; ETHERNET here is the vendor-driver menu, not the L2 stack,
# so dropping it does not affect virtio_net.
d IPV6 NETFILTER WIRELESS WLAN BT ETHERNET NET_VENDOR_INTEL PHYLIB MDIO_DEVICE \
  NET_9P 9P_FS NFS_FS NFSD SUNRPC BRIDGE VLAN_8021Q NET_SCHED XFRM_USER \
  INET_ESP INET_AH IP_MULTICAST TCP_CONG_ADVANCED

# ---- 5. filesystems: ext2/ext4 for the rootfs, tmpfs/proc/sysfs for a live
# system, EROFS added as the lead asked so it rides this rebuild.
d BTRFS_FS XFS_FS F2FS_FS JFS_FS REISERFS_FS NTFS_FS UBIFS_FS JFFS2_FS \
  FUSE_FS CIFS ISO9660_FS UDF_FS FAT_FS MSDOS_FS VFAT_FS SQUASHFS \
  QUOTA ROMFS_FS CRAMFS
e EROFS_FS

# ---- 6. crypto: nothing here needs it. Anything a kept subsystem depends on
# comes back through olddefconfig.
d CRYPTO_AES CRYPTO_USER_API CRYPTO_HW KVM VIRTUALIZATION

# ---- 7. everything the port and Blinka require, asserted rather than assumed
e VIRTIO VIRTIO_MENU VIRTIO_MMIO VIRTIO_BLK BLK_DEV
e VIRTIO_NET                     # +33,432 B of Image; ssh needs it (notes/ssh.md)
e EXT4_FS EXT4_USE_FOR_EXT2 TMPFS PROC_FS SYSFS DEVTMPFS DEVTMPFS_MOUNT
e I2C I2C_CHARDEV GPIOLIB GPIO_CDEV GPIO_SYSFS
e SERIAL_8250 SERIAL_8250_CONSOLE SERIAL_OF_PLATFORM SERIAL_EARLYCON
e RISCV_SBI_V01 SERIAL_EARLYCON_RISCV_SBI HVC_RISCV_SBI
e MODULES MODULE_UNLOAD          # guest-runtime is building pv-io.c as a module
e BLK_DEV_INITRD                 # cheap, and a fallback boot path
e FPU                            # hard float: the userspace is ilp32d

make ARCH=riscv CROSS_COMPILE=riscv32-linux- olddefconfig >/dev/null
echo "trimmed symbols: $(grep -c '^CONFIG_' .config)"
echo "--- required symbols after olddefconfig ---"
for s in VIRTIO_MMIO VIRTIO_BLK VIRTIO_NET EXT4_FS EXT4_USE_FOR_EXT2 EROFS_FS I2C_CHARDEV \
         GPIO_CDEV GPIO_SYSFS SERIAL_8250_CONSOLE FPU MODULES STRICT_KERNEL_RWX; do
  printf "%-26s %s\n" "$s" "$(grep -E "^CONFIG_$s=|^# CONFIG_$s " .config || echo MISSING)"
done
