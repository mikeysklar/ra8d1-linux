# pv-io.c: first compile, and what it found

`pv-io.c` had never been compiled. This is the record of building it against a
real riscv32 kernel tree and loading it in a guest.

Environment: Linux 6.1.44, `riscv32-buildroot-linux-gnu-gcc 12.3.0`, kernel tree
at `/br/mmu/build/linux-6.1.44` in the `br` container, run under
`qemu-system-riscv32 -M virt`.

Built out of tree:

```sh
printf 'obj-m := pv-io.o\n' > Makefile
make -C /br/mmu/build/linux-6.1.44 M=$PWD ARCH=riscv \
     CROSS_COMPILE=/br/mmu/host/bin/riscv32-buildroot-linux-gnu- W=1 modules
```

## Summary

| | Result |
|---|---|
| Compiles | Yes, **zero warnings even at `W=1`** |
| Builds as a module unmodified | No: missing `MODULE_LICENSE()` |
| Loads without the bridge present | **No: oopses.** Fixed |
| Registers an i2c adapter and a gpiochip | Yes, verified against a fake bridge |
| Blinka works through it end to end | Yes, `detect_test.py` 7/7 |

The C itself was in better shape than expected. Every real defect was in how it
talks to the kernel, not in its logic.

## 1. Missing MODULE_LICENSE (only matters for an out-of-tree build)

```
ERROR: modpost: missing MODULE_LICENSE() in /br/mmu-pv/pv-io.o
make[1]: *** [scripts/Makefile.modpost:126: Module.symvers] Error 1
```

On 6.1 this is a hard modpost **error**, so the build stops. It is not a taint
warning and not a link failure against `EXPORT_SYMBOL_GPL`; those come later if
you force past it. The intended built-in path via `device_initcall()` needs none
of this, so the file was not wrong, just unbuildable in the form used to test it.

## 2. The zero-length quirk cannot be set the way it was specified

The suggested form does not compile:

```
pv-io.c:318:40: error: assignment of member 'flags' in read-only object
  318 |                 pv->adap.quirks->flags |= I2C_AQ_NO_ZERO_LEN;
      |                                        ^~
```

`struct i2c_adapter.quirks` is `const struct i2c_adapter_quirks *`
(`include/linux/i2c.h:747`). Set the flag in the static initialiser instead.

## 3. The bridge probe oopses the kernel

This is the important one.

```
Oops - load access fault [#1]
Modules linked in: pv_io(O+)
CPU: 1 PID: 120 Comm: insmod Tainted: G           O       6.1.44 #2
Hardware name: riscv-virtio,qemu (DT)
epc : pv_io_init+0x58/0x1000 [pv_io]
status: 00000120 badaddr: a066d000 cause: 00000005
[<a0669058>] pv_io_init+0x58/0x1000 [pv_io]
[<c000210c>] do_one_initcall+0x48/0x246
```

`ioremap()` succeeded and returned `0xa066d000` in the vmalloc window. The fault
is the very first `readl(pv->base + PV_ID)`, `cause 5` = load access fault.

The comment at the probe said:

```c
/* Not an error: a guest booted under a plain emulator with no
 * bridge reads back zero here and must still boot. */
```

**That is not true on RISC-V.** A load from an unassigned physical address raises
a load access fault; it does not read back zero. Because `pv_io_init()` is a
`device_initcall()`, built into the kernel as intended this is an **oops during
boot** on any machine that does not have the bridge, not merely a failed insmod.

The fix keeps the promise the comment makes. `copy_from_kernel_nofault()` plants
an exception-table entry, and riscv's `do_trap_error()` consults
`fixup_exception()` before `die()` (`arch/riscv/kernel/traps.c`), so the fault is
caught and returned as `-EFAULT`. riscv does not override
`copy_from_kernel_nofault_allowed()`, so the permissive default in
`mm/maccess.c` applies. After the fix:

```
ra8d1-pv: no bridge at 0x11200000 (probe faulted), skipping
insmod: can't insert '/root/pv-io.ko': No such device
```

Clean `-ENODEV`, kernel alive, no taint beyond the out-of-tree marker.

A device tree node would be the more idiomatic answer and would avoid probing a
hardcoded address at all. The nofault probe is the smaller change and preserves
the current no-DT design.

## 4. The zero-length quirk is necessary but NOT sufficient

Setting `I2C_AQ_NO_ZERO_LEN` does exactly what it should. The i2c core rejects
the transfer before it reaches the driver:

```
i2c i2c-0: adapter quirk: no zero length (addr 0x0008, size 0, write)
```

and `i2cdetect -F 0` flips from `SMBus Quick Command  yes` to `no`.

It does **not** on its own make `scan()` accurate. Blinka's
`generic_linux/i2c.py` falls back:

```python
for addr in range(0x08, 0x78):
    try:
        self._i2c_bus.write_quick(addr)
    except OSError:
        try:
            self._i2c_bus.read_byte(addr)
        except OSError:
            continue
    found.append(addr)
```

When `write_quick` starts failing, the probe becomes a one-byte `read_byte`. A
bridge that returns success for a read to an address nobody acked still reports
all 112 addresses. Measured against a fake bridge that acks everything:

| Variant | `scan()` result |
|---|---|
| No quirk, bridge acks everything | 112 addresses |
| Quirk set, bridge acks everything | **still 112 addresses** |
| Quirk set, bridge NAKs all but 0x14 | `['0x14']` |

**The real requirement is on the Zephyr side:** `pv_cmd()` must return a failure
status for an address that did not ACK. If the host returns 0 unconditionally,
`scan()` is meaningless no matter what the guest driver advertises. Worth
checking `rvlinux/src/main.c` before trusting any scan on hardware.

## 5. gpiochip base really does land at 504

Confirmed empirically with `gc.base = -1`:

```
/sys/class/gpio/gpiochip504/label = ra8d1-pv
/sys/class/gpio/gpiochip504/base  = 504
```

Left as `-1`. `ra8d1_pv/pin.py` already resolves the base by label and that
worked first time, so a fixed base buys predictability at the cost of a probe
failure if GPIO 0 is ever taken.

There is a latent bug on the Python side though, and it is worth more than the
base question: `_pv_gpio_base()` falls back to returning **0** when it cannot
find a chip labelled `ra8d1-pv`. That silently produces wrong pin numbers rather
than failing. It should raise. With `gc.base = 0` the bad fallback would be
correct by accident, which is a reason to prefer fixing `pin.py`.

## 6. What actually works, verified

Against a RAM-backed fake bridge (test harness, not the driver) reporting 1 bus,
8 GPIOs and a 256-byte window, with the fake NAKing everything except 0x14:

```
ra8d1-pv: bridge v1, 1 i2c bus(es), 8 gpio(s), 256 B window
/sys/bus/i2c/devices/i2c-0/name = EK-RA8D1 paravirt I2C
/sys/class/gpio/gpiochip504/label = ra8d1-pv

i2cdetect -y -r 0
10: -- -- -- -- 14 -- -- -- -- -- -- -- -- -- -- --

blinka detect_test.py: 7/7 stages passed
  4. PureIO straight at /dev/i2c-0   read_byte probe found: ['0x14']
  5. busio.I2C                       scan: ['0x14']
  6. digitalio on an on-board LED    blinked LED1 three times
```

So `i2c_add_numbered_adapter()`, `gpiochip_add_data()`, `pv_i2c_xfer()`, the
quirks, the functionality mask, `pv_gpio_set()` and `pv_gpio_dir_out()` are all
exercised and behave. Module unloads cleanly, no oops anywhere in dmesg.

What this does **not** prove: any of the actual MMIO protocol. The fake bridge is
kernel RAM, so word-packing in `pv_put()`/`pv_get()`, the register offsets, and
the assumption that a store to `PV_CMD` blocks until the host finishes are all
still untested. Those need the real emulator.

## 7. The patch

Applied to a copy; `ra8d1-linux/guest/pv-io.c` is unmodified. The `#ifdef MODULE`
block is only needed to build a `.ko` for testing and can be dropped for the
built-in path, though `MODULE_LICENSE` is harmless to keep.

```diff
--- a/pv-io.c
+++ b/pv-io.c
@@ -41,6 +41,7 @@
 #include <linux/i2c.h>
 #include <linux/gpio/driver.h>
 #include <linux/errno.h>
+#include <linux/uaccess.h>
 
 #define PV_PHYS            0x11200000UL
 #define PV_LEN             0x1000
@@ -197,7 +198,7 @@
 
 static u32 pv_i2c_func(struct i2c_adapter *adap)
 {
-	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
+	return I2C_FUNC_I2C | (I2C_FUNC_SMBUS_EMUL & ~I2C_FUNC_SMBUS_QUICK);
 }
 
 static const struct i2c_algorithm pv_i2c_algo = {
@@ -213,7 +214,7 @@
 	.max_read_len = 256,
 	.max_comb_1st_msg_len = 128,
 	.max_comb_2nd_msg_len = 128,
-	.flags = I2C_AQ_COMB_WRITE_THEN_READ,
+	.flags = I2C_AQ_COMB_WRITE_THEN_READ | I2C_AQ_NO_ZERO_LEN,
 };
 
 /* ------------------------------------------------------------------ gpio */
@@ -293,10 +294,27 @@
 		goto err_free;
 	}
 
-	id = readl(pv->base + PV_ID);
+	/* Probe with copy_from_kernel_nofault(), NOT a bare readl().
+	 *
+	 * On RISC-V a load from an unassigned physical address raises a load
+	 * access fault (scause 5); it does NOT read back zero. The plain
+	 * readl() this used to do therefore oopsed on any machine without the
+	 * bridge -- and as a device_initcall that is an oops during boot, not
+	 * merely a failed insmod. copy_from_kernel_nofault() plants an
+	 * exception-table fixup, which riscv do_trap_error() consults via
+	 * fixup_exception(), so we get -EFAULT instead of a dead kernel.
+	 * Verified on qemu-system-riscv32 -M virt, which has nothing mapped
+	 * at 0x11200000.
+	 */
+	if (copy_from_kernel_nofault(&id,
+				     (const void __force *)(pv->base + PV_ID),
+				     sizeof(id))) {
+		pr_info("ra8d1-pv: no bridge at %#lx (probe faulted), skipping\n",
+			PV_PHYS);
+		ret = -ENODEV;
+		goto err_unmap;
+	}
 	if (id != PV_ID_MAGIC) {
-		/* Not an error: a guest booted under a plain emulator with no
-		 * bridge reads back zero here and must still boot. */
 		pr_info("ra8d1-pv: no bridge at %#lx (id %#x), skipping\n",
 			PV_PHYS, id);
 		ret = -ENODEV;
@@ -357,3 +375,22 @@
 /* Late enough that the i2c and gpio cores are up, early enough that anything
  * hanging off the bus can still probe normally. */
 device_initcall(pv_io_init);
+
+#ifdef MODULE
+static void __exit pv_io_exit(void)
+{
+	if (!pv)
+		return;
+	if (pv->gc.ngpio)
+		gpiochip_remove(&pv->gc);
+	if (pv->adap.algo)
+		i2c_del_adapter(&pv->adap);
+	iounmap(pv->base);
+	kfree(pv);
+	pv = NULL;
+}
+module_exit(pv_io_exit);
+#endif
+
+MODULE_LICENSE("GPL v2");
+MODULE_DESCRIPTION("EK-RA8D1 paravirtual I2C/GPIO bridge");
```

## 8. Build variants kept in the container

| Path | What it is |
|---|---|
| `/br/mmu-pv/` | Production source with the patch above, plus `pv-io.c.orig` |
| `/br/mmu-pv-test/` | RAM-backed fake bridge, acks everything |
| `/br/mmu-pv-noquirk/` | Fake bridge with both i2c fixes reverted, to reproduce the 112-address scan |
| `/br/mmu-pv-nak/` | Fake bridge that NAKs all but 0x14, the 7/7 run |

Guest console logs for each run are in `/br/mmu-boot/boot9.log` through
`boot11.log`. Guest image with Blinka and the board definition already installed
is `/br/mmu-boot/rootfs-work2.ext2`.
