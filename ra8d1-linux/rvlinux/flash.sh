#!/bin/sh
# Flash the app to the EK-RA8D1 with probe-rs.
#
# probe-rs cannot map the RA8 option-setting flash regions, so those sections
# are stripped from a scratch copy of the ELF before download. Do NOT use
# west/JLinkExe here: JLinkExe force-updates the on-board probe firmware and
# bricks it.
set -eu

APP_DIR=$(cd "$(dirname "$0")" && pwd)
ELF=${1:-$APP_DIR/build/zephyr/zephyr.elf}
CHIP=R7FA8D1BH
PROBE=1366:0105:001086859839
STRIPPED=/tmp/ra8d1-app-noofs.elf

set --
for s in $(arm-none-eabi-objdump -h "$ELF" | awk '{print $2}' | grep '^\.option_setting'); do
	set -- "$@" --remove-section "$s"
done
arm-none-eabi-objcopy "$@" "$ELF" "$STRIPPED"

probe-rs download --chip "$CHIP" --probe "$PROBE" "$STRIPPED"
probe-rs reset --chip "$CHIP" --probe "$PROBE"
