# Host-side test harnesses. Everything under ../emu is the same code the
# firmware compiles, so a pass here is meaningful for the target.
#
#   make test    prelim + zexdoc + zexall against the vendored Z80 core
#   make render  decode a ROM image and dump frames as PNM
#   make surface guards the zero-copy contract with the display layer
#   make bench   Z80 instruction-rate benchmark on this machine

CC      ?= cc
CFLAGS  := -O2 -std=c99 -Wall -Wextra -I../emu -DZ80_TRACE
LDFLAGS :=

BINS := test_z80 test_pacman test_surface bench
ROM  := testrom.bin

.PHONY: all test render surface runbench clean

all: $(BINS)

test_z80: test_z80.c ../emu/z80.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_pacman: test_pacman.c ../emu/z80.c ../emu/pacman.c ../emu/romimage.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_surface: test_surface.c ../emu/z80.c ../emu/pacman.c ../emu/romimage.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

bench: bench.c ../emu/z80.c ../emu/pacman.c ../emu/romimage.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# The synthetic ROM is generated, never committed as a binary.
$(ROM): ../tools/mktestrom.py ../tools/romimage.py
	python3 ../tools/mktestrom.py $@

# zexall is ~5.8 billion instructions; budget a couple of minutes.
test: test_z80
	./test_z80

render: test_pacman $(ROM)
	./test_pacman $(ROM)

# Guards the zero-copy contract with the display layer.
surface: test_surface $(ROM)
	./test_surface $(ROM)

runbench: bench $(ROM)
	./bench $(ROM)

clean:
	rm -f $(BINS) *.o *.pnm $(ROM)
