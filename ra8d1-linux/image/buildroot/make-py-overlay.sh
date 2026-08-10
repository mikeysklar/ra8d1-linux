#!/bin/bash
# Turn the 116 MB `make install` tree into something that can live in an
# initramfs. Runs INSIDE the container. Produces /br/pyoverlay.
#
# Two things do the work:
#  * drop whole packages we cannot use or do not need. ctypes and anything
#    built on it are pointless here (no dlopen); subprocess/multiprocessing
#    are pointless too (no fork on nommu).
#  * ship *sourceless* .pyc only. `make install` leaves both foo.py and
#    __pycache__/foo.cpython-311.pyc, and CPython ignores a cached .pyc whose
#    source is missing -- but it will import a .pyc sitting directly at the
#    module path. So each __pycache__/x.cpython-311.pyc is moved to x.pyc and
#    the .py is deleted. This is the same trick buildroot's PYC_ONLY uses.
set -e

SRC=/br/py-install/usr
DST=/br/pyoverlay
V=3.11

rm -rf "$DST"
mkdir -p "$DST/usr/bin" "$DST/usr/lib"

cp "$SRC/bin/python$V" "$DST/usr/bin/python$V"
ln -sf "python$V" "$DST/usr/bin/python3"
ln -sf "python3"   "$DST/usr/bin/python"

cp -a "$SRC/lib/python$V" "$DST/usr/lib/python$V"
L="$DST/usr/lib/python$V"

# 56 MB of static libpython + object files, and the whole GUI/packaging world
rm -rf "$L"/config-$V-* "$L"/idlelib "$L"/tkinter "$L"/turtledemo "$L"/turtle.py
rm -rf "$L"/lib2to3 "$L"/ensurepip "$L"/pydoc_data "$L"/distutils "$L"/venv
rm -rf "$L"/test "$L"/unittest "$L"/doctest.py "$L"/site-packages
rm -rf "$L"/asyncio "$L"/email "$L"/xml "$L"/xmlrpc "$L"/http "$L"/urllib
rm -rf "$L"/wsgiref "$L"/html "$L"/sqlite3 "$L"/dbm "$L"/curses "$L"/zoneinfo
rm -rf "$L"/lib-dynload "$L"/__phello__ "$L"/ctypes "$L"/concurrent
# no fork() on nommu, so these can only ever raise
rm -rf "$L"/multiprocessing "$L"/subprocess.py

# encodings is the one package CPython must import at startup (everything else
# it needs -- os, site, codecs, io, abc, stat, importlib -- is deep-frozen into
# the binary). The full set is 4 MB of codecs nobody here will use.
find "$L/encodings" -name '*.py' \
	! -name '__init__.py' ! -name 'aliases.py' ! -name 'utf_8.py' \
	! -name 'ascii.py' ! -name 'latin_1.py' ! -name 'utf_8_sig.py' \
	! -name 'utf_16.py' ! -name 'utf_32.py' ! -name 'raw_unicode_escape.py' \
	! -name 'unicode_escape.py' ! -name 'idna.py' -delete

# sourceless .pyc layout.
#
# A cross build never byte-compiles (it cannot run the target interpreter), so
# `make install` leaves .py only. Compile with the *host* python instead --
# checked safe because the bytecode magic is identical: host 3.11.2 reports
# a70d0d0a, and Lib/importlib/_bootstrap_external.py in the 3.11.9 tree we
# built says MAGIC_NUMBER = 3495 = 0x0da7. Same number, so the target
# interpreter will load these.
#
# -b writes foo.pyc beside foo.py instead of __pycache__/foo.cpython-311.pyc,
# which is precisely the sourceless layout CPython will import from.
python3.11 -m compileall -b -q "$L" >/dev/null 2>&1 || true
find "$L" -name '*.py' -delete
find "$L" -name '__pycache__' -type d -exec rm -rf {} + 2>/dev/null || true

echo "=== overlay size ==="
du -sh "$DST"
du -sh "$L"
echo "=== top ==="
du -sh "$L"/* 2>/dev/null | sort -h | tail -8
