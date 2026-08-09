# Guest networking: virtio-net into the emulated guest, bridged to real Ethernet

Working history for the guest-net project. The design was settled in
`08-guest-net-mmu.md` (read that first — it prices every alternative); this
file records the implementation as it lands. Companion projects with their own
files: `ssh.md` (dropbear, rides on this), `../../ra8d1-tinyemu/notes/pusher.md`
(image pusher, independent).

## Plan of record (from 08-guest-net-mmu.md option #1)

Guest sees a virtio-net NIC; the Zephyr host bridges its frames onto the real
Ethernet at L2, so the guest gets its own MAC and its own DHCP lease on the
LAN. Four pieces:

| piece | where | status |
|---|---|---|
| Zephyr eth driver: promiscuous mode | zephyr fork, `eth_renesas_ra.c` | **done, committed `8ca5eb88530`** |
| Guest kernel: `CONFIG_VIRTIO_NET` | `/br/mmu-trim` tree + trim config in repo | delegated to the dropbear/QEMU work |
| Emulator: virtio-net device | `ra8d1-tinyemu/emu/` | scoped, below |
| Zephyr host: L2 plumbing to eth0 | `ra8d1-tinyemu/src/` | after the pusher port lands (same tree) |

## 2026-08-09: eth_renesas_ra promiscuous patch (issue mikeysklar/zephyr#10)

`eth_bridge_iface_add()` requires `ETHERNET_PROMISC_MODE` in the driver's
capabilities and the RA driver advertised none and had no `.set_config`. The
patch adds both, gated on `CONFIG_NET_PROMISCUOUS_MODE`, modeled on
`eth_xmc4xxx.c`.

The part that was NOT in the issue's proposed fix and matters: the FSP
re-applies `ECMR.PRM` from `ether_cfg_t.promiscuous` on **every link
establishment** (`r_ether.c:1790`), so writing the register alone is undone by
any unplug or autoneg cycle — the bridge would go deaf with no error, on a
timescale of days. The committed handler therefore writes both the (formerly
`const`) `g_ether0_cfg.promiscuous` field and, when the FSP ctrl is open, the
live register.

Measured: ek_ra8d1 app with `CONFIG_NET_PROMISCUOUS_MODE=y` builds at
173,544 B flash, +696 B over the same app without.

## Emulator side, scoped 2026-08-09

`emu/rv_virtio.c` is a 563-line block-only rewrite in this app's own style —
it is NOT upstream TinyEMU's `virtio.c`, so the "virtio-net is 130 lines on
top of the factored vtable" figure from 08-guest-net-mmu.md §1.1 does not
apply directly. The net device gets written fresh in rv_virtio.c's idiom:

- Second virtio-mmio window. Block is at `RV_VIRTIO_BASE 0x10001000`, PLIC
  source 2 (`rv_machine.h:37,45`). Net goes at `0x10002000`, source 3, with a
  matching FDT node emitted by `rv_fdt.c`.
- `VIRTIO_ID_NET = 1`, two queues (0 = RX, 1 = TX), the 12-byte
  `virtio_net_hdr` (no offloads, all zeros), feature bit `VIRTIO_NET_F_MAC`
  so the guest takes the MAC we hand it rather than randomizing one.
- Backend via two new platform hooks, keeping emu/ portable:
  `plat_net_send(buf,len)` and `plat_net_recv(buf,cap) -> len|-1`, no-op
  stubs when networking is off, same pattern as the console hooks.
- Guest MAC: derive from the board MAC with a flipped locally-administered
  bit, so the two are related but distinct on the segment.

## Zephyr host side, to implement after the pusher lands

Follow 08-guest-net-mmu.md §7. Note for the implementation pass: evaluate
Zephyr's `CONFIG_NET_ETHERNET_BRIDGE` (needs a second net_if for the guest
side) against the lighter promiscuous + frame-filter approach (deliver frames
matching guest MAC or broadcast/multicast to the guest, hand guest TX frames
straight to `net_if`); pick whichever needs less machinery *in this app* and
record why here. The bridge subsystem's iface requirement is why the driver
patch exists either way.

## 2026-08-09: upstream research sweep (5 parallel scans), what it changed

Run while the pusher and dropbear builds were in flight. Findings that
changed code or plans, with the noise omitted:

- **Upstream Zephyr has no promiscuous support for this driver** — checked
  against current `main`: `api_funcs` still has only init/caps/get_phy/send.
  Our fork patch is the only implementation. (Recon only; nothing goes
  upstream from here per the collaboration norms.)
- **Cherry-picked upstream PR #115734** into the fork as `1b5f1c41b1d`:
  duplicate `R_ETHER_Write()` per TX, TX `K_NO_WAIT` silently dropping under
  backpressure, RX draining one frame per interrupt credit. All three sit in
  the bridged-guest data path; the RX one especially — promiscuous mode turns
  the whole segment's broadcast domain into our RX load.
- **virtio-mmio device validation** against Linux 6.1 driver behavior:
  `num_buffers=1` mandatory without MRG_RXBUF (we do), missing
  `VIRTIO_NET_F_STATUS` just means the link reads permanently up (accepted),
  and config space should tolerate 8/16/32-bit reads even though Linux reads
  the MAC bytewise (device updated to serve all three widths).
- **Semper S28HL512T ECC**: 16-byte internal ECC units; re-programming a unit
  without erase is prohibited by Infineon and corrupts ECC state. This is the
  leading suspect for the historical intermittent single-64KB verify miss,
  relayed to the pusher implementation (its retry path must erase before any
  rewrite) and recorded on fork issue #11. No upstream fix for the
  PAGE_SIZE_BYTE=64 throughput hardcode either.
- **Blinka `sysv_ipc` recon** (for the report to PT/limor's team, not for a
  PR): the mips exclusion precedent is commit `599f21f` (Jan 2020, Onion
  Omega, no on-target gcc — the identical situation to riscv32). But upstream
  direction moved in PR #1114 (Jul 2026) to runtime dependency checking, so
  the fix most likely to be accepted is a try/except runtime guard in the
  PulseIn paths via their existing `importing.py` pattern, not another
  platform_machine exclusion.

## Risks / open questions

- Second MAC on the Mac Internet Sharing segment: §7.1 of the design says
  prove the LAN tolerates it before writing code (30 min, no code). Still
  unproven.
- Promiscuous RX load: every LAN frame now reaches the M85. On a quiet lab
  segment fine; worth watching the eth RX thread priority (2) vs emulator (10).
- The guest kernel trim must gain virtio-net without regressing the 8 MB slot
  fit (was 6,543,840 B with ~1.8 MB headroom).
