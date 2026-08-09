# Host build of the emulator, for development and testing on the Mac.
#
# Compiles the SAME emu/ sources the Zephyr app does, with the same -D flags,
# against a POSIX platform layer. If a change breaks here it breaks on the
# board, and finding out takes two seconds instead of a flash cycle on
# hardware someone else is using.

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
CFLAGS  += -I../emu -I../emu/shim -I.
# Must match TINYEMU_FLEN in ../CMakeLists.txt, for the reason in
# notes/00-port.md: the guest userspace we have is hard-float.
FLEN    ?= 64
CFLAGS  += -DMAX_XLEN=32 -DCONFIG_RISCV_MAX_XLEN=32 -DFLEN=$(FLEN)
CFLAGS  += -DUSE_GLOBAL_STATE -DCONFIG_HOST_SBI

EMU_SRC = ../emu/riscv_cpu.c ../emu/rv_iomem.c ../emu/rv_hostsbi.c \
          ../emu/rv_fdt.c ../emu/rv_machine.c ../emu/rv_virtio.c ../emu/rv_virtio_net.c
ifneq ($(FLEN),0)
EMU_SRC += ../emu/softfp.c
endif
SRC     = $(EMU_SRC) platform_host.c main_host.c

all: tinyemu-host

tinyemu-host: $(SRC) ../emu/*.h Makefile
	$(CC) $(CFLAGS) -o $@ $(SRC)

# The RV32 test payloads. Built in the `br` Docker container, which is the
# only place with a riscv32 toolchain; see tests/README.
tests:
	$(MAKE) -C tests

clean:
	rm -f tinyemu-host
	$(MAKE) -C tests clean

.PHONY: all tests clean
