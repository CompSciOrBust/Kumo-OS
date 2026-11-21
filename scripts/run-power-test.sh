#!/bin/sh
# Usage: [TIMEOUT=60] [SMP=4] scripts/run-power-test.sh [off|reboot|both]
#   SMP is the number of CPUs QEMU gives the machine (default 4). The kernel
#   uses at most four and runs on fewer.
#
# Boots sysmodules/tests/power_test in QEMU and checks that userland can power
# the machine off and reboot it: power_test asks spl, spl makes the SetConfig
# SMC, and the kernel makes the PSCI call. Runs both by default. Exits 0 only
# if every run passed.
#
#   off     PSCI SYSTEM_OFF makes QEMU exit by itself. The run passes if QEMU
#           exits within TIMEOUT seconds and the kernel logged the power-off.
#   reboot  QEMU runs with -no-reboot, so a reset makes it exit too. The run
#           passes if QEMU exits within TIMEOUT seconds and the kernel logged
#           the reboot; the bootloader banner must appear exactly once, which
#           shows the exit was the reset and not a second boot.
#
# Like run-net-test.sh it touches nothing shared: each run builds its own disk
# in a temporary folder (an NVMe disk with an EFI system partition and an SD
# card holding /power_test.txt, which tells power_test what to do) from
# build/BOOTAA64.EFI, build/kernel.bin and the KIPs straight from their build
# folders, so kips/ and build/disk.img are left alone.
#
# Build first:
#   make bootloader && make kernel
#   for m in sm pm spl; do make -C vendor/atmosphere/stratosphere/$m nx_release ATMOSPHERE_CHECKED_LIBSTRATOSPHERE=1; done
#   make -C sysmodules/fs              nx_release ATMOSPHERE_CHECKED_LIBSTRATOSPHERE=1
#   make -C sysmodules/tests/power_test
cd "$(dirname "$0")/.."

: "${OVMF:?set OVMF to the AArch64 UEFI firmware file}"
TIMEOUT=${TIMEOUT:-60}
SMP=${SMP:-4}
MODE=${1:-both}
OUT=out/nintendo_nx_arm64_armv8a/release

# The bootloader's first line, printed once per boot.
BANNER='HorizonUEFI init'

KIPS="vendor/atmosphere/stratosphere/sm/$OUT/sm.kip
vendor/atmosphere/stratosphere/pm/$OUT/pm.kip
vendor/atmosphere/stratosphere/spl/$OUT/spl.kip
sysmodules/fs/$OUT/fs.kip
sysmodules/tests/power_test/$OUT/power_test.kip"

case "$MODE" in
    off|reboot|both) ;;
    *) echo "usage: scripts/run-power-test.sh [off|reboot|both]" >&2; exit 2 ;;
esac

for f in "$OVMF" build/BOOTAA64.EFI build/kernel.bin $KIPS; do
    if [ ! -f "$f" ]; then
        echo "missing $f (see the build steps at the top of this script)" >&2
        exit 2
    fi
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/horizon-power-test.XXXXXX")
QEMU_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

mkdir -p "$WORK/kips"
for k in $KIPS; do cp "$k" "$WORK/kips/"; done

# run <off|reboot>: one boot. Returns 0 if it passed.
run() {
    action=$1
    disk=$WORK/$action.img
    log=$WORK/$action.log

    KIPS="$WORK/kips" LAYOUT="SYSTEM:32M:fat,SDCARD:rest:fat" \
        scripts/build-system-disk.sh "$disk" 128M >/dev/null || return 2
    mkdir -p "$WORK/sd-$action"
    echo "$action" > "$WORK/sd-$action/power_test.txt"
    scripts/hzdisk.py put "$disk" SDCARD "$WORK/sd-$action" / >/dev/null || return 2

    extra=""
    [ "$action" = reboot ] && extra="-no-reboot"

    : > "$log"
    qemu-system-aarch64 \
        -device virtio-rng-pci \
        -machine virt,gic-version=3 \
        -cpu cortex-a57 \
        -smp "$SMP" \
        -m 4G \
        -bios "$OVMF" \
        -drive file="$disk",if=none,id=nvme0,format=raw \
        -device nvme,drive=nvme0,serial=horizon0 \
        -display none \
        $extra \
        -serial file:"$log" &
    QEMU_PID=$!

    # Wait for QEMU to exit by itself.
    i=0
    exited=0
    while [ $i -lt $((TIMEOUT * 5)) ]; do
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            exited=1
            break
        fi
        sleep 0.2
        i=$((i + 1))
    done
    if [ $exited = 0 ]; then
        kill "$QEMU_PID" 2>/dev/null
    fi
    wait "$QEMU_PID" 2>/dev/null
    QEMU_PID=""

    echo "--- $action ---"
    grep -a "^\[power_test\]\|SetConfig SMC\|PSCI call\|$BANNER" "$log"

    if [ $exited = 0 ]; then
        echo "$action: FAIL, QEMU still running after $TIMEOUT seconds" >&2
        return 1
    fi
    case "$action" in
        off)    want='SetConfig SMC from spl: powering off (PSCI SYSTEM_OFF)' ;;
        reboot) want='SetConfig SMC from spl: rebooting (reboot type 4, PSCI SYSTEM_RESET)' ;;
    esac
    if ! grep -aqF "$want" "$log"; then
        echo "$action: FAIL, QEMU exited but the kernel did not log the request" >&2
        return 1
    fi
    if grep -aq '^\[power_test\] FAIL' "$log"; then
        echo "$action: FAIL, power_test reported a failure" >&2
        return 1
    fi
    boots=$(grep -ac "$BANNER" "$log")
    if [ "$boots" != 1 ]; then
        echo "$action: FAIL, the bootloader started $boots times" >&2
        return 1
    fi
    echo "$action: PASS"
    return 0
}

result=0
if [ "$MODE" = off ] || [ "$MODE" = both ]; then
    run off || result=1
fi
if [ "$MODE" = reboot ] || [ "$MODE" = both ]; then
    run reboot || result=1
fi
exit $result
