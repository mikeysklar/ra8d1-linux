"""Blinka bring-up test, run unattended from S99i2ctest at boot.

Prints one PASS/FAIL line per stage so the whole story is readable from the
boot log alone. Never raises: the console input does not work at the shell on
hardware, so an uncaught exception here would end the test early with no way
to poke at the remains.
"""
import sys
import traceback


def stage(name, fn):
    try:
        result = fn()
        print(f"BLINKA PASS {name}: {result}")
        return result
    except Exception:
        print(f"BLINKA FAIL {name}:")
        traceback.print_exc(file=sys.stdout)
        return None


def detect():
    import adafruit_platformdetect
    d = adafruit_platformdetect.Detector()
    return f"chip={d.chip.id} board={d.board.id}"


def import_board():
    import board
    return f"board module ok, SCL={board.SCL} SDA={board.SDA}"


def i2c_scan():
    import board
    i2c = board.I2C()
    while not i2c.try_lock():
        pass
    try:
        found = [hex(a) for a in i2c.scan()]
    finally:
        i2c.unlock()
    return f"scan={found}"


stage("detect", detect)
if stage("import-board", import_board) is not None:
    stage("i2c-scan", i2c_scan)
print("BLINKA TEST DONE")
