#!/bin/bash
# Runs INSIDE the Linux container. Stages cnlohr's known-good mini-rv32ima
# buildroot configs, layers our fragments on top, and configures the tree.
# Does not start the build -- that is a separate step so it can be watched.
set -e

BR=/br/buildroot
SRC=/br/src            # cnlohr's configs, copied in from the host

cd "$BR"

cp -a "$SRC/custom_kernel_config"  "$BR/kernel_config"
cp -a "$SRC/buildroot_config"      "$BR/.config"
cp -a "$SRC/busybox_config"        "$BR/busybox_config"
cp -a "$SRC/uclibc_config"         "$BR/uclibc_config"
cp -a "$SRC/uclibc_config"         "$BR/uclibc_config_extra"

cp -a "$SRC/kernel_fragment"       "$BR/kernel_fragment"
cp -a "$SRC/busybox_fragment"      "$BR/busybox_fragment"

# Point the config at our fragments and overlay, and drop the qemu post-image
# script (it regenerates a qemu-specific readme we do not want or need).
sed -i \
  -e 's|^BR2_LINUX_KERNEL_CONFIG_FRAGMENT_FILES=.*|BR2_LINUX_KERNEL_CONFIG_FRAGMENT_FILES="kernel_fragment"|' \
  -e 's|^BR2_PACKAGE_BUSYBOX_CONFIG_FRAGMENT_FILES=.*|BR2_PACKAGE_BUSYBOX_CONFIG_FRAGMENT_FILES="busybox_fragment"|' \
  -e 's|^BR2_ROOTFS_OVERLAY=.*|BR2_ROOTFS_OVERLAY="/br/overlay"|' \
  -e 's|^BR2_ROOTFS_POST_IMAGE_SCRIPT=.*|BR2_ROOTFS_POST_IMAGE_SCRIPT=""|' \
  "$BR/.config"

# wchar and locale in uClibc. Nothing we ship *needs* these, but CPython does,
# and turning them on later would mean rebuilding the whole toolchain. They are
# enabled here so the Python attempt costs no extra toolchain build.
# BR2_ENABLE_LOCALE_PURGE / _WHITELIST are already "C en_US" in cnlohr's config,
# which keeps the generated locale data small.
sed -i \
  -e 's|^# BR2_TOOLCHAIN_BUILDROOT_WCHAR is not set|BR2_TOOLCHAIN_BUILDROOT_WCHAR=y|' \
  -e 's|^# BR2_TOOLCHAIN_BUILDROOT_LOCALE is not set|BR2_TOOLCHAIN_BUILDROOT_LOCALE=y|' \
  "$BR/.config"

make olddefconfig

echo "--- resulting settings of interest ---"
grep -E '^BR2_(LINUX_KERNEL_CONFIG_FRAGMENT_FILES|PACKAGE_BUSYBOX_CONFIG_FRAGMENT_FILES|ROOTFS_OVERLAY|ROOTFS_POST_IMAGE_SCRIPT|LINUX_KERNEL_VERSION|TOOLCHAIN_USES_UCLIBC|STATIC_LIBS|BINFMT_FLAT)=' "$BR/.config" || true
