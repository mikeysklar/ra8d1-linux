# Guest networking transport for the emulated rv32 nommu Linux

Date: 2026-08-07. Everything below was run against the real image/emulator on this
machine, or read out of the actual kernel source. Nothing is quoted from docs unchecked.

---

## 0. Facts established by measurement (not assumption)

Booted `image/Image` under `emulator/mini-rv32ima/mini-rv32ima` and inspected the
running guest. Transcript reproducible via the probe script pattern in §6.

**The prebuilt guest has no networking at all, and no telnetd.**

```
~ # ls /sys/class/net
ls: /sys/class/net: No such file or directory
~ # cat /proc/net/dev
cat: can't open '/proc/net/dev': No such file or directory
```

`/proc/net` does not exist => `CONFIG_NET=n`. This matches
`emulator/configs/custom_kernel_config:435` (`# CONFIG_NET is not set`).

BusyBox applet list from the running guest (`busybox --list`) contains:
`ifconfig ip ipaddr iplink iproute nc netstat ping route telnet udhcpc wget`
and does **not** contain `telnetd`, `slattach`, or `dropbear`.

> Correction to the task brief: BusyBox telnetd is **not** built in. The reference
> config has `# CONFIG_TELNETD is not set` and `# CONFIG_SLATTACH is not set`
> (`emulator/configs/busybox_config:961,971`). The login daemon is also a rebuild.

**The emulator core has no external interrupt.** `mini-rv32ima.h` only ever sets
`mip` bit 7 (MTIP) at line 132; there is no MEIP, no PLIC, no `interrupts` property
on any device. The console 8250 enumerates as `irq = 0`, i.e. polled:

```
[0.080582] 10000000.uart: ttyS0 at MMIO 0x10000000 (irq = 0, base_baud = 1048576) is a XR16850
```

**A second 8250 node is accepted by the DTB and the kernel probes it.** I added
`uart@10000100` to `sixtyfourmb.dts`, rebuilt, and booted:

```
[0.084306] of_serial: probe of 10000100.uart failed with error -28
```

`-28` is `ENOSPC` from `serial8250_register_8250_port()`: the port array is full
because the prebuilt kernel has one port (`Serial: 8250/16550 driver, 1 ports`).
So the DTB half of SLIP already works; only `SERIAL_8250_*_UARTS` needs bumping.

---

## 1. Option 1 — SLIP over a second emulated 8250

Works on nommu without reservation. SLIP is a tty line discipline plus a netdev;
neither needs contiguous userspace allocations, and `slattach` is a ~200-line
applet that just does `ioctl(TIOCSETD, N_SLIP)` and sleeps.

**No interrupt needed.** `serial8250` with `port->irq == 0` runs a jiffy timer
(`uart_poll_timeout`, floor 1 jiffy). At `CONFIG_HZ=100` that is a 10 ms poll, and
`serial8250_rx_chars` drains while `LSR & DR` is set, so ~256 B per poll.

*Throughput ceiling ~25 KB/s, one-way latency floor ~10 ms.* Fine for an
interactive telnet session, poor for `wget`. Note the byte stream never touches a
real UART: both ends are RAM ring buffers inside the Zephyr app, so baud is irrelevant.

Emulator-side cost: ~50 lines. Clone the existing `mmio_store`/`mmio_load` arms in
`rvlinux/src/main.c:170-200` against two `ring_buf`s at `0x10000100`. `0x10000100`
is already inside `MINIRV32_MMIO_RANGE` (`0x10000000..0x12000000`,
`mini-rv32ima.h:37`), so the core needs no change at all.

**The catch is the host side, not the guest side.** SLIP gives you raw IP packets
in the Zephyr app. Putting them on the LAN needs a second `net_if` plus routing.
Zephyr has `CONFIG_NET_IPV4_FORWARDING` and `CONFIG_NET_IPV4_NAT`
(`subsys/net/ip/ipv4_nat.c`, 702 lines) but NAT is marked **EXPERIMENTAL** and
needs `NET_PKT_FILTER` + iptable rules. Budget ~250-350 lines of Zephyr glue
(SLIP framing net_if driver + NAT config) on top of the 50 emulator lines.

## 2. Option 2 — virtio-net over virtio-mmio

**Blocked by a hard dependency chain.** `virtio_mmio.c` (Linux 6.1, `vm_find_vqs`):

```c
int irq = platform_get_irq(vm_dev->pdev, 0);
err = request_irq(irq, vm_interrupt, IRQF_SHARED, dev_name(&vdev->dev), vm_dev);
```

No polling fallback; a missing IRQ is a hard probe failure. And you cannot hang the
device straight off `riscv,cpu-intc`, because `irq-riscv-intc.c` maps its lines as
per-cpu:

```c
static int riscv_intc_domain_map(struct irq_domain *d, unsigned int irq,
                                 irq_hw_number_t hwirq)
{
        irq_set_percpu_devid(irq);
        irq_domain_set_info(d, irq, hwirq, &riscv_intc_chip, d->host_data,
                            handle_percpu_devid_irq, NULL, NULL);
```

`request_irq()` on a percpu-devid IRQ is rejected by genirq. So virtio-net costs:
MEIP support in the core (~10 lines: `mip`/`mie` bit 11, trap `0x8000000B`) **plus a
minimal PLIC** (`semu/plic.c` is 132 lines) **plus** the virtio device itself.

For the device, `sysprog21/semu` is the best reference: `virtio-net.c` is 643 lines
and offers `VNET_FEATURES_0 = 0` — no MRG_RXBUF, no MAC, no CSUM. That zero is
lucky for us: with no MRG_RXBUF and no GUEST_TSO, Linux `virtio_net` uses
single-page receive buffers rather than order-3 `big packets`, which is what you
want on nommu where high-order allocations fail under fragmentation. Its host
backend is `netdev.c` (158 lines) + `slirp.c` (248 lines).

Realistic total: **~800-950 lines** of new emulator code, and the host still has
exactly the same NAT/forwarding problem as SLIP. It buys 1-2 orders of magnitude
throughput we do not need for a login shell.

## 3. Option 3 — do not give the guest a network at all

The stated goal is "an automated remote login". The guest **already** serves a root
shell on ttyS0, and the Zephyr app already owns both ends of that byte stream
(`rvlinux/src/main.c:174` TX, `:191` RX). Route those two arms to a TCP socket on
the real Ethernet instead of `uart_poll_out`/`uart_poll_in` and you have a telnet
console server.

`telnet <board-ip>` lands on the guest root prompt. Zero guest kernel changes,
zero DTB changes, zero BusyBox changes, and it works against the image we have on
disk **today**.

Cost: ~150 lines of Zephyr (listener thread, two `ring_buf`s, RFC854 minimum:
escape outbound `0xFF` as `0xFF 0xFF`, answer inbound `IAC DO/WILL` with
`WONT/DONT` except `SGA`/`ECHO`). Board support is already there: `&eth`, `&mdio`
and `phy@5` are `okay` in `ek_ra8d1.dts:369-387`, and `eth_renesas_ra.c` exists in
the Zephyr tree.

Limits, stated honestly: one session at a time, and no guest-originated traffic
(no `wget`, no `scp`). It is a console, not a network.

*Option 3b, a paravirt packet device* (guest writes a physical address + length to
a doorbell; host reads guest RAM directly since it already has the flat 64 MB
array, so zero-copy and no virtqueue) is ~60 emulator lines but needs an
out-of-tree guest netdev driver carried as a Buildroot kernel patch. Cheaper than
virtio, but it trades emulator code for image-build friction and still needs NAT.

---

## 4. RECOMMENDATION

**Stage 0 now: telnet-to-console bridge (option 3).** It satisfies the actual
requirement, needs no image rebuild, and is the only option that can be finished
and demoed against the current `Image`. Do this first regardless of what follows,
because the socket plumbing and the Zephyr networking config are shared with every
other option.

**Stage 1 only if the guest genuinely needs its own IP: SLIP (option 1).** 50
emulator lines, no core changes, no interrupt controller. Its 25 KB/s ceiling is
the price of polled 8250, and that is acceptable for anything short of bulk transfer.

**Do not build virtio-net.** MEIP + PLIC + 643 lines of virtqueue on a nommu guest,
for throughput a login shell will never use.

## 5. Exact asks for the image-build agent

Only needed for Stage 1. Stage 0 needs nothing from them.

Kernel (`custom_kernel_config`):

```
CONFIG_NET=y
CONFIG_UNIX=y
CONFIG_INET=y
CONFIG_PACKET=y
CONFIG_NETDEVICES=y
CONFIG_SLIP=y
CONFIG_SLIP_COMPRESSED=y      # pulls in CONFIG_SLHC
# CONFIG_SLIP_MODE_SLIP6 is not set
CONFIG_SERIAL_8250_NR_UARTS=2
CONFIG_SERIAL_8250_RUNTIME_UARTS=2
```

That last pair is what the measured `-28` above is asking for.

BusyBox (`busybox_config`) — all four are currently `is not set`:

```
CONFIG_SLATTACH=y
CONFIG_TELNETD=y
CONFIG_FEATURE_TELNETD_STANDALONE=y
CONFIG_IFCONFIG=y
CONFIG_ROUTE=y
```

Also confirm root has an empty or known password in the rootfs overlay, or
`telnetd -l /bin/sh` will be needed instead of `-l /bin/login`.

## 6. DTB workflow (verified)

`dtc` 1.8.1 is installed at `/opt/homebrew/bin/dtc`. From
`emulator/mini-rv32ima/Makefile:76-82`:

```sh
dtc -I dts -O dtb -o sixtyfourmb.dtb sixtyfourmb.dts -S 2048
./bintoh default64mbdtb < sixtyfourmb.dtb > default64mbdtc.h
cp default64mbdtc.h ../../rvlinux/src/default64mbdtc.h
```

**`-S 1536` must become `-S 2048`.** Adding one `uart@10000100` node pushed the
blob to 1595 bytes and `dtc` warned `blob size 1595 >= minimum size 1536`. The
padding is not cosmetic: `mini-rv32ima.c` patches the memory-size cell in place and
the Makefile comment at line 78 requires >=16 bytes of slack and 16-byte alignment.
The DTB is placed at the very top of guest RAM (`mini-rv32ima.c:151`), just below
`struct MiniRV32IMAState`, so growing it by 512 B costs 512 B of guest RAM and
nothing else. Verified: the guest still boots to a login prompt with the 2048-byte blob.

Node to add, inside `soc`:

```dts
uart@10000100 {
        clock-frequency = <0x1000000>;
        reg = <0x00 0x10000100 0x00 0x100>;
        compatible = "ns16850";
};
```

No `interrupts` property. Polled is deliberate; see §1.
