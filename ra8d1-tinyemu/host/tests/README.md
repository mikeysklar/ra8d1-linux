# RV32 test payloads

`smoke.c` is an S-mode payload that exercises the parts of the port that are
ours rather than TinyEMU's: the boot protocol, the generated devicetree, SBI,
the `time` CSR, Sv32 translation, page-fault delegation and the SBI timer. It
prints a line per check and shuts down through SBI.

It is the same payload the Zephyr app boots when the flash image slot is
empty, via the generated `src/testrom.h`.

## Building

There is no riscv toolchain on the Mac, so `make` shells into the `br` Docker
container and uses the Buildroot cross-compiler another agent built there
(`/br/buildroot/output/host/bin/riscv32-linux-gcc`). If you have a local
toolchain, override `RV_PREFIX`.

```sh
make            # smoke.bin + smoke.elf
make run        # build and run under ../tinyemu-host
```

Two constraints worth knowing before editing:

- **No compressed instructions** (`-march=rv32ima_zicsr_zifencei`, no `c`).
  The trap handler in `start.S` steps `sepc` by a fixed 4, which is only
  correct while every faulting instruction is 4 bytes.
- **`-mno-relax`**, because nothing sets `gp` and the linker relaxation would
  turn symbol references into `gp`-relative ones.

## Regenerating the compiled-in copy

```sh
make && python3 ../../tools/mktestrom.py
```

Kept a manual step rather than a CMake rule: building the payload needs Docker
and a Zephyr build should not.
