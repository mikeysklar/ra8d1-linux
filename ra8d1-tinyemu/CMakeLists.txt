cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ra8d1_tinyemu)

# emu/ is portable C99 and knows nothing about Zephyr or this board. The same
# files are compiled unchanged by host/Makefile for the Mac test harness, so
# nothing in here may include a Zephyr header. Everything emu/ needs from the
# outside it asks for through emu/rv_platform.h.
target_sources(app PRIVATE
	emu/riscv_cpu.c
	emu/rv_iomem.c
	emu/rv_hostsbi.c
	emu/rv_fdt.c
	emu/rv_machine.c
	emu/rv_virtio.c
)

# Guest floating point. See notes/00-port.md section "Float".
#
# The rv32 Buildroot guest we have is built for the ilp32d ABI, so its glibc
# and every binary linked against it execute F and D instructions. With
# TINYEMU_FLEN=0 those are illegal instructions and init dies with SIGILL -
# observed, not predicted. 64 is therefore the default; drop it to 0 and save
# the flash below only once the guest userspace is rebuilt soft-float.
set(TINYEMU_FLEN 64 CACHE STRING "Guest FP register width: 0, 32 or 64")

if(NOT TINYEMU_FLEN EQUAL 0)
	target_sources(app PRIVATE emu/softfp.c)
	set_source_files_properties(emu/softfp.c
		PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-sign-compare")
endif()

# src/ is the platform shim plus glue. platform_zephyr.c is the only file that
# knows what board this is, and it takes its facts from devicetree.
target_sources(app PRIVATE
	src/main.c
	src/platform_zephyr.c
)

# The guest console over TCP. Nothing under emu/ is involved: the bridge sits
# behind plat_putc()/plat_getc() in platform_zephyr.c, so the host harness and
# a no-networking build both compile without it. src/telnet.h stubs the whole
# interface to inline no-ops when CONFIG_NETWORKING is off, which is what keeps
# main.c and platform_zephyr.c free of #ifdefs.
target_sources_ifdef(CONFIG_NETWORKING app PRIVATE src/telnet.c)

# The TCP image loader, speaking the protocol rvlinux's pushimage.py already
# tests. Same arrangement: src/pusher.h stubs its interface to inline no-ops
# when it is off, so main.c has no #ifdefs. Unlike telnet.c this one is guarded
# on its own symbol rather than on CONFIG_NETWORKING, because the flash
# accessors it needs from platform_zephyr.c are compiled unconditionally and
# nothing else in a no-pusher build has any use for a listener.
target_sources_ifdef(CONFIG_RVT_PUSHER app PRIVATE src/pusher.c)

target_include_directories(app PRIVATE src emu emu/shim)

# TinyEMU build-time configuration.
#
#   MAX_XLEN=32 / CONFIG_RISCV_MAX_XLEN=32
#       Build only the RV32 template. The 64- and 128-bit instantiations are
#       separate translation units upstream and are simply not compiled.
#   FLEN
#       Width of the guest's FP registers, from TINYEMU_FLEN above. At 0 every
#       "#if FLEN > 0" block disappears and softfp.c is not compiled at all.
#       The value also feeds misa, which feeds the generated devicetree's
#       riscv,isa - there is no second place to keep in step.
#   USE_GLOBAL_STATE
#       Upstream guards a static RISCVCPUState behind this (riscv_cpu.c:57)
#       for the emscripten build. Defining it here is what replaces the one
#       and only mallocz() in the core with a .bss object, so the emulator
#       does no dynamic allocation at all. See notes/00-port.md.
#   CONFIG_HOST_SBI
#       Enables the five-line hook in raise_exception2() that services the
#       guest's supervisor ecall in host C instead of in emulated M-mode
#       firmware. Also documented in notes/00-port.md.
target_compile_definitions(app PRIVATE
	MAX_XLEN=32
	CONFIG_RISCV_MAX_XLEN=32
	FLEN=${TINYEMU_FLEN}
	USE_GLOBAL_STATE
	CONFIG_HOST_SBI
)

# The vendored TinyEMU sources are not warning-clean under Zephyr's flags and
# we do not want to diverge from upstream just to silence them. Confine the
# suppression to the one file that needs it.
set_source_files_properties(emu/riscv_cpu.c
	PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-sign-compare")
