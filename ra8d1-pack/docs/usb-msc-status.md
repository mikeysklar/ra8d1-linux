# USB MSC on the EK-RA8D1 — status

**Symptom:** the board enumerates as a USB composite device, the host binds the
full mass-storage stack, and no `/dev/disk` node is ever created. The volume
never mounts.

**Not blocking.** The REPL file-push workflow (`tools/putfile.py`) replaces the
drive for development. This note exists so the next person does not re-derive
eight dead ends.

---

## Where the fault is

The failure is in the **RA8 USB HS bulk-IN data stage** — not in CircuitPython
storage, not in the filesystem, not in the descriptors.

With a per-read trace compiled into `_zephyr_disk_read()`, the read count across
a full enumerate-and-timeout cycle is **zero**. The disk layer is never reached.

The board console shows this repeating every ~31 seconds, which is the host's
SCSI command timeout plus device-reset recovery:

```
<wrn> usbd_cdc_acm: request ep 0x01, len 0 cancelled
<wrn> usbd_cdc_acm: request ep 0x82, len 0 cancelled
<wrn> usbd_msc:     request ep 0x02, len 0 cancelled
<wrn> usbd_msc:     request ep 0x83, len 512 cancelled     <-- 512-byte IN queued, never transfers
```

So: host sends READ(10) -> `usbd_msc` queues the 512-byte IN response -> the
data never moves on the wire -> host times out, resets, retries, eventually
gives up and caches that verdict.

Host-side registry state for the published media:

```
Size                 = 127488        correct (249 x 512)
Preferred Block Size = 512           correct
Whole                = True
BSD Name             = <ABSENT>      no /dev/disk node is created
Content              = ''            no partition scheme claimed it
IOServiceBusyState   = 1             stuck mid-matching, never clears
```

The media publishes with correct geometry and then hangs in matching, so the
BSD client never attaches.

The board's own `ek_ra8d1.conf` already carries an upstream comment warning that
the RA8 USB peripheral "can generate bursts of UDC/USBD events (especially with
HS + MSC)" leading to "dropped events that can desynchronize control-transfer
state." Somebody fought this before.

---

## The filesystem is provably fine

To take USB out of the picture, the exact host-visible image was reconstructed
on the host: 248 sectors read over SWD from the filesystem region, with a
locally rebuilt MBR replicating `supervisor/shared/flash.c build_partition()`
prepended (type 0x06, start LBA 1, size 248) = 127,488 bytes / 249 sectors.

```
hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount image.img
  -> FDisk_partition_scheme
  -> DOS_FAT_16
mount
  -> Volume CIRCUITPY mounted
```

Every file readable: `boot_out.txt` with the correct banner, `code.py`, `lib/`,
`settings.toml`. **MBR, VBR, FAT16, directory and volume label are all valid.**

This also confirms patch 0001 worked, and settles the last BPB question:
`BS_FilSysType = "FAT     "` against FAT12 geometry (238 clusters) is cosmetic —
the host mounted it as `DOS_FAT_16` regardless.

---

## Ruled out with evidence — do not re-derive

| Theory | Refuted by |
|---|---|
| USB descriptors wrong | `bInterfaceClass=8, SubClass=6, Protocol=80`, 2 endpoints — matches the host mass-storage matcher chain exactly |
| No MSC LUN declared | `USBD_DEFINE_MSC_LUN(circuitpy_lun, "CIRCUITPY", ...)`, name matches `disk_access_register` |
| Disk status returns write-protect / no-media | instrumented — that function is **never called** |
| `disk_ioctl` returns uninitialized capacity | measured `rc=0 val=249`, `rc=0 val=512` — correct |
| Host never sees a partition table | host reads LBA 0 -> 1 -> 2, so it parses the MBR and follows the partition entry |
| Boot sector / BPB invalid | mounts cleanly as a disk image (above) |
| APFS container scheme attaching is the fault | that scheme matches all whole block devices speculatively by design; it appears on healthy drives too |
| VBUS-detect gate skips `usbd_enable` | the RA8 UDC driver never sets `caps.can_detect_vbus`, so the enable branch does run; instrumented and confirmed |

---

## Reproducing / re-testing

Once the host has declined a device it caches that verdict, and a soft or hard
reset will **not** re-test it — subsequent resets produce zero further MSC
reads. Only a physical USB unplug and replug forces re-enumeration.

To instrument the read path, add a compact `printk` in `_zephyr_disk_read()`
reporting lba, count, return code and the first/last bytes of the buffer. Keep
it to one line per read: a hexdump per sector floods the Zephyr log backend and
drops several hundred messages per cycle, which hides the pattern you are
looking for.

---

## Suggested next step

Report upstream to Renesas as a UDC HS + MSC bulk-IN issue, with the endpoint
trace above. The device-level stack is healthy (device is enabled, configured,
at High Speed); it is the class-level data stage that never completes.
