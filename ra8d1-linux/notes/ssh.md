# SSH in the rv32 Linux guest — dropbear

Date: 2026-08-09. Implements `08-guest-net-mmu.md` §6 (Q5). Container + QEMU
only; nothing here has run on the board.

Result up front: **`ssh root@guest` works under QEMU.** New kernel with
`CONFIG_VIRTIO_NET=y`, dropbear 2022.83 in the rootfs with a pre-generated
ed25519 host key, starting on a genuinely read-only root.

---

## 1. What is on the target now

| item | path in `/br/blinkabuild/tree` | size |
|---|---|---:|
| dropbear multi-call binary | `usr/sbin/dropbear` | 281,628 |
| symlinks to it | `usr/bin/{dropbearkey,dropbearconvert,scp,dbclient,ssh}` | 16 each |
| ed25519 host key | `etc/dropbear/dropbear_ed25519_host_key` | 83 |
| start script | `etc/init.d/S50dropbear` | 1,964 |
| tunables | `etc/default/dropbear` | 420 |
| DHCP script | `etc/init.d/S45eth0` | 864 |

`dropbear` is `MULTI=1`, so the server, the key tools, `scp` and the `dbclient`
client are all one binary. Enabling the client cost nothing but symlinks.

---

## 2. Building it

Dropbear was **not** prebuilt anywhere in the tree — `find /br/mmu /br/buildroot
-iname 'dropbear*'` returned only the Buildroot package recipe, and
`/br/buildroot/dl/` had no tarball. So it was built from source:

```sh
# /br/mmu/.config, four lines, saved as .config.pre-dropbear first
BR2_PACKAGE_DROPBEAR=y
BR2_PACKAGE_DROPBEAR_CLIENT=y
BR2_PACKAGE_DROPBEAR_DISABLE_REVERSEDNS=y
BR2_PACKAGE_DROPBEAR_SMALL=y

cd /br/buildroot && make O=/br/mmu olddefconfig
cd /br/mmu       && make dropbear          # downloads dropbear-2022.83.tar.bz2
```

Why those options:

- **`_SMALL=y`** builds against the *bundled* libtomcrypt/libtommath and
  `--disable-zlib`. Without it the package pulls in `libtomcrypt` as a new
  Buildroot target package. Bundled costs nothing here because the multi-binary
  only links the objects it uses.
- **`_DISABLE_REVERSEDNS=y`** is the one that matters at runtime. The default is
  `DO_HOST_LOOKUP 1`, and a reverse lookup for a client address on a segment
  with no working PTR path stalls every connection for the resolver timeout.
- **`_CLIENT=y`** adds `dbclient`/`ssh` to the same binary.
- `_WTMP`, `_LASTLOG`, `_LEGACY_CRYPTO` all left off. Legacy crypto would add
  3DES/CBC/SHA1-96/DH-group1/DSS; no modern client needs any of it.

### Build options, verified rather than assumed

The lead flagged five traps from an upstream scan. Each is checked here against
this binary, not against the docs.

| trap | status | evidence |
|---|---|---|
| host-key flag name differs by version | `-r` is correct for 2022.83 | `svr-runopts.c` takes `-r`; `-d`/`-y` were the pre-2013 DSS/RSA-specific flags. Verified working in §6. |
| pid file needs a writable dir | `/var/run` -> `/run`, `tmpfs` per `/etc/fstab` | `tmpfs on /run type tmpfs (rw,nosuid,nodev,...)` in the read-only boot. No `-P /tmp/...` fallback needed. |
| blank-password auth is off by default | `-B` passed | §4 |
| lastlog/wtmp writes | compiled out | below |
| zlib compression costs CPU | compiled out, never offered | below |

**lastlog/wtmp.** `configure` ran with `--disable-wtmp --disable-lastlog`
(Buildroot adds both because `BR2_PACKAGE_DROPBEAR_WTMP`/`_LASTLOG` are unset).
`configure` still *probes* successfully — `checking for lastlog.h... yes`,
`checking for logwtmp... yes` — so the log is misleading; what settles it is
that `logwtmp` is **not** an undefined dynamic symbol in the shipped binary:

```
$ riscv32-linux-readelf --dyn-syms usr/sbin/dropbear | grep -iE 'logwtmp|lastlog|crypt'
     3: 00000000     0 FUNC    GLOBAL DEFAULT  UND crypt@GLIBC_2.33 (3)
```

`crypt` is there (password auth is live), the login-record functions are not.
Confirmed at runtime: after ~15 logins, `/var/log/lastlog` and `/var/log/wtmp`
do not exist. Note this would not have failed anyway — `/var/log` is a symlink
to `../tmp`, i.e. tmpfs — but not writing them at all is better.

**Compression.** `_SMALL=y` implies `--disable-zlib`, and `config.h` has
`#define DISABLE_ZLIB 1`. There is no per-connection flag to worry about
because the algorithm is never offered:

```
debug1: kex: server->client cipher: chacha20-poly1305@openssh.com MAC: <implicit> compression: none
debug1: kex: client->server cipher: chacha20-poly1305@openssh.com MAC: <implicit> compression: none
```

`compression: none` on both directions is the desired outcome, not a fallback.

**One `localoptions.h` line that looks alarming and is not.** The package writes

```
#if !HAVE_CRYPT
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#endif
```

`config.h` has `HAVE_CRYPT 1` (crypt found in `-lcrypt`), so the branch is dead
and password auth is compiled in. Grepping `localoptions.h` without the
surrounding `#if` reads as "password auth disabled", which it is not.

### Stripping

Buildroot leaves the target binary unstripped (it strips at rootfs-assembly
time, which we bypass by hand-populating `/br/blinkabuild/tree`), so it was
stripped explicitly:

```sh
riscv32-linux-strip --strip-unneeded  # 359,228 -> 281,628, -21.6%
```

Verified riscv32, not host-arch, per BUILD.md gotcha 16:

```
ELF 32-bit LSB pie executable, UCB RISC-V, double-float ABI,
  dynamically linked, interpreter /lib/ld-linux-riscv32-ilp32d.so.1, stripped
NEEDED  libcrypt.so.1
NEEDED  libc.so.6
```

`libcrypt.so.1` is already in `tree/lib/`. No new library shipped.

---

## 3. Host key

Generated **on the container host**, once, at build time. The guest never runs
keygen: `08-guest-net-mmu.md` §6 is right that first-boot keygen at tens of MIPS
is a bad first impression, and it is impossible anyway on a read-only root.

There is no `dropbearkey` in the Buildroot host tools and no user-mode
`qemu-riscv32` to run the target one, so a native `dropbearkey` was built from
the same tarball Buildroot downloaded:

```sh
mkdir -p /br/hostdb && cd /br/hostdb
tar xjf /br/buildroot/dl/dropbear/dropbear-2022.83.tar.bz2
cd dropbear-2022.83
./configure --disable-zlib --enable-bundled-libtom
make PROGRAMS="dropbearkey dropbearconvert" -j8

/br/hostdb/dropbear-2022.83/dropbearkey -t ed25519 \
    -f /br/blinkabuild/tree/etc/dropbear/dropbear_ed25519_host_key
chmod 600 /br/blinkabuild/tree/etc/dropbear/dropbear_ed25519_host_key
```

Same source, same version, so the key format is exactly what the target binary
expects — no `dropbearconvert` step needed. The `ssh-keygen` + `dropbearconvert`
route would also have worked and is the fallback if the native build ever breaks.

The key shipped in this image:

```
ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIMCU7LYt/lQf3oC5NwO5qTh+Afyxmr5Syzx6eGChrMGg
SHA256:mw2uYM2aswBxqDlLuAsbfERfovF8dwRZbTTqkgh25aw
```

`ssh-keyscan` against the booted guest returns the same fingerprint, so the
shipped key is the one actually in use and nothing regenerated it (§6).

Only ed25519 is shipped. RSA/ECDSA host keys are deliberately absent: dropbear
is started with `-r <ed25519 key>` and no `-R`, so it offers exactly one host
key type and never attempts to write a new one.

---

## 4. Security posture — read this before putting the board on a real network

**The image as built accepts root over SSH with no credential at all.**

- `/etc/shadow` has `root::::::::` — the password field is empty. That was
  already true; the console gets a shell via `/bin/login -f root` in
  `/etc/inittab`, which skips auth entirely.
- `S50dropbear` passes **`-B`**, which is dropbear's `allowblankpass`. With it,
  the SSH `none` auth method succeeds for an account whose password field is
  empty (`svr-auth.c:126-137`). OpenSSH clients send `none` first automatically,
  so `ssh root@guest` connects with no prompt.
- The guest log says so out loud on every login:
  `dropbear[136]: Auth succeeded with blank password for 'root' from 10.0.2.2`

This is the lab-board tradeoff `08-guest-net-mmu.md` §6 anticipated, taken
one step further than "weak entropy". It is fine behind the Mac's Internet
Sharing segment. It is **not** fine on an office LAN, and once the Zephyr L2
bridge lands (§8) the guest is a peer on whatever wire the board is plugged
into, with a real DHCP lease. Revisit it then.

Turning it off is two lines, and the mechanism is already shipped —
`/etc/default/dropbear` is sourced by the init script and carries the
replacement `DROPBEAR_ARGS` commented out. Uncomment it (drops `-B`) and put a
hash in `/etc/shadow`:

```sh
openssl passwd -6 -salt ra8d1lab blinka
# $6$ra8d1lab$inNouD00jJRcbaGNSFHMz07CukDXSa2jwvG2tp6PYdXV7VKZnLzoC8WYyppB2plKdAhfp3HmQeByzdbmNqbwu/
```

The target glibc has SHA-512 crypt, and dropbear is linked against
`libcrypt.so.1`, so `$6$` hashes work. Pubkey-only auth (`-s`, plus an
`authorized_keys` baked into `/root/.ssh/`) is the better answer long-term and
costs nothing extra at runtime; it was not done here because it needs a key pair
decision that belongs to whoever owns the board.

### Entropy

As predicted in §6 of the design note, there is no virtio-rng and no hardware
RNG in the guest, and `/dev/urandom` on 6.1 never blocks, so dropbear starts
regardless. Under QEMU the kernel prints `random: crng init done` at
`[0.000000]` because QEMU seeds the DTB `rng-seed` property; **TinyEMU does not
emit that property**, so on the board the pool will be seeded from far less.

Also observed in passing and relevant here: on a read-only root, `S01seedrng`
fails with `seedrng: can't create directory '/var/lib/seedrng': Read-only file
system`, so the save/restore-a-seed-across-boots mechanism does nothing on the
shipped image either. Both point at the same fix already costed in the design
note: a TRNG read on the existing paravirt MMIO bridge at `0x11200000`
(`05-paravirt-io.md` §2), ~30 lines, feeding `RNDADDENTROPY` early in `rcS`.
Not done, not blocking.

---

## 5. Startup, and the read-only-root problem

The Buildroot stock `package/dropbear/S50dropbear` does not work here. Two of
its assumptions are false on this image:

1. `DROPBEAR_INSTALL_TARGET_CMDS` makes `/etc/dropbear` a **symlink to
   `/var/run/dropbear`**, i.e. host keys live on tmpfs and are regenerated every
   boot. The stock script even prints "New keys will be generated at each boot.
   Are you sure this is what you want to do?" when it detects a read-only root.
   We ship a real `/etc/dropbear` directory instead.
2. The stock script adds `-R` (generate host keys on demand). On a read-only
   `/etc` that write fails at connect time, not at start time, so the daemon
   looks healthy and every connection fails. Dropped.

The shipped `/etc/init.d/S50dropbear`:

```sh
DAEMON=/usr/sbin/dropbear
HOSTKEY=/etc/dropbear/dropbear_ed25519_host_key
PIDFILE=/var/run/dropbear.pid
DROPBEAR_ARGS="-B -p 22 -r $HOSTKEY -P $PIDFILE"
test -r /etc/default/dropbear && . /etc/default/dropbear
...
    if [ ! -r "$HOSTKEY" ]; then echo "SKIP (no host key at $HOSTKEY)"; return 0; fi
    umask 077
    start-stop-daemon -S -q -p $PIDFILE --exec $DAEMON -- $DROPBEAR_ARGS
```

Runtime writable state is exactly one file, the pid file, and `/var/run` is a
symlink to `/run`, which `/etc/fstab` mounts `tmpfs`. Nothing else is written.

**It does not block boot when the interface is down.** dropbear binds
`0.0.0.0:22`, which succeeds with only `lo` up; there is no interface
dependency, no retry loop, and the host key is already on disk so there is no
keygen delay. If the key is ever missing the script prints `SKIP` and returns 0
rather than leaving a half-started daemon.

### Bringing up eth0

New `/etc/init.d/S45eth0`, deliberately not an `/etc/network/interfaces` stanza:

```sh
[ -e /sys/class/net/eth0 ] || exit 0
ip link set eth0 up
udhcpc -i eth0 -b -t 4 -T 2 -p /var/run/udhcpc.eth0.pid
```

`/etc/network/interfaces` was edited earlier in this project to *remove* the
eth0 stanza precisely because a nonexistent device cost 15 s of every boot.
Putting it back would reintroduce that on hardware, where the TinyEMU machine
layer still has no virtio-net device. This script is a no-op when the device is
absent, and its worst case when the device exists but no DHCP server answers is
`-t 4 -T 2` = 8 s, after which `-b` puts udhcpc in the background and boot
continues.

---

## 6. QEMU validation

`/br/qemu-ssh-test.sh` — same shape as `/br/qemu-pip-test.sh`, including its
unique-scratch-image trap (qemu takes a write lock and a leftover from a killed
run blocks the next one), plus:

```
-netdev user,id=n0,hostfwd=tcp:127.0.0.1:2222-:22
-device virtio-net-device,netdev=n0
```

`/br/qemu-ssh-ro-test.sh` is the same with `readonly=on` on the drive, `ro` on
the cmdline and port 2223, so `/etc/inittab`'s `mount -o remount,rw /` fails and
the guest really does run read-only. Logs: `/br/blinkabuild/qemu-ssh.log`,
`/br/blinkabuild/qemu-ssh-ro.log`.

### Boot

```
Starting network: OK
Starting DHCP on eth0: OK
Starting dropbear sshd: OK
```

### Login (read-write root, port 2222)

```
$ ssh -p 2222 root@127.0.0.1 'id; uname -a; ip -4 addr show eth0'
Warning: Permanently added '[127.0.0.1]:2222' (ED25519) to the list of known hosts.
uid=0(root) gid=0(root) groups=0(root),10(wheel)
Linux buildroot 6.1.44 #4 SMP Sun Aug  9 19:54:22 UTC 2026 riscv32 GNU/Linux
2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc pfifo_fast qlen 1000
    inet 10.0.2.15/24 brd 10.0.2.255 scope global eth0
```

No password prompt, no keyboard-interactive, no delay.

### The interface is really virtio-net, and the lease is really QEMU's

```
$ ssh ... 'readlink -f /sys/class/net/eth0/device/driver; cat /sys/class/net/eth0/address'
/sys/bus/virtio/drivers/virtio_net
52:54:00:12:34:56

$ ssh ... 'netstat -ltn | head -3; ping -c2 -W2 10.0.2.2'
tcp        0      0 0.0.0.0:22       0.0.0.0:*        LISTEN
64 bytes from 10.0.2.2: seq=0 ttl=255 time=1.952 ms
64 bytes from 10.0.2.2: seq=1 ttl=255 time=1.217 ms
2 packets transmitted, 2 packets received, 0% packet loss
```

### The host key is the shipped one

```
$ ssh-keyscan -p 2222 -t ed25519 127.0.0.1 | ssh-keygen -lf -
256 SHA256:mw2uYM2aswBxqDlLuAsbfERfovF8dwRZbTTqkgh25aw [127.0.0.1]:2222 (ED25519)
$ dropbearkey -y -f tree/etc/dropbear/dropbear_ed25519_host_key
Fingerprint: SHA256:mw2uYM2aswBxqDlLuAsbfERfovF8dwRZbTTqkgh25aw
```

### PTY allocation works (devpts, `CONFIG_UNIX98_PTYS`)

```
$ ssh -tt -p 2222 root@127.0.0.1 'tty; ls -l /dev/pts/'
/dev/pts/0
crw--w----    1 root     tty       136,   0 Jan  1 00:01 0
crw-rw-rw-    1 root     root        5,   2 Jan  1 00:00 ptmx
```

### scp — but only with `-O`

**This is the gotcha.** OpenSSH 9.x `scp` defaults to the SFTP protocol, and
dropbear has no sftp-server:

```
$ scp -P 2222 root@127.0.0.1:/tmp/blob.bin /tmp/back.bin
sh: /usr/libexec/sftp-server: not found
scp: Connection closed
```

`scp -O` selects the legacy SCP protocol, which dropbear's built-in `scp`
speaks, and both directions then work with matching checksums:

```
$ scp -O -P 2222 /tmp/blob.bin root@127.0.0.1:/tmp/blob.bin     # 200,000 B, 0.19 s
$ ssh -p 2222 root@127.0.0.1 'md5sum /tmp/blob.bin'
9ed0bfa1e6900fe82e24502a8cbba2c1  /tmp/blob.bin
$ scp -O -P 2222 root@127.0.0.1:/tmp/blob.bin /tmp/back.bin
$ md5sum /tmp/blob.bin /tmp/back.bin
9ed0bfa1e6900fe82e24502a8cbba2c1  /tmp/blob.bin
9ed0bfa1e6900fe82e24502a8cbba2c1  /tmp/back.bin
```

`scp` was worth shipping: it is inside `dropbearmulti` already, so it costs one
16-byte symlink. `sftp` is not available at any price short of building the
`openssh` package for one binary.

Fallbacks if `-O` is inconvenient, in order of preference:

```sh
ssh guest 'cat > /path/dest' < src              # binary-safe, no extra tools
ssh guest 'base64 -d > /path/dest' < <(base64 src)   # if the transport mangles 8-bit
rsync -e 'ssh -p 2222' src root@guest:/path     # rsync does not use SFTP; not shipped
```

The plain `cat` form is binary-safe over SSH and is what to reach for first;
base64 costs 33% more bytes and only earns it if something in the path is not
8-bit clean. `rsync` is not in the rootfs today.

### The guest's own client works

```
$ ssh -p 2222 root@127.0.0.1 'dbclient -y -p 22 root@127.0.0.1 uname -m'
Host '127.0.0.1' key accepted unconditionally.
(ssh-ed25519 fingerprint SHA256:mw2uYM2aswBxqDlLuAsbfERfovF8dwRZbTTqkgh25aw)
riscv32
```

### Read-only root (port 2223)

The point of the whole `-r`/no-`-R` arrangement:

```
$ ssh -p 2223 root@127.0.0.1 'mount | grep " / "; touch /etc/xx; ls -l /etc/dropbear/'
/dev/root on / type ext2 (ro,relatime)
touch: /etc/xx: Read-only file system
-rw-------    1 root     root            83 Aug  9  2026 dropbear_ed25519_host_key
```

Boot reached `Starting dropbear sshd: OK` and login succeeded with `/etc`
unwritable.

### Handshake cost, and what it should cost on silicon

Negotiated suite, all of it cheap by construction:

```
debug1: kex: algorithm: curve25519-sha256
debug1: kex: host key algorithm: ssh-ed25519
debug1: kex: server->client cipher: chacha20-poly1305@openssh.com  compression: none
```

curve25519 + ed25519 + chacha20-poly1305 is the combination to want here: no
RSA exponentiation, no AES table lookups, no compression, and all of it is
fixed-cost.

Wall-clock `connect + auth + exec + teardown` from the container: 0.148 s,
0.151 s, 0.151 s. That includes the local `ssh` process start, so it overstates
the guest's share.

The number that transfers is guest CPU. Ten loopback `dbclient` connections
inside the guest, which exercises **both** ends on the guest:

```
$ ssh ... 'time sh -c "i=0; while [ $i -lt 10 ]; do dbclient -y -p 22 root@127.0.0.1 true; i=$((i+1)); done"'
real 1.00s   user 0.41s   sys 0.09s
```

0.50 s of guest CPU for 10 handshake pairs = **50 ms per pair, ~25 ms per end**.

Projecting to the board (INFERRED, and the assumptions matter):

- This guest's peak straight-line throughput under QEMU, measured with a
  2-instruction counted loop (`/tmp/mips.c`, `addi`+`bnez`, 2×10^8 instructions):
  **3,190 / 3,199 / 3,240 MIPS**. That is TCG's *best* case — the whole loop
  lives in one translated block with no memory traffic — so real code is slower
  and this is an upper bound.
- 25 ms at ≤3,200 MIPS puts one handshake end at **≤8×10^7 guest instructions**.
- `08-guest-net-mmu.md` §2.5 estimates the M85 at 25-50 MIPS interpreting RV32.

8×10^7 / (25-50)×10^6 = **1.6 to 3.2 seconds per handshake on hardware, and
that is the pessimistic end**, because the instruction count came from TCG's
peak. So: seconds, not minutes, and a one-off per connection rather than a
per-byte cost. If it lands worse than ~5 s, the first thing to check is
`BR2_PACKAGE_DROPBEAR_SMALL`, which leaves `DROPBEAR_SMALL_CODE=1` (dropbear's
size-over-speed crypto paths); turning it off costs a `libtomcrypt` package
build and some flash, and is the only speed lever here that does not weaken the
cipher suite.

None of this is measured under TinyEMU, which is a slower interpreter than QEMU
TCG and is what actually runs on the board. The chain above is two estimates
deep. Treat it as an order-of-magnitude claim.

---

## 7. Sizes

### Kernel

`CONFIG_VIRTIO_NET=y` added to the trim config. It was the only symbol
`olddefconfig` changed. Its one `select` in 6.1.44 is `NET_FAILOVER`
(`drivers/net/Kconfig:414`), and `NET_FAILOVER`/`FAILOVER` were already `y`;
`NETDEVICES`, `PACKET`, `INET` and `IP_PNP_DHCP` were never trimmed. Note that
`CONFIG_ETHERNET` stays `n` — that symbol is the vendor-NIC menu, not the L2
stack, and `virtio_net` sits outside it.

| | Image bytes |
|---|---:|
| before | 6,543,840 |
| after | **6,577,272** |
| delta | **+33,432** |
| 8 MB slot | 8,388,608 |
| spare | 1,811,336 |

Backups kept at `/br/mmu-trim/Image.pre-virtionet` and
`/br/mmu-trim/config.pre-virtionet`.

### Rootfs

`mke2fs -q -t ext2 -d tree -b 1024 -I 128 -F out 55296`, so the image is a fixed
56,623,104 bytes either way; what changes is how much of it is free.

| | free 1K blocks | free inodes |
|---|---:|---:|
| `rootfs-pip.ext2` | 9,643 | 10,694 |
| `rootfs-ssh.ext2` | 9,358 | 10,682 |
| consumed | 285 (291,840 B) | 12 |

**56,623,104 B against the 58,458,112 B ceiling**, 1,835,008 B of slot to spare
and 9.1 MB of free space still inside the filesystem. `du -sk tree` reads
48,280 KB after the additions.

---

## 8. What remains for hardware

Nothing here has touched the board, and it cannot work there yet, because the
guest has no way to get an address:

1. **TinyEMU-side virtio-net is not wired up in the machine layer.** The device
   exists in `virtio.c` and costs 556 bytes of flash on top of the virtio-blk
   build we already need (`08-guest-net-mmu.md` §1.6), but the `EthernetDevice`
   vtable and the DTB node are unwritten. Until that lands, `/sys/class/net/eth0`
   does not exist on the board and `S45eth0` correctly exits 0.
2. **The Zephyr L2 bridge is unwritten** — the guest-facing `net_if`, the
   ~12-line `.set_config` promiscuous patch to `eth_renesas_ra.c`, and the
   `CONFIG_NET_ETHERNET_BRIDGE` wiring. That is the guest-net project's §7.2-§7.6.
3. **Untested assumption carried over:** whether the Mac's Internet Sharing
   `bootpd` hands out a second lease for the guest MAC alongside the board's
   `192.168.2.3` (`08-guest-net-mmu.md` §3.3). Everything above degrades
   gracefully if it does not — dropbear still binds, it just has no address to
   be reached on.

When those land, the only change needed on this side should be none: the same
rootfs, the same key, the same init scripts.

Before the board is on anything but a private segment, §4.

---

## 9. Fixed or found in passing

- **`trim-config.sh` ran `olddefconfig` with the host compiler.** There was no
  `CROSS_COMPILE` in the script, so `make ARCH=riscv olddefconfig` probed
  Debian's gcc 12.2.0 instead of Buildroot's riscv32 12.3.0 and silently
  rewrote a dozen unrelated symbols: `CC_VERSION_TEXT`, `GCC_VERSION`,
  `TOOLCHAIN_HAS_ZICBOM` + `RISCV_ISA_ZICBOM`, `RISCV_DMA_NONCOHERENT`, the
  whole `ARCH_HAS_SYNC_DMA_FOR_*`/`DMA_DIRECT_REMAP` block,
  `CC_HAVE_STACKPROTECTOR_TLS`, `STACKPROTECTOR_PER_TASK`, `GCC_PLUGINS`. No
  error, just a different kernel. Caught by diffing the config before and after
  the one-symbol change; the run was redone with the cross toolchain and the
  diff came back to exactly `CONFIG_VIRTIO_NET`. Fixed in
  `ra8d1-tinyemu/guest/trim-config.sh` with an explanatory comment.
- **Buildroot does not strip a package installed by `make <pkg>` alone.**
  Stripping happens in `target-finalize`, which we never run because the rootfs
  tree is hand-populated. 21.6% of the dropbear binary was debug info.
- **OpenSSH 9.x `scp` needs `-O` against dropbear** (§6). Costs an hour if you
  do not know it, because the error names a path that has never existed on this
  system.
- **`configure` probe output is not evidence a feature is compiled in.**
  Dropbear's `configure` prints `checking for lastlog.h... yes` and `checking
  for logwtmp... yes` even with `--disable-lastlog --disable-wtmp` on the
  command line. The dynamic symbol table is the check that actually answers the
  question (§2).

## 10. Regression check

`S99i2ctest` autorun under QEMU. The `I2C BRING-UP TEST` .. `END TEST` block was
extracted from all three logs and diffed; both new boots come back `IDENTICAL`
against the pre-change baseline `/br/blinkabuild/qemu-force2.log`:

```
--- /dev/i2c-* ---
ls: /dev/i2c-*: No such file or directory
--- i2cdetect -y -r 0 ---
Error: Could not open file `/dev/i2c-0' ...
BLINKA PASS detect: chip=None board=None
BLINKA FAIL import-board: NotImplementedError (platformdetect cannot identify the board)
BLINKA TEST DONE
```

Both failures are the expected QEMU shape — there is no paravirt bridge in
QEMU, so no I2C adapter registers and platformdetect has nothing to match. Not
a regression; identical to the baseline logs line for line.

## 11. Files changed

Repo:

- `ra8d1-linux/notes/ssh.md` — this file, new.
- `ra8d1-linux/notes/guest-mips.c` — new. The counted-loop probe from §6, kept
  because "how fast is the guest actually" comes up in every one of these notes
  and has so far always been estimated. Two instructions per iteration, exact
  dynamic count, no memory traffic. Build with the Buildroot cross gcc, run in
  the guest with an iteration count. Reports TCG's/interpreter's best case, not
  an average — read the caveat in §6 before quoting it.
- `ra8d1-tinyemu/guest/trim-config.sh` — `VIRTIO_NET` moved from the disable
  list to the enable list; `CROSS_COMPILE` fix (§9); `VIRTIO_NET` added to the
  post-`olddefconfig` assertion loop.
- `ra8d1-tinyemu/guest/linux-6.1.44-trim.config` — regenerated, one line.
- `ra8d1-tinyemu/guest/README.md` — `VIRTIO_NET` row and size table.

Container (not in the repo):

- `/br/mmu/.config` — four `BR2_PACKAGE_DROPBEAR*` lines; previous saved as
  `.config.pre-dropbear`.
- `/br/blinkabuild/tree/` — the six files in §1.
- `/br/blinkabuild/rootfs-ssh.ext2` — new image. `rootfs-pip.ext2` untouched.
- `/br/mmu-trim/linux-6.1.44/` — rebuilt; `Image.pre-virtionet` and
  `config.pre-virtionet` kept alongside.
- `/br/qemu-ssh-test.sh`, `/br/qemu-ssh-ro-test.sh` — new.
- `/br/hostdb/dropbear-2022.83/` — native `dropbearkey`/`dropbearconvert`.

## 12. Progress log

| Date | Entry |
|---|---|
| 2026-08-09 (rev 2) | Folded in the lead's upstream-scan refinements, each checked against this binary rather than the docs. `-r` is the right flag for 2022.83; pid file is on tmpfs already; `-B` was already there. New: proved lastlog/wtmp are compiled out via the dynamic symbol table (`configure`'s probe output says the opposite and is misleading), proved compression is `none` on both directions, and flagged the `#if !HAVE_CRYPT` wrapper that makes `localoptions.h` read as if password auth were disabled when it is not. **Measured the handshake instead of estimating it**: 50 ms of guest CPU per loopback pair, ~25 ms per end, and a counted-loop MIPS probe (new `notes/guest-mips.c`, 3,200 MIPS peak under QEMU) puts one handshake end at ≤8×10^7 instructions, i.e. **1.6-3.2 s on the board at the §2.5 estimate of 25-50 MIPS** — seconds, not minutes. Added the `cat`/base64 transfer fallbacks alongside `scp -O`. |
| 2026-08-09 | Built dropbear 2022.83 for riscv32 (281,628 B stripped, multi-call, `_SMALL` + `_CLIENT` + `_DISABLE_REVERSEDNS`); pre-generated the ed25519 host key on the build host with a natively compiled `dropbearkey`; wrote `S50dropbear` for a read-only root (real `/etc/dropbear`, `-r`, no `-R`, pid on tmpfs) and `S45eth0` for guarded DHCP. Enabled `CONFIG_VIRTIO_NET` in the trim kernel, +33,432 B. Proved login, PTY, `scp -O` both directions and `dbclient` under QEMU on both a read-write and a genuinely read-only root; `S99i2ctest` matches the baseline. Found and fixed a missing `CROSS_COMPILE` in `trim-config.sh` that had `olddefconfig` reading the host compiler. Root logs in over SSH with no credential — §4. |
