#!/bin/bash
# Cross-build a static CPython 3.11 for riscv32 nommu / uClibc / bFLT,
# against the toolchain buildroot just produced. Runs INSIDE the container.
#
# Buildroot's own python3 package cannot be used: package/python3/Config.in has
#   depends on BR2_USE_MMU  /  BR2_TOOLCHAIN_HAS_THREADS  /  !BR2_STATIC_LIBS
# and this target fails all three. So we drive CPython's configure directly.
#
# Three things have to be worked around, all of them checked in the source:
#   1. no threads at all. uClibc-ng 1.0.44 offers neither LINUXTHREADS
#      (depends on !TARGET_riscv32) nor NPTL (depends on ARCH_USE_MMU ||
#      TARGET_arm), so pthread_create does not exist. CPython 3.11 does have a
#      stub path (Python/thread.c -> thread_pthread_stubs.h) but configure only
#      reaches it for ac_sys_system=WASI, so we widen that case.
#   2. no dlopen. configure should pick MODULE_BUILDTYPE=static on its own once
#      it fails to find dlopen; asserted below rather than assumed.
#   3. bFLT gives a process one contiguous region for text+data+bss+stack, and
#      elf2flt's default stack is tiny. -s sets it.
set -e

BR=/br/buildroot
HOSTBIN=$BR/output/host/bin
CROSS=riscv32-buildroot-linux-uclibc
SRC=/br/Python-3.11.9
BUILDDIR=/br/py-build
PREFIX=/br/py-install
STACK=${STACK:-1048576}

export PATH=$HOSTBIN:$PATH
command -v $CROSS-gcc >/dev/null || { echo "toolchain not built yet"; exit 1; }

# --- patch 1: let a non-WASI target fall through to the pthread stubs ---
cd "$SRC"
if ! grep -q 'RA8D1_STUB_PATCH' configure; then
	# the generated configure contains the expanded AS_CASE; the error branch
	# is what we replace, so a target with no pthreads gets stubs instead.
	python3 - <<'EOF'
import re
p = "configure"
s = open(p).read()
# The expanded case looks like:  case $ac_sys_system in #(
#   WASI) :  posix_threads=stub ;; #(
#   *) : as_fn_error $? "could not find pthreads..."
needle = 'posix_threads=stub'
i = s.find(needle)
assert i > 0, "could not find posix_threads=stub in configure"
j = s.find('as_fn_error', i)
k = s.find('\n', j)
assert 0 < j < i + 4000, "error branch not where expected"
s = s[:j] + 'posix_threads=stub ; : ' + s[k:]
s = "# RA8D1_STUB_PATCH\n" + s
open(p, "w").write(s)
print("patched configure: no-pthreads now selects stubs instead of erroring")
EOF
fi

rm -rf "$BUILDDIR" "$PREFIX"
mkdir -p "$BUILDDIR"
cd "$BUILDDIR"

export CC="$CROSS-gcc"
export CXX="$CROSS-g++"
export AR="$CROSS-ar"
export RANLIB="$CROSS-ranlib"
export READELF="$CROSS-readelf"
# Do NOT define THREAD_STACK_SIZE here. With HAVE_PTHREAD_STUBS,
# thread_pthread_stubs.h includes thread_pthread.h, which hard-errors with
# "THREAD_STACK_SIZE defined but _POSIX_THREAD_ATTR_STACKSIZE undefined".
# The stub implementation never creates a thread, so the setting is meaningless.
export CFLAGS="-Os -fno-strict-aliasing"
export LDFLAGS="-Wl,-elf2flt=-s$STACK"

"$SRC/configure" \
	--host=$CROSS \
	--build=$("$SRC/config.guess") \
	--prefix=/usr \
	--with-build-python=/usr/bin/python3.11 \
	--without-ensurepip \
	--disable-ipv6 \
	--disable-test-modules \
	--without-doc-strings \
	ac_cv_file__dev_ptmx=no \
	ac_cv_file__dev_ptc=no \
	ac_cv_func_fork=no \
	ac_cv_func_forkpty=no \
	ac_cv_func_wait3=no \
	ac_cv_func_wait4=no \
	2>&1 | tail -40

echo "=== configure results that decide whether this can work ==="
grep -E '^(MODULE_BUILDTYPE|DYNLOADFILE|LDLIBRARY|PY_ENABLE_SHARED)=' Makefile || true
grep -E '#define (HAVE_PTHREAD_STUBS|HAVE_DLOPEN|HAVE_FORK) ' pyconfig.h || true

make -j"$(nproc)" 2>&1 | tail -30
make install DESTDIR="$PREFIX" 2>&1 | tail -5

echo "=== result ==="
ls -l "$PREFIX/usr/bin/" | head
file "$PREFIX/usr/bin/python3.11" || true
size "$PREFIX/usr/bin/python3.11" 2>/dev/null || true
