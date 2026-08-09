# Build the RV32 test payloads.
#
# There is no riscv toolchain on the Mac, so this shells into the `br` Docker
# container, which has the Buildroot riscv32 cross-compiler from the guest
# image work. Set RV_CC directly if you have one locally.
#
#   make            build smoke.bin
#   make run        build it and run it under ../tinyemu-host

CONTAINER ?= br
RV_PREFIX ?= /br/buildroot/output/host/bin/riscv32-linux-
# No 'c': the trap handler steps sepc by a fixed 4, which is only correct if
# nothing in the payload assembles to a compressed instruction. zicsr/zifencei
# are separate extensions from gcc 12 onwards and have to be asked for by name.
RV_CFLAGS  = -march=rv32ima_zicsr_zifencei -mabi=ilp32 -mno-relax -O2 -g \
             -ffreestanding -nostdlib -fno-builtin -Wall -Wextra

SRC = start.S smoke.c
OBJDIR = /tmp/ra8d1-tinyemu-tests

all: smoke.bin

smoke.bin: $(SRC) smoke.ld
	docker exec $(CONTAINER) mkdir -p $(OBJDIR)
	for f in $(SRC) smoke.ld; do \
		docker cp $$f $(CONTAINER):$(OBJDIR)/$$f; \
	done
	docker exec $(CONTAINER) sh -c \
		'cd $(OBJDIR) && $(RV_PREFIX)gcc $(RV_CFLAGS) -T smoke.ld \
		 -o smoke.elf $(SRC) && \
		 $(RV_PREFIX)objcopy -O binary smoke.elf smoke.bin && \
		 $(RV_PREFIX)size smoke.elf'
	docker cp $(CONTAINER):$(OBJDIR)/smoke.bin smoke.bin
	docker cp $(CONTAINER):$(OBJDIR)/smoke.elf smoke.elf

run: smoke.bin
	$(MAKE) -C .. tinyemu-host
	../tinyemu-host smoke.bin -n 200000000

clean:
	rm -f smoke.bin smoke.elf

.PHONY: all run clean
