# 08 — Guest networking for the MMU rv32 Linux guest (TinyEMU on EK-RA8D1)

Date: 2026-08-07. Design + research only. **No source files were modified.**

This supersedes `06-guest-net.md` for the MMU target. That note was written against
mini-rv32ima with a nommu guest, and two of its three blocking arguments no longer
apply: TinyEMU already has a PLIC and already has virtio-net, so the "MEIP + PLIC +
643 lines of virtqueue" cost it priced is now zero. Its Stage-0 recommendation
(telnet-to-console bridge, `07-telnet-bridge.md`) still stands and is orthogonal —
keep it.

Everything marked VERIFIED was read out of source on this machine today, with
file:line. Everything marked INFERRED is reasoning I did not execute.

---

## 0. Recommendation in one paragraph

**Use TinyEMU's existing virtio-net, and bridge it to the real Ethernet with
Zephyr's `CONFIG_NET_ETHERNET_BRIDGE`.** The emulator half is now essentially free:
the storage design already commits us to virtio-blk, which already requires the
PLIC, so networking inherits a paid-for interrupt controller and **adds 556 bytes
of flash** (measured, §1.7). The Zephyr half needs three things: a ~200-line Zephyr
Ethernet driver presenting the guest as a second `net_if`, a ~20-line patch to the
Renesas RA Ethernet driver to expose promiscuous mode (the hardware supports it; the
driver does not offer it), and moving Zephyr's own DHCP/telnet onto the bridge
interface. Total new code ~260 lines of Zephyr plus ~80 of emulator glue, none of it
novel. **But** see §5: guest networking is *not* what unblocks Blinka, and `pip
install adafruit-blinka` will actually fail on riscv32 even with perfect networking.
Do the offline wheelhouse first; it is an afternoon and it is the thing that works.

---

## 1. VERIFIED facts established today

### 1.1 TinyEMU's virtio-net is complete, modern, and small

Source read: `/private/tmp/.../scratchpad/tinyemu-2019-12-21` (also `/br/tinyemu-2019-12-21`).

| Fact | Evidence |
|---|---|
| virtio-mmio **version 2** (modern, VIRTIO 1.0), not legacy | `virtio.c:620-622` returns `2` for `VIRTIO_MMIO_VERSION`; `virtio.c:629-641` returns `1` for feature word 1, i.e. `VIRTIO_F_VERSION_1` |
| Magic is correct | `virtio.c:618` `0x74726976` |
| virtio-net device is 130 lines | `virtio.c:1135-1258` |
| Offers exactly one feature: `VIRTIO_NET_F_MAC` | `virtio.c:1243` `device_features = (1 << 5)`; `VIRTIO_NET_F_STATUS` is commented out |
| Header is the 12-byte virtio-1.0 `virtio_net_hdr_v1` | `virtio.c:1143-1151`, `header_size = sizeof(VIRTIONetHeader)` at `:1257` |
| Zero-copy into guest RAM, no DMA API | `virtio.c:186-189` `virtio_mmio_get_ram_ptr()` returns a host pointer straight into the guest RAM array |
| RX is host-driven, TX is guest-driven | `virtio_net_write_packet()` `virtio.c:1189` (host → guest, queue 0, `manual_recv = TRUE` at `:1245`); `virtio_net_recv_request()` `virtio.c:1153` handles `queue_idx == 1` only (guest → host) |
| Backpressure hook already exists | `virtio_net_can_write_packet()` `virtio.c:1177` returns FALSE when the RX ring is empty |
| Host backend is a 4-function vtable | `virtio.h:85-104` `struct EthernetDevice`: we implement `write_packet` (guest→wire) and call `device_write_packet` (wire→guest) |

**Only two device features are negotiated, and that is good for us.** With no
`VIRTIO_NET_F_MRG_RXBUF` and no `GUEST_TSO*`, Linux's `virtio_net` selects
small-receive-buffer mode — one descriptor chain per frame, no high-order page
allocations. Simplest possible path.

### 1.2 TinyEMU's PLIC is 90 lines and Linux 6.8 binds to it

- `riscv_machine.c:241-315` — the whole PLIC. It implements *only* the per-hart
  threshold (`0x200000`) and claim/complete (`0x200004`) registers. Priority and
  enable-bit writes are silently dropped (`default:` arms at `:277` and `:298`).
- It asserts both `MIP_MEIP` and `MIP_SEIP` (`riscv_machine.c:250`).
- Devicetree compatible is `"riscv,plic0"` (`riscv_machine.c:687`). **Linux 6.8-rc1
  matches it**: `IRQCHIP_DECLARE(riscv_plic0, "riscv,plic0", plic_init); /* for
  legacy systems */` at `drivers/irqchip/irq-sifive-plic.c:573` in
  `/br/buildroot/output/build/linux-6.8-rc1`. Verified by reading the file in the
  `br` container.

So the whole reason `06-guest-net.md §2` called virtio-net "blocked by a hard
dependency chain" is gone. We are not writing MEIP support or a PLIC; we are
copying 90 lines that already boot Linux.

### 1.3 Zephyr 4.4.99 has a real Ethernet bridge

Tree: `/Users/sklarm/Downloads/ada/siwx917/circuitpython/ports/zephyr-cp/zephyr`
(v4.4.99, HEAD `1f6f2c3`).

- `subsys/net/l2/ethernet/bridge/` — `bridge.c`, `bridge_input.c`, `bridge_fdb.c`,
  `bridge_shell.c`. New in 2025 (NXP).
- `CONFIG_NET_ETHERNET_BRIDGE` `selects NET_PROMISCUOUS_MODE` and `NET_L2_VIRTUAL`
  (`subsys/net/l2/ethernet/bridge/Kconfig:8-10`).
- API: `eth_bridge_iface_add(struct net_if *br, struct net_if *iface)`
  (`include/zephyr/net/ethernet_bridge.h:84`).
- The bridge interface is a `NET_L2_VIRTUAL` iface with capability
  `VIRTUAL_INTERFACE_BRIDGE` (`include/zephyr/net/virtual.h:47`). It is created by
  the subsystem, not the app; the app finds it with `net_eth_bridge_foreach()`.
- Forwarding is hooked into the normal Ethernet RX path:
  `subsys/net/l2/ethernet/ethernet.c:288-293` calls `eth_bridge_input_process()`
  right after the L2 header is parsed.
- A working reference app exists: `samples/net/ethernet/bridge/` (prj.conf + 105-line
  `src/main.c`). Its `main()` is literally `net_eth_bridge_foreach(find)` →
  `net_if_foreach(add)` → `net_if_up(bridge)`.

### 1.4 The blocker: the Renesas RA Ethernet driver does not offer promiscuous mode

This is the one real gap, and it is small.

```c
/* drivers/ethernet/eth_renesas_ra.c:153 */
static enum ethernet_hw_caps renesas_ra_eth_get_capabilities(...)
{
        return ETHERNET_LINK_10BASE | ETHERNET_LINK_100BASE;
}

/* drivers/ethernet/eth_renesas_ra.c:340 */
static const struct ethernet_api api_funcs = {
        .iface_api.init   = renesas_ra_eth_initialize,
        .get_capabilities = renesas_ra_eth_get_capabilities,
        .get_phy          = renesas_ra_eth_get_phy,
        .send             = renesas_ra_eth_tx,
};                                    /* <-- no .set_config at all */
```

The gate chain, all verified:

1. `eth_bridge_iface_add()` returns `-EINVAL` unless the iface reports
   `ETHERNET_PROMISC_MODE` — `bridge.c:113-116`.
2. `ETHERNET_PROMISC_MODE` (`BIT(10)`, `ethernet.h:173`) is what sets the
   `NET_L2_PROMISC_MODE` L2 flag — `ethernet.c:1060-1063`.
3. `net_if_set_promisc()` → `promisc_mode_set()` returns `-ENOTSUP` without that
   flag — `net_if.c:6429-6432`.
4. With the flag, it calls `net_eth_promisc_mode()` → `net_mgmt(NET_REQUEST_
   ETHERNET_SET_PROMISC_MODE)` → the driver's `.set_config` — `ethernet.c:987-995`.

**The hardware and the HAL both support it.** The ETHERC has an `ECMR.PRM` bit, and
FSP writes it: `modules/hal/renesas/drivers/ra/fsp/src/r_ether/r_ether.c:1790`
`p_reg_etherc->ECMR_b.PRM = p_instance_ctrl->p_ether_cfg->promiscuous;`, inside
`ether_config_ethernet()` (`r_ether.c:1447`), which runs on link establishment. The
enum values exist (`r_ether_api.h:72-73`, `ETHER_PROMISCUOUS_ENABLE = 1`). The
Zephyr driver simply hardcodes `.promiscuous = ETHER_PROMISCUOUS_DISABLE`
(`eth_renesas_ra.c:135`) in a `const` config struct and never exposes a way to
change it.

A 12-line `.set_config` in the style of `drivers/ethernet/eth_xmc4xxx.c:1141-1154`
closes this. See §7.3.

### 1.5 Buffer sizing is currently far too small for bridging

- `eth_bridge_forward()` calls `net_pkt_clone(pkt, K_NO_WAIT)` for **every**
  forwarded frame (`bridge_input.c:81`). Bridging therefore doubles net_buf demand.
- `CONFIG_NET_BUF_DATA_SIZE` defaults to **128** bytes in fixed-data mode
  (`subsys/net/ip/Kconfig:784-787`). A 1514-byte frame is 12 net_bufs.
- Current `rvlinux/prj.conf` sets `NET_BUF_RX_COUNT=32` / `TX_COUNT=32`, i.e. about
  **two and a half full-size frames per pool**. That is fine for a telnet console
  and hopeless for a bridge.

Fix in §7.5.

### 1.6 VERIFIED: virtio-net costs 556 bytes of flash on top of virtio-blk

Measured today on this machine, `arm-none-eabi-gcc 14.3.rel1`, Cortex-M85, `-O2
-ffunction-sections -fdata-sections -Wl,--gc-sections`, stub externs, entry point
calling only the device `*_init()` functions under test. Text bytes:

| Devices linked | text |
|---|---:|
| `virtio.c` compiled, nothing collected | 13 167 |
| **block only** | **5 166** |
| net only | 4 968 |
| console only | 4 584 |
| **block + net** | **5 722** |
| block + net + console | 5 886 |

The 5 166 figure reproduces the other agent's block-only measurement exactly, which
validates the harness.

> **virtio-net's marginal cost over the virtio-blk build we are already committed to
> is 5 722 − 5 166 = 556 bytes of text.** Against 2 MB of flash. Adding the virtio
> console too would cost a further 164 B.

This is the number that should settle any "is virtio-net worth it" argument on size
grounds. It is not. The cost of option #1 is entirely on the Zephyr side.

### 1.7 The PLIC is no longer a cost attributable to networking

Storage is virtio-blk over virtio-mmio, and `drivers/virtio/virtio_mmio.c` has no
polling fallback — `vm_find_vqs()` does `platform_get_irq()` then `request_irq()`
and fails the probe if either fails. So the PLIC (`riscv_machine.c:241-315`, 90
lines) is already on the critical path for the rootfs. Networking reuses it: same
PLIC, same `IRQSignal` array, `VIRTIO_IRQ + n` for the *n*th bus slot.

`06-guest-net.md §2` priced virtio-net at "MEIP + PLIC + 643 lines of virtqueue".
Every term in that sum is now zero.

### 1.8 Board Ethernet facts

- `boards/renesas/ek_ra8d1/ek_ra8d1.dts:369-387` — `&eth` okay, MAC
  `74:90:50:b0:5d:e9`, `&mdio` okay, `phy@5` okay.
- `g_ether0_mac_address[6] = DT_INST_PROP(0, local_mac_address)` —
  `eth_renesas_ra.c:57`. The guest needs a *different* MAC; pick a locally
  administered one (bit 1 of octet 0 set), e.g. `76:90:50:b0:5d:e9`.
- `ETHER_BUF_SIZE 1536` (`eth_renesas_ra.c:35`) — full frames, no fragmentation
  issue at the driver.

---

## 2. Q1 — Ranked transports

### #1 — virtio-net, bridged into Zephyr's real Ethernet at L2

Guest sees a normal `eth0`. `udhcpc` gets a lease from the same DHCP server that
gave Zephyr `192.168.2.3`. The guest is a peer on the LAN, not behind anything.

- Emulator side: **556 bytes of flash and ~80 lines of glue.** `virtio.c` and the
  90-line PLIC are already coming across for virtio-blk (§1.6, §1.7). We write the
  `EthernetDevice` vtable and add one DTB node.
- Zephyr side: a guest-facing `net_if` (~200 lines), the RA promiscuous patch
  (~20 lines), bridge wiring in `main()` (~40 lines, copyable from the sample).
- Throughput: bounded by emulator speed, not by the transport. See §2.5.
- Risk: medium-low. Every piece exists and is known to work; the assembly is new.

**This is the recommendation.**

### #2 — virtio-net, NAT/proxy at the socket layer instead of bridged

Same emulator side. Instead of bridging, Zephyr terminates the guest's L2 on an
internal subnet (say `192.168.7.0/24`), runs a tiny DHCP/ARP responder, and NATs
outbound TCP/UDP through its own socket API.

- Zephyr has `CONFIG_NET_IPV4_NAT` (`subsys/net/ip/Kconfig.ipv4:260-269`) but it is
  `EXPERIMENTAL`, depends on `NET_IPV4_ROUTE`, selects `NET_PKT_FILTER` +
  `NET_PKT_FILTER_IPV4_HOOK`, and needs iptable rules configured in C.
- Fall back position if #1's promiscuous patch turns out not to stick across a PHY
  re-link, or if the upstream LAN objects to a second MAC.
- Cost: +250-400 lines over #1, and you are debugging experimental NAT.

### #3 — SLIP / PPP over a second emulated UART

- Zephyr has `drivers/net/slip.c` and `CONFIG_SLIP_TAP=y` even carries Ethernet
  frames, but it is built on `CONFIG_UART_PIPE` + `UART_INTERRUPT_DRIVEN`
  (`drivers/net/Kconfig:148-152`), i.e. it wants a real Zephyr UART device. Our
  emulated UART is not one. We would fake a `uart_pipe` backend.
- The 25 KB/s ceiling from `06-guest-net.md §1` was a consequence of mini-rv32ima's
  polled 8250 with no interrupt. With a PLIC available that ceiling lifts — but then
  you are doing byte-stuffing on a path where virtio already gives you framed,
  zero-copy packets.
- **Strictly dominated by #1.** No reason to pick it now.

### #4 — paravirt socket bridge (guest MMIO device, Zephyr does the BSD sockets)

This got materially *worse* when we moved to glibc/ELF. Under nommu with a hand-rolled
userspace you could plausibly point a couple of programs at a custom API. With a full
userspace, `pip`, `ssh` and everything else call `socket()`/`connect()` through glibc
into the kernel. To serve them you would have to implement a Linux address family or
an `AF_INET` shim inside the guest kernel that forwards to the MMIO device — that is
an out-of-tree kernel networking patch carried in Buildroot forever, for less
function than virtio-net gives free.

**Do not build this.** It is the only option where the MMU move increases the cost.

### 2.5 What throughput to actually expect (INFERRED)

`sv32-mmu.md §5b` measured TinyEMU at 464-468 MIPS — **in a Debian container on the
Mac, not on the M85.** Do not carry that number to the board. A rough M85 estimate:
480 MHz, an interpreted RV32 step costing on the order of 10-20 M85 cycles once
SDRAM and the softmmu TLB are in play, gives **~25-50 MIPS**. Linux's virtio_net RX
path plus the IP stack is a few thousand instructions per packet, so low thousands of
packets/sec, i.e. roughly **1-5 MB/s**. Ample for SSH and for `pip` pulling a few MB
of wheels. Not a file server. This is a guess and should be measured, not trusted.

---

## 3. Q2 — The Zephyr-side bridging problem

**Short answer: yes, true L2 bridging is workable, and it is the intended
mechanism. One driver patch stands in the way.**

### 3.1 Does Zephyr support a raw/promiscuous packet path?

Three distinct mechanisms exist; only one is right for us.

| Mechanism | Symbol | Verdict |
|---|---|---|
| Promiscuous *capture* | `CONFIG_NET_PROMISCUOUS_MODE` | Wrong shape. `net_promisc_mode_wait_data()` (`include/zephyr/net/promiscuous.h:40`) hands the app a *copy* for inspection; the packet still goes up the normal stack. It is tcpdump, not a bridge. |
| Raw mode / no IP stack | `CONFIG_NET_RAW_MODE`, `drivers/ethernet/eth_raw.c` | Wrong target. It stubs out the upper layer entirely (`__weak net_recv_data()` returning `-ENOTSUP`). That would cost us Zephyr's own telnet console and DHCP. |
| **L2 bridge** | **`CONFIG_NET_ETHERNET_BRIDGE`** | **Correct.** Purpose-built, forwards frames between member ifaces, and the bridge iface itself can hold an IP address so Zephyr keeps its own stack. |

### 3.2 How the bridge actually behaves (read from source)

`eth_bridge_input_process()` (`bridge_input.c:126`) runs on every frame received on
a member interface and decides three ways:

- destination MAC == the **bridge's** MAC → `eth_bridge_handle_locally()`
  (`bridge_input.c:99`), delivered up the bridge's own IP stack;
- otherwise → `eth_bridge_forward()` (`bridge_input.c:77`), which
  `net_pkt_clone()`s and `net_if_queue_tx()`s to the bridge, which fans it out to
  the other members;
- IEEE 802.1D link-local group addresses (`01:80:c2:00:00:0x`) are filtered
  (`bridge_input.c:110-123`), so we do not forward STP/LLDP noise into the guest.

Consequence for us, and it is the important architectural point:

> **Zephyr's own IP address moves from `eth0` to `bridge0`.** The reference sample
> does exactly this — `net_dhcpv4_restart(net_eth_get_bridge(eth_ctx))` on
> `NET_EVENT_IF_UP` (`samples/net/ethernet/bridge/src/main.c:87`).

So after this change the board takes **two** DHCP leases on the same wire: one for
`bridge0` (Zephyr, still serving telnet on :23) and one for the guest's `eth0`. Two
MACs, two leases, one physical port.

### 3.3 Will the Mac's Internet Sharing segment tolerate two MACs? (INFERRED)

macOS Internet Sharing runs `bootpd` plus NAT and behaves as a router serving a
shared segment; it is designed for multiple simultaneous clients. A second MAC
asking for a lease should just get one. **Not tested.** Cheap way to de-risk before
writing any code: plug a second machine into the same shared segment and confirm it
gets a lease. Five minutes, and it invalidates or confirms the whole approach.

If it does not work, fall to option #2 (NAT at the socket layer), where only one MAC
is ever visible on the wire.

### 3.4 Is NAT/proxy the pragmatic answer instead?

Not as first choice. `CONFIG_NET_IPV4_NAT` is `EXPERIMENTAL`, requires
`NET_PKT_FILTER` and hand-written iptable rules, and the bridge path gives the guest
a *real* address which is what makes `ssh board-guest` work without port-forward
bookkeeping. Keep NAT as the documented fallback (§9), not the plan.

---

## 4. Q3 — What the guest kernel needs

### 4.1 Kernel CONFIG symbols

Verified against `/br/buildroot/output/build/linux-6.8-rc1`.

```
# --- core net ---
CONFIG_NET=y
CONFIG_UNIX=y
CONFIG_INET=y
CONFIG_PACKET=y                 # ARP/DHCP userspace clients want AF_PACKET

# --- interrupt controller (needed by virtio-mmio; not optional) ---
CONFIG_RISCV_INTC=y             # selected by arch
CONFIG_SIFIVE_PLIC=y            # drivers/irqchip/Kconfig:543, bool, depends on RISCV
                                #   matches "riscv,plic0" at irq-sifive-plic.c:573

# --- virtio ---
CONFIG_VIRTIO=y                 # selected by VIRTIO_MMIO
CONFIG_VIRTIO_MENU=y
CONFIG_VIRTIO_MMIO=y            # drivers/virtio/Kconfig:153, "depends on HAS_IOMEM && HAS_DMA"
# CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES is not set   # DT nodes are cleaner

# --- the NIC ---
CONFIG_NETDEVICES=y
CONFIG_NET_CORE=y
CONFIG_VIRTIO_NET=y             # drivers/net/Kconfig:433; selects NET_FAILOVER, DIMLIB

# --- optional but convenient ---
CONFIG_IP_PNP=y                 # ip=dhcp on the kernel cmdline, no userspace needed
CONFIG_IP_PNP_DHCP=y
```

Notes:

- **Build `VIRTIO_MMIO` and `VIRTIO_NET` as `=y`, not `=m`.** No module loading
  hassle in an initramfs we control.
- `VIRTIO_NET` pulls in `NET_FAILOVER` and `DIMLIB` automatically; both are small.
- TinyEMU advertises virtio-mmio **version 2**, so no legacy-device quirks and no
  `virtio_mmio.device=` cmdline hack is needed.
- `CONFIG_IP_PNP_DHCP` with `ip=dhcp` on the bootargs is the shortest path to a
  working guest address; BusyBox `udhcpc` is the alternative if you want lease
  renewal.

### 4.2 Consequences of implementing SBI in the machine layer instead of loading BBL

We are not running OpenSBI or BBL in the guest; the machine layer traps `ECALL` from
S-mode and services it host-side. That is the right call, but it moves two setup
jobs that BBL normally does onto us, and **if either is missed, the symptom is
"virtio interrupts never arrive"** — which will look like a virtio bug and is not.

1. **`mideleg` must delegate the S-mode interrupts.** TinyEMU's PLIC asserts *both*
   `MIP_MEIP` and `MIP_SEIP` (`riscv_machine.c:250`). Linux runs in S-mode and waits
   on `sip.SEIP` (bit 9). With `mideleg = 0` the interrupt is taken in M-mode, where
   there is no guest handler at all, and the guest hangs or trap-loops silently.
   Set `mideleg = 0x222` (SSIP bit 1, STIP bit 5, SEIP bit 9) at reset — the value
   BBL uses.
2. **`medeleg` must delegate the usual exceptions**, or every page fault from the
   Sv32 walk goes to M-mode instead of Linux. BBL's value is `0xb1ff` (misaligned
   fetch, fetch/load/store access faults, illegal instruction, breakpoint, ecall
   from U, and the three page faults 12/13/15).

Both apply to storage as much as to networking; flagging them here because
networking is where a missing delegation will first be *noticed*, since virtio-blk
can limp along on polling during early boot while virtio-net cannot.

Timer: with a host-side SBI, `sbi_set_timer` does not need the usual M-mode
trampoline. Program the CLINT compare and, when it fires, set `mip.STIP` directly
from C rather than emulating the BBL dance of setting STIP and clearing `mie.MTIE`.
Fewer moving parts, and it keeps `MIP_MEIP`/`MIP_SEIP` from the PLIC on a completely
separate path from the timer.

### 4.3 Devicetree nodes

TinyEMU's own generator emits exactly these (`riscv_machine.c:682-708`); the
addresses come from `riscv_machine.c:63-73`. Since our machine layer keeps the
existing offline-`dtc` workflow (`06-guest-net.md §6`), hand-write them:

```dts
/ {
    soc {
        #address-cells = <2>;
        #size-cells    = <2>;
        compatible     = "simple-bus";
        ranges;

        plic: plic@40100000 {
            #interrupt-cells = <1>;
            interrupt-controller;
            compatible  = "riscv,plic0";
            riscv,ndev  = <31>;
            reg         = <0x00 0x40100000 0x00 0x00400000>;
            /* S ext irq = 9, M ext irq = 11 */
            interrupts-extended = <&cpu0_intc 9 &cpu0_intc 11>;
            phandle     = <2>;
        };

        virtio_net: virtio@40010000 {
            compatible          = "virtio,mmio";
            reg                 = <0x00 0x40010000 0x00 0x1000>;
            interrupts-extended = <&plic 1>;
        };
    };
};
```

With virtio-blk in the picture, block takes slot 0 (IRQ 1) and net takes slot 1
(`virtio@40011000`, IRQ 2), matching TinyEMU's own allocation order in
`riscv_machine.c:884-916`.

Four things to get right:

- **`riscv,ndev` must be >= the highest IRQ used.** TinyEMU uses 31.
- **IRQ numbering is 1-based and the PLIC subtracts one internally**
  (`plic_set_irq()` at `riscv_machine.c:303` does `mask = 1 << (irq_num - 1)`).
  Node *n* on the bus gets `VIRTIO_IRQ + n` = `1 + n`.
- **Keep the S-mode entry FIRST in the PLIC's `interrupts-extended`.** This one will
  bite. Linux's `irq-sifive-plic.c` derives the PLIC context index from the *array
  position* of the matching entry and computes the claim register as
  `0x200000 + i * 0x1000`. TinyEMU implements **only context 0**
  (`PLIC_HART_BASE 0x200000`, `riscv_machine.c:253-256`). Listing M-ext (11) before
  S-ext (9) makes Linux use `i = 1` → `0x201000`, which TinyEMU's `default:` arm
  reads as 0 — the claim always returns "no interrupt" and every virtio device
  appears dead. `<&cpu0_intc 9 &cpu0_intc 11>` is the correct order and is what
  TinyEMU emits at `riscv_machine.c:692-696`.
- The DTB grows by roughly 300 bytes per node. `06-guest-net.md §6` already found
  that `dtc -S 1536` overflows once you add nodes; use `-S 2048` or larger.

### 4.4 One emulator-side tuning change worth making

```c
/* virtio.c:96 */
#define MAX_QUEUE_NUM 16
```

16 descriptors is a very short RX ring — about 24 KB in flight. The ring memory is
allocated by the *guest*, so raising this costs the host nothing but a larger
advertised `VIRTIO_MMIO_QUEUE_NUM_MAX`. **Bump it to 128.** It must stay a power of
two: `virtio_net_write_packet()` masks with `qs->num - 1` (`virtio.c:1206`).

---

## 5. Q4 — Does `pip` actually need this? No. And networking does not fix the hard part.

This is the section I would read first if I were deciding.

### 5.1 VERIFIED: `pip install adafruit-blinka` will fail on riscv32 even with a network

`Adafruit-Blinka` 9.2.0's `requires_dist`, from the PyPI JSON API today:

```
Adafruit-PlatformDetect>=3.89.1
Adafruit-PureIO>=1.1.7
binho-host-adapter>=0.1.6
pyftdi>=0.40.0
adafruit-circuitpython-typing
sysv_ipc>=1.1.0; sys_platform == "linux" and platform_machine != "mips"
toml>=0.10.2; python_version < "3.11"
```

`sysv_ipc` is a **C extension**, and it is an unconditional dependency on Linux.
Across all 69 files ever published for `sysv_ipc`, the only wheel platforms are:

```
macosx_10_6/10_9/10_13/10_15_x86_64, macosx_11_0_arm64,
manylinux2014_x86_64, manylinux2014_aarch64,
musllinux_1_2_x86_64, musllinux_1_2_aarch64
```

**Zero riscv wheels, ever.** So on a riscv32 guest, `pip install adafruit-blinka`
downloads the `sysv_ipc` sdist and tries to compile it — which needs `gcc`, Python
headers and setuptools *inside* a 64 MB emulated guest. That is a much bigger ask
than the networking is.

### 5.2 VERIFIED: Blinka does not actually use `sysv_ipc` on a generic Linux board

Unzipped `adafruit_blinka-9.2.0-py3-none-any.whl` and grepped every `.py`. The
string `sysv_ipc` appears in exactly three files:

```
adafruit_blinka/microcontroller/amlogic/a311d/pulseio/PulseIn.py
adafruit_blinka/microcontroller/amlogic/meson_g12_common/pulseio/PulseIn.py
adafruit_blinka/microcontroller/bcm283x/pulseio/PulseIn.py
```

All three are `PulseIn` implementations for Amlogic and Raspberry Pi. Nothing on the
`generic_linux` path imports it. Same check for the other heavyweight deps:
`pyftdi` appears only under `microcontroller/ftdi_mpsse/`, and `binho` appears in
`busio.py`/`pwmio.py` only as the *detector attribute* `detector.board.binho_nova`
— never as an import.

So the actual runtime dependency set for our board is three pure-Python
`py3-none-any` wheels: `adafruit_blinka`, `Adafruit_PureIO`,
`adafruit_platformdetect` (plus `adafruit-circuitpython-typing` for annotations).

### 5.3 Therefore: the offline install is not a fallback, it is the better call

On the Mac:

```sh
pip download --no-deps --only-binary=:all: \
    adafruit-blinka Adafruit-PureIO Adafruit-PlatformDetect \
    adafruit-circuitpython-typing -d wheelhouse/
```

All four resolve to `py3-none-any`, so the Mac's arch is irrelevant. Drop
`wheelhouse/` into the Buildroot rootfs overlay, then in the guest:

```sh
pip install --no-index --no-deps --find-links=/wheels \
    adafruit-blinka Adafruit-PureIO Adafruit-PlatformDetect
```

or skip pip entirely and unzip the four wheels straight into
`/usr/lib/python3.x/site-packages/` in the overlay. No network, no compiler, no pip
on the target at all.

**`--no-deps` is load-bearing.** Without it pip pulls the `sysv_ipc` sdist and the
install fails regardless of whether networking works.

### 5.4 When guest networking *is* the better call

- Iterating on Python code without reflashing the OSPI image.
- Installing anything not known in advance — a driver library for a sensor someone
  just plugged in.
- `scp`/`rsync` of files off the board.
- It is the thing that makes the board feel like an SBC rather than a demo.

Those are real, and they are why option #1 is still worth building. They are just
not "Blinka needs it".

---

## 6. Q5 — SSH

Once the guest has an address this is genuinely trivial, with three caveats worth
writing down now.

- **Use dropbear, not openssh.** `BR2_PACKAGE_DROPBEAR=y`. Roughly 200 KB static
  against several MB for openssh, and it is what every constrained Buildroot target
  uses. Openssh only earns its size if you need sftp-server or certificate auth.
- **Pre-generate the host keys on the build host and ship them in the rootfs
  overlay** (`/etc/dropbear/dropbear_ed25519_host_key`). First-boot keygen on an
  emulated rv32 core running at (estimated) tens of MIPS is a bad first impression,
  and RSA-2048 keygen especially so. Use ed25519: generation and signing are both
  cheap.
- **Entropy.** (INFERRED) There is no virtio-rng in the machine and no hardware RNG
  in the guest. On Linux 6.x `/dev/urandom` never blocks, so dropbear will start —
  but the entropy it starts with is weak. For a lab board on a private segment that
  is acceptable. If it matters, `virtio_rng_init` is not in TinyEMU but the RA8D1
  has a TRNG, so a fifth paravirt MMIO register on the existing `0x11200000` bridge
  (`05-paravirt-io.md §2`) would be a ~30-line fix and is the cheapest good answer.
  Do not add a virtio-rng device for this.

SSH plus Blinka being the two things that justify the port is the right framing, and
it cuts cleanly: **SSH genuinely needs §2's guest network. Blinka does not.** So the
honest ordering is offline Blinka first (it is small and it proves the userspace),
guest network second (it unlocks SSH and everything after).

---

## 7. Implementation steps for the recommendation

Ordered so each step is independently testable.

### 7.1 Prove the LAN tolerates a second MAC (30 min, no code)

Put a second device on the Mac's Internet Sharing segment; confirm it gets its own
DHCP lease alongside the board's `192.168.2.3`. If this fails, stop and switch to
option #2 before writing anything.

### 7.2 Emulator side: PLIC + virtio-net (~80 new lines + copied code)

1. Copy `plic_read`/`plic_write`/`plic_set_irq`/`plic_update_mip` from
   `riscv_machine.c:241-315` into the machine layer. Register at `0x40100000`.
2. Take `virtio.c` and `iomem.c` as-is. `--gc-sections` drops the 9p, block, input
   and PCI devices we do not instantiate. (`sv32-mmu.md §5a` already plans to take
   `iomem.c`.)
3. Bump `MAX_QUEUE_NUM` 16 → 128 (`virtio.c:96`).
4. Write the `EthernetDevice` vtable in `platform_zephyr.c`:
   - `write_packet(es, buf, len)` — guest → wire. Called from the emulator thread.
     Allocate a `net_pkt` on the guest `net_if` and `net_recv_data()` it, or push to
     a k_fifo drained by a TX work item.
   - Wire → guest: the Zephyr driver's `send()` must **not** touch virtio state
     directly — it runs on a Zephyr net TX thread while the emulator thread is
     stepping. Push the frame onto a bounded k_fifo; drain it at the top of the
     emulator step loop by calling `device_can_write_packet()` then
     `device_write_packet()`. Drop on full — that is what a NIC does.
5. Add the two DTB nodes from §4.2, rebuild with `dtc -S 2048`.

### 7.3 Zephyr: patch the RA driver for promiscuous mode (~20 lines)

Pattern from `eth_xmc4xxx.c:1141-1154`:

```c
/* drivers/ethernet/eth_renesas_ra.c */
static enum ethernet_hw_caps renesas_ra_eth_get_capabilities(...)
{
        return ETHERNET_LINK_10BASE | ETHERNET_LINK_100BASE
#if defined(CONFIG_NET_PROMISCUOUS_MODE)
             | ETHERNET_PROMISC_MODE
#endif
             ;
}

#if defined(CONFIG_NET_PROMISCUOUS_MODE)
static int renesas_ra_eth_set_config(const struct device *dev,
                                     enum ethernet_config_type type,
                                     const struct ethernet_config *config)
{
        if (type != ETHERNET_CONFIG_TYPE_PROMISC_MODE) {
                return -ENOTSUP;
        }
        R_ETHERC0->ECMR_b.PRM = config->promisc_mode ? 1U : 0U;
        return 0;
}
#endif

static const struct ethernet_api api_funcs = {
        ...
        .set_config = renesas_ra_eth_set_config,     /* new */
};
```

`<soc.h>` is already included (`eth_renesas_ra.c:17`), so `R_ETHERC0` is in scope.

**Known wrinkle:** FSP rewrites `ECMR.PRM` from `p_ether_cfg->promiscuous` on every
link establishment (`r_ether.c:1790`, inside `ether_config_ethernet()`). A PHY
re-link — cable unplug, autoneg glitch — would silently clear promiscuous mode and
the bridge would go deaf without an error. The durable fix is to drop `const` from
`g_ether0_cfg` (`eth_renesas_ra.c:130`) and set `.promiscuous` there as well as
poking the register. Do both.

This tree is already a working fork (HEAD is a local `drivers/display/renesas_ra`
commit), so an in-tree driver patch is the normal move here — no out-of-tree copy
needed. Worth upstreaming later; it is a clean gap-fill.

### 7.4 Zephyr: the guest-facing Ethernet interface (~200 lines)

A device with no DT node, registered via `ETH_NET_DEVICE_INIT()`
(`include/zephyr/net/ethernet.h:1122`), MTU `NET_ETH_MTU` (1500,
`ethernet.h:118`).

- `get_capabilities()` returns `ETHERNET_LINK_100BASE | ETHERNET_PROMISC_MODE`. We
  own this driver, so the capability that blocked the RA driver is free here.
- `set_config()` for `ETHERNET_CONFIG_TYPE_PROMISC_MODE` is a no-op returning 0 —
  an emulated NIC is always promiscuous.
- `send(dev, pkt)` → linearize into a 1514-byte scratch buffer, push to the
  emulator's inbound k_fifo (§7.2 step 4).
- Guest → wire: from `write_packet()`, `net_pkt_alloc_with_buffer()` on this iface,
  copy, `net_recv_data()`. The bridge picks it up in `ethernet_recv()`
  (`ethernet.c:288`) and forwards it to `eth0`.
- MAC: `76:90:50:b0:5d:e9` — the board MAC with the locally-administered bit set.
  Feed the same six bytes to `virtio_net_init()` via `es->mac_addr`, which copies
  them into virtio config space (`virtio.c:1246`) where `VIRTIO_NET_F_MAC` exposes
  them to the guest.
- Carrier: `net_eth_carrier_on(iface)` once the emulator is running.

### 7.5 Zephyr: config changes to `rvlinux/prj.conf`

```conf
# --- bridge ---
CONFIG_NET_ETHERNET_BRIDGE=y
CONFIG_NET_ETHERNET_BRIDGE_COUNT=1
CONFIG_NET_ETHERNET_BRIDGE_ETH_INTERFACE_COUNT=2
CONFIG_NET_ETHERNET_BRIDGE_FDB=y          # avoids flooding every frame both ways
CONFIG_NET_ETHERNET_BRIDGE_UNIQUE_MAC=y   # stable bridge MAC across reboots (needs HWINFO)
CONFIG_HWINFO=y
# selected automatically, stated for clarity:
CONFIG_NET_PROMISCUOUS_MODE=y
CONFIG_NET_L2_VIRTUAL=y
CONFIG_NET_L2_ETHERNET_MGMT=y
CONFIG_NET_MGMT=y
CONFIG_NET_MGMT_EVENT=y
CONFIG_NET_MGMT_EVENT_STACK_SIZE=2048

# --- three interfaces now: eth0, guest0, bridge0 ---
CONFIG_NET_IF_MAX_IPV4_COUNT=3

# --- buffers: bridging clones every forwarded frame (bridge_input.c:81),
#     and the 128 B default fragment means 12 net_bufs per full frame. ---
CONFIG_NET_BUF_DATA_SIZE=1536      # one buf per frame, no chain walking
CONFIG_NET_PKT_RX_COUNT=32
CONFIG_NET_PKT_TX_COUNT=32
CONFIG_NET_BUF_RX_COUNT=24
CONFIG_NET_BUF_TX_COUNT=24
```

At 1536 B/buf that is 24 × 1536 × 2 ≈ **72 KB** of internal SRAM for the data pools.
The RA8D1 has 1 MB on-chip SRAM and the guest's 64 MB lives in SDRAM, so they do not
compete. Choosing `NET_BUF_DATA_SIZE=1536` over the 128 B default also removes a
12-fragment chain walk from every packet, which matters when the CPU is also
interpreting RISC-V.

Recommend keeping `CONFIG_NET_ETHERNET_BRIDGE_SHELL=y` and `CONFIG_NET_SHELL=y`
during bring-up; drop them after.

### 7.6 Zephyr: wire it up in `main()` (~40 lines)

Copy `samples/net/ethernet/bridge/src/main.c` verbatim in shape:

```c
net_eth_bridge_foreach(bridge_find_cb, &u);      /* finds bridge0 */
net_if_foreach(bridge_add_iface_cb, &u);         /* eth_bridge_iface_add() both */
net_if_up(u.bridge);
/* on NET_EVENT_IF_UP for a bridged iface: net_dhcpv4_restart(bridge) */
```

**Move the telnet console server from the `eth0` address to the bridge address.**
After bridging, `eth0` no longer holds an IP. `07-telnet-bridge.md`'s listener binds
`INADDR_ANY` if written the obvious way, in which case nothing changes — check it.

### 7.7 Guest: kernel config + DTB (§4), then

```sh
udhcpc -i eth0        # or ip=dhcp on the bootargs with CONFIG_IP_PNP_DHCP
ping 192.168.2.1
```

---

## 8. Effort estimates, honestly

Lines are *new or adapted* code, excluding files taken from TinyEMU unmodified.

| Option | Emulator | Zephyr | Guest kernel | Realistic effort | Risk |
|---|---:|---:|---:|---|---|
| **#1 virtio-net + L2 bridge** | ~80 | ~260 | config + DTB only | **2-3 days** | Medium-low. Every piece exists; the assembly is new. Biggest unknown is §3.3 (second MAC on the shared segment) and whether promiscuous survives a re-link. |
| #2 virtio-net + socket NAT | ~80 | ~550 | same | 5-7 days | Medium-high. `NET_IPV4_NAT` is EXPERIMENTAL; you own the DHCP/ARP responder. |
| #3 SLIP over a 2nd UART | ~120 | ~300 | +`CONFIG_SLIP`, +8250 port | 3-4 days | Medium. Needs a fake `uart_pipe`. Strictly worse than #1. |
| #4 paravirt socket bridge | ~60 | ~200 | **out-of-tree kernel patch** | 2+ weeks | High. Carried forever in Buildroot. Do not. |
| **Offline wheelhouse (no network)** | 0 | 0 | 0 | **2-3 hours** | Very low. Four `py3-none-any` wheels into the rootfs overlay. |

Sub-tasks inside #1, so it can be parallelised or stopped partway:

| Step | Effort | Independently testable? |
|---|---|---|
| §7.1 second-MAC check | 30 min | yes, no code |
| §7.3 RA promiscuous patch | 1-2 h | yes — `net_promisc_mode_on(eth0)` returns 0 instead of -ENOTSUP |
| §7.4 guest `net_if` (loopback stub first) | 4-6 h | yes — bridge two Zephyr ifaces with a stub that echoes |
| §7.2 PLIC + virtio-net in the machine layer | 1 day | yes — guest boots and `ip link` shows `eth0` |
| §7.5/§7.6 bridge config + wiring | 3-4 h | yes — `net iface` in the shell shows bridge0 with a lease |
| guest DHCP end to end | 2-4 h | the payoff |

---

## 9. Fallback if networking proves hard

Stated in priority order, each usable on its own:

1. **Offline wheelhouse (§5.3).** Delivers Blinka completely. Do this regardless.
2. **Keep the telnet console** (`07-telnet-bridge.md`). Already designed, already
   satisfies "remote login" for a human, needs no guest network. Not SSH, and not
   encrypted, but on a lab segment behind a Mac it is fine.
3. **virtio-block or virtio-9p instead of virtio-net for file movement.** TinyEMU
   has both (`virtio_block_init`, `virtio_9p_init`, wired at
   `riscv_machine.c:907-923`). A block device backed by the OSPI image gives the
   guest a writable filesystem you can rebuild from the Mac. This gets you "put new
   files on the guest" without any of the L2 bridging work — cheaper than #2 in §2
   and probably the right consolation prize.
4. **NAT at the socket layer (option #2)** if bridging is blocked specifically by
   the upstream segment refusing a second MAC.

The thing *not* to do is spend a week on option #4 to avoid a driver patch that is
twelve lines.

---

## 10. Open questions

- **§3.3, untested:** does the Mac's Internet Sharing `bootpd` hand out a second
  lease on the shared segment? Cheapest possible de-risk; do it first.
- **§7.3, untested:** does `ECMR.PRM` survive a PHY re-link once FSP re-runs
  `ether_config_ethernet()`? Mitigation is in the step; verify by unplugging the
  cable.
- **Emulator throughput on the actual M85 is unmeasured.** The 464 MIPS in
  `sv32-mmu.md §5b` is a Mac-container number. Everything in §2.5 is extrapolation.
- **Promiscuous mode means every frame on the segment enters Zephyr's RX thread and
  gets cloned.** On a quiet point-to-point Internet Sharing segment that is nothing;
  on a busy office LAN it would compete with the emulator for the M85. Worth a
  `net stats` check once running.
- **Does 64 MB SDRAM still fit** an MMU kernel + glibc + virtio rings + page tables
  + Python? Already open in `sv32-mmu.md §6`; virtio-net adds only the ring memory
  (128 descriptors × 2 queues ≈ a few KB) plus the socket buffers.

## 11. Progress log

| Date | Entry |
|---|---|
| 2026-08-07 (rev 2) | Re-weighted after three inputs from the lead. (a) Storage is virtio-blk, so the PLIC is already paid for and `06-guest-net.md`'s entire objection to virtio-net is void. (b) SBI is host-side in the machine layer, not BBL — added §4.2 on the `mideleg = 0x222` / `medeleg = 0xb1ff` requirement and the PLIC context-ordering trap, both of which present as "virtio interrupts never arrive". (c) **Measured virtio-net's marginal flash cost: 556 B** on top of a block-only build (5 722 vs 5 166 text, Cortex-M85, gc-sections; block-only reproduces the other agent's number exactly). Recommendation unchanged and strengthened. |
| 2026-08-07 | Designed. Read TinyEMU `virtio.c`/`riscv_machine.c` (virtio-net is v2/modern, 130 lines, host vtable already factored; PLIC is 90 lines and Linux 6.8 binds `riscv,plic0`). Read Zephyr 4.4.99: `CONFIG_NET_ETHERNET_BRIDGE` exists and is the right mechanism, but `eth_bridge_iface_add()` gates on `ETHERNET_PROMISC_MODE`, which the Renesas RA driver does not report and has no `.set_config` for — 12-line patch, hardware and FSP both support `ECMR.PRM`. Verified guest kernel symbols against `linux-6.8-rc1` in the `br` container. **Separately established that `pip install adafruit-blinka` cannot succeed on riscv32 at all** (hard `sysv_ipc` C-extension dep, zero riscv wheels ever published) while Blinka never imports it outside the Amlogic/BCM283x `PulseIn` paths — so the offline wheelhouse is the correct primary answer for Blinka and networking is for SSH. No code written. |
