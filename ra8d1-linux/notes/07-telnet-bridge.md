# 07 - Telnet console bridge

Goal: `telnet <board-ip>` from the Mac lands on the emulated guest's root shell.

Status: **builds clean, not flashed, not run on hardware.** There is one hardware
question that has to be settled before it can work at all - see section 6.

---

## 1. Why this shape

The guest cannot serve a shell itself. Its kernel is `CONFIG_NET=n`, its BusyBox
has no telnetd, and virtio-net is structurally blocked (the emulator only ever
raises MTIP, so `virtio_mmio`'s `request_irq` has no external interrupt to bind).

It does not need to. The shell it already runs on its emulated 8250 has *both*
ends inside our app:

| direction | hook | address |
|---|---|---|
| guest stdout | `mmio_store()` | `0x10000000` |
| guest stdin | `mmio_load()` | `0x10000000` (RX), `0x10000005` (LSR) |

So a telnet server on the host side is just a matter of re-pointing those two
ends at a socket. Nothing in the guest changes and nothing in the guest can tell.
The guest image does not need rebuilding.

`mmio_store`/`mmio_load` now call `guest_out()` / `guest_rx_poll()` /
`guest_rx_take()`, which route to the socket when a client is attached and to
the physical UART otherwise. The UART path is unchanged when idle, so the serial
console keeps working exactly as before.

---

## 2. Kconfig needed for the Renesas Ethernet driver

Short answer: **just `CONFIG_NETWORKING=y`**. Everything else falls out of the
devicetree.

The board DTS already has `&eth`, `&mdio` and `phy@5` as `status = "okay"`, and
every driver in the chain is `default y` on its `DT_HAS_*_ENABLED` symbol:

| symbol | enabled by | file |
|---|---|---|
| `ETH_RENESAS_RA` | `DT_HAS_RENESAS_RA_ETHERNET_ENABLED` | `drivers/ethernet/Kconfig.renesas_ra` |
| `MDIO_RENESAS_RA` | `DT_HAS_RENESAS_RA_MDIO_ENABLED` | `drivers/ethernet/mdio/Kconfig.renesas_ra` |
| `PHY_GENERIC_MII` | `DT_HAS_ETHERNET_PHY_ENABLED` | `drivers/ethernet/phy/Kconfig` |

The catch is that those menus only *exist* once the ethernet driver class is
compiled, which needs `NET_L2_ETHERNET`, which needs `NETWORKING`. That is why
none of them appeared in the old `.config` even though the DT nodes were already
enabled. `prj.conf` states them explicitly anyway so a default change upstream
cannot quietly drop the link.

Two things `ETH_RENESAS_RA` pulls in by itself that are worth knowing about:

- `select USE_RA_FSP_ETHER` - the FSP `r_ether` / `r_ether_phy` sources in
  `hal_renesas`. Both present in the tree.
- `select NOCACHE_MEMORY if ARCH_HAS_NOCACHE_MEMORY_SUPPORT` - which we get,
  since we run `CONFIG_DCACHE=y` with the ARM MPU. **Verified in the map file:**
  a `nocache` region appears at `0x22000000`, 0x3080 bytes, holding all ten
  `eth_renesas_ra.c` buffer/descriptor objects. This matters a lot here: we
  turned D-cache on for the SDRAM throughput (3.8x), and without the nocache
  placement the EDMAC descriptors would be read through a stale cache line.

Also checked:

- **No IRQ collision.** `eth` takes NVIC 42 (`interrupts = <0x2a 0x0>`). No other
  enabled node in the generated DTS claims it, and entry 42 of the generated ISR
  table is populated.
- **No pin collision with anything we use.** Ethernet B is on ports 4 and 7
  (`P401/402` MDIO, `P403/405/406`, `P700-P705`). SDRAM is on ports 1, 3, 6, 9,
  10. `iic1` is `P511/P512`. The paravirt GPIOs are `P600/P414/P107/P009/P008/
  P010/P907/P507`. Disjoint.
- Entropy for TCP ISNs comes from the real TRNG
  (`CONFIG_ENTROPY_RENESAS_RA=y`), not the timer fallback.

Everything else added to `prj.conf` is ordinary IPv4/TCP/socket/DHCP config plus
modest buffer counts. IPv6 is off.

---

## 3. Scheduling - the part most likely to have bitten us

This is the non-obvious hazard and it needed a code change, not just Kconfig.

The emulator step loop **never blocks and never yields**. Zephyr schedules
preemptible threads strictly by priority, so a loop like that starves everything
numerically below it. The main thread defaults to priority 0; the Renesas
Ethernet RX thread sits at 2 and the TCP worker at 2. Left alone, the network
stack would have gotten **zero** CPU and nothing would have worked.

Fix: `main()` lowers itself to `CONFIG_RVL_EMU_PRIORITY` (default 10) via
`k_thread_priority_set()`. Final ordering, highest first:

| thread | priority |
|---|---|
| net RX traffic class | cooperative (`NET_TC_THREAD_COOPERATIVE=y`) |
| system workqueue | -1 (cooperative) |
| eth RX, TCP worker | 2 |
| telnet | 5 |
| **emulator step loop** | **10** |

The drop happens *before* the image loader, not just before the step loop. The
loader busy-polls the UART for the length of a multi-megabyte upload, and at
priority 0 that would have blackholed the network for minutes.

The guest loses a few microseconds per packet to preemption, which is
unmeasurable. Note the guest's timebase is wall-clock (`now_us()` feeds real
elapsed microseconds into `MiniRV32IMAStep`), so preemption shows up as time
passing, not as a stalled clock - which is the correct behaviour.

---

## 4. Ring buffers

Two hand-rolled single-producer/single-consumer byte rings, power-of-two sized,
`volatile` head/tail with a `barrier_dmem_fence_full()` between the data write
and the index publish. Exactly one thread on each side of each ring, which is
what lets them run lock-free.

| ring | producer | consumer | default size |
|---|---|---|---|
| `tn_tx` (guest to client) | emulator thread | telnet thread | 8192 |
| `tn_rx` (client to guest) | telnet thread | emulator thread | 1024 |

Rolled by hand rather than using `sys_ring_buf` for one reason: `spsc_empty()`
is on the genuine hot path. Linux polls the 8250 LSR far more often than a byte
actually moves, so the common case has to cost two loads and a compare, with no
interrupt lock. `guest_out()` on the store side is a bounds check and a byte
store.

**The emulator never blocks on the network.** When `tn_tx` is full, guest output
is dropped and counted (`tn_dropped`, reported on the UART at disconnect). That
is the only policy that does not risk stalling guest execution behind a TCP
retransmit timeout. 8 KB is sized to absorb a kernel boot log burst.

The telnet thread drives everything else: `zsock_poll()` with a 5 ms timeout,
then drain `tn_tx` and send in batches of up to 256 bytes. Batching matters -
one TCP segment per character would be both slow and rude. `TCP_NODELAY` is set
since the batching already does the coalescing Nagle would.

Ring reset only ever happens with `tn_active == false`, i.e. with the producer
of both rings quiescent, so the reset is not racing anything.

---

## 5. Telnet protocol

Enough of RFC 854 to keep negotiation out of the guest's shell, no more.

**Opening offer:** `IAC WILL ECHO`, `IAC WILL SUPPRESS-GO-AHEAD`,
`IAC DO SUPPRESS-GO-AHEAD`. That is the classic pair that drops a client out of
line mode into character-at-a-time, which is what we want - the guest tty does
its own echo and line editing.

**Inbound state machine** handles `IAC`, the four negotiation verbs, and
subnegotiation (`SB ... IAC SE`, skipped, with `IAC IAC` correctly treated as
escaped data inside it rather than as the end).

**Loop avoidance** is the subtle part, and the rule is:

- `DO x` -> `WILL x` for ECHO and SGA, `WONT x` for everything else.
- `WILL x` -> `DO x` for SGA, `DONT x` for everything else.
- `WONT x` / `DONT x` -> **no reply at all.** These are final confirmations;
  answering them is what creates negotiation ping-pong.
- Options we already announced are not announced twice, so a client echoing our
  `WILL` back as `DO` does not restart the exchange.

**CR handling:** Enter arrives as `CR LF` or `CR NUL`. We pass the CR (the guest
tty maps it via ICRNL) and swallow the trailing NUL or LF.

**Outbound:** `0xFF` is doubled, done in the flush path rather than in
`guest_out()` so the hot path stays cheap.

On connect a bare `\r` is pushed into `tn_rx` so the shell reprints its prompt
and the client sees something immediately instead of a seemingly dead socket.

A second simultaneous client gets `"guest console already in use"` and an
immediate close, rather than sitting in the accept queue looking hung.

---

## 6. THE HARDWARE QUESTION - read this before flashing

**The Zephyr board doc says Ethernet needs SW1-7 (SDRAM) OFF. We cannot run
without SDRAM.**

From `boards/renesas/ek_ra8d1/doc/index.rst`, the Ethernet row is:

| SW1-1 PMOD1 | SW1-2 TRACE | SW1-3 CAMERA | SW1-4 ETHA | SW1-5 ETHB | SW1-6 GLCD | SW1-7 SDRAM | SW1-8 I3C |
|---|---|---|---|---|---|---|---|
| OFF | OFF | OFF | OFF | **ON** | OFF | **OFF** | OFF |

and note 03 already established that SW1-7 OFF *physically isolates the SDRAM
from the MCU bus*. Guest RAM is that SDRAM. Taken literally, this kills the
whole thing.

**I believe the table is over-specified rather than exclusive**, for three
reasons:

1. **No pin overlap.** Ethernet B is ports 4 and 7; SDRAM is ports 1, 3, 6, 9,
   10. There is no mechanism by which the SDRAM chip being connected to the bus
   would disturb the RMII PHY.
2. **The table already combines features elsewhere.** The MIPI and parallel
   graphics rows both have SW1-6 GLCD **and** SW1-7 SDRAM ON, because those
   demos need a framebuffer. So the rows are "what this use case needs", with
   everything not needed switched off - not "these are mutually exclusive".
3. The one real conflict on ports 4/7 is **SDHC1 vs Ethernet**, which the
   pinctrl file itself flags (`/* NOTE: pins conflict with ether_default */`).
   SDHC is not enabled here.

**So the setting to try is SW1-5 ON, SW1-7 left ON as it is today**, with SW1-3
CAMERA and SW1-4 ETHA OFF (our DTS uses the `_B` pin assignment throughout -
`RMII0_TXD0_B`, `REF50CK0_B`, etc.).

This is untested and I could be wrong. The good news is that **the test is
self-diagnosing and takes one boot**: the app prints SDRAM size and does a
3.4 MB copy into it before the guest starts. If SW1-5 ON broke SDRAM, the banner
and the guest boot would fail loudly and immediately. Flip SW1-5 back and
nothing is lost.

---

## 7. What is unverified without hardware

Everything past compilation. Specifically:

- **The SW1-5 / SW1-7 coexistence above.** Biggest unknown.
- **Whether the RA Ethernet driver has ever been exercised on this board.**
  I could not find a single sample or test in the tree that enables
  `ETH_RENESAS_RA`, and `ek_ra8d1.yaml` does **not** list `ethernet` in its
  `supported:` features (it lists gpio, uart, watchdog, usbd, display, counter,
  i2s, i3c, video). The DTS nodes, pinctrl, driver, MDIO driver and PHY address
  are all present and self-consistent, but that is not the same as anyone having
  run it. Treat first bring-up as bring-up.
- The ICU event-link line in the driver
  (`R_ICU->IELSR[42] = EVENT_EDMAC_EINT(0)`) compiles for RA8, so the FSP event
  exists, but the interrupt has not been seen to fire.
- PHY address 5 and RMII link/autoneg - taken on faith from the upstream board
  DTS.
- DHCP on the user's LAN. If there is no lease, `CONFIG_RVL_NET_FALLBACK_ADDR`
  (empty by default) can be set to a static address; DHCP keeps running so a
  late lease still takes effect. Or set `CONFIG_RVL_NET_STATIC_IP=y` to skip
  DHCP entirely.
- Real telnet client negotiation. Written against the RFC, not tested against
  macOS `telnet`.
- Throughput and latency of the console under a boot-log burst, and whether
  8 KB of TX ring is actually enough to avoid drops. `tn_dropped` reports this.

---

## 8. Build

Builds clean with no warnings, both with and without networking:

| config | text | data | bss |
|---|---|---|---|
| `CONFIG_NETWORKING=y` (default) | 137,116 | 5,364 | 67,645 |
| `-DCONFIG_NETWORKING=n` | 59,792 | 2,044 | 16,446 |

Flash 6.9% of 2016 KB, RAM 8.15% of 896 KB. The no-networking build is kept
working as the escape hatch if Ethernet turns out to be unusable on this board.

```sh
source /Users/sklarm/Downloads/ada/siwx917/env.sh
source /Users/sklarm/Downloads/ada/siwx917/.venv/bin/activate
cd /Users/sklarm/Downloads/ada/siwx917/circuitpython/ports/zephyr-cp
west build -p always -b ek_ra8d1 /Users/sklarm/Downloads/ada/siwx917/ra8d1-linux/rvlinux \
  -d /Users/sklarm/Downloads/ada/siwx917/ra8d1-linux/rvlinux/build
```

## 9. Files touched

- `rvlinux/src/main.c` - console split (`guest_out` / `guest_rx_poll` /
  `guest_rx_take` vs the UART-only `uart_rx_*` the loader uses), the telnet
  bridge, network bring-up, the priority drop.
- `rvlinux/prj.conf` - networking block.
- `rvlinux/Kconfig` - **new.** App options. Because this file now exists, Zephyr
  uses it as the Kconfig root, so it ends with `source "Kconfig.zephyr"`.

`src/main.c.orig`, the paravirt I/O bridge at `0x11200000`, its selftest, and
`app.overlay` are all untouched.

## 10. Expected console output

```
=== rv32ima Linux on EK-RA8D1 ===
core 480 MHz, sdram 64 MB @ 0x68000000
--- paravirt I/O selftest @ 0x11200000 ---
...
net: mac 74:90:50:b0:5d:e9, dhcp.....

***  board ip 192.168.1.47   guest console:  telnet 192.168.1.47  ***

image ok, 3414528 bytes
copy 3414528 B flash->sdram... ok 41 ms
booting guest...
```

then on connect, on the UART:

```
telnet: session 1 from 192.168.1.20, console moving off uart
```

and on disconnect:

```
telnet: client gone, console back on uart
```
