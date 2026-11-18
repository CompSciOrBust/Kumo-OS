#!/bin/sh
# Usage: [TIMEOUT=60] [SMP=4] [CPU=cortex-a57] scripts/run-counter-test.sh
#   SMP is the number of CPUs QEMU gives the machine (default 4). The kernel
#   uses at most four and runs on fewer.
#
# Runs sysmodules/tests/counter_test in QEMU and reports the result: that
# userland sees a 19.2 MHz counter (kernel patch 0013), whatever rate the
# machine's counter runs at, and what one read costs. Exits 0 only if every
# check passed.
#
# Like the other test scripts it builds its own boot image in a temporary
# folder from build/BOOTAA64.EFI, build/kernel.bin and the KIPs straight from
# their build folders, so kips/ and build/disk.img are left alone.
#
# Build first:
#   make bootloader && make kernel
#   for m in sm pm; do make -C vendor/atmosphere/stratosphere/$m nx_release ATMOSPHERE_CHECKED_LIBSTRATOSPHERE=1; done
#   make -C sysmodules/tests/counter_test
cd "$(dirname "$0")/.."

: "${OVMF:?set OVMF to the AArch64 UEFI firmware file}"
TIMEOUT=${TIMEOUT:-60}
SMP=${SMP:-4}
CPU=${CPU:-cortex-a57}
OUT=out/nintendo_nx_arm64_armv8a/release

KIPS="vendor/atmosphere/stratosphere/sm/$OUT/sm.kip
vendor/atmosphere/stratosphere/pm/$OUT/pm.kip
sysmodules/tests/counter_test/$OUT/counter_test.kip"

for f in "$OVMF" build/BOOTAA64.EFI build/kernel.bin $KIPS; do
    if [ ! -f "$f" ]; then
        echo "missing $f (see the build steps at the top of this script)" >&2
        exit 2
    fi
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/horizon-counter-test.XXXXXX")
QEMU_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

mkdir -p "$WORK/root/EFI/BOOT" "$WORK/root/kips"
cp build/BOOTAA64.EFI "$WORK/root/EFI/BOOT/BOOTAA64.EFI"
cp build/kernel.bin "$WORK/root/kernel.bin"
for k in $KIPS; do cp "$k" "$WORK/root/kips/"; done
scripts/hzdisk.py create "$WORK/boot.img" --size 96M --layout "ESP:rest:esp" >/dev/null || exit 2
scripts/hzdisk.py put "$WORK/boot.img" ESP "$WORK/root" / >/dev/null || exit 2

LOG=$WORK/serial.log
: > "$LOG"
qemu-system-aarch64 \
    -machine virt,gic-version=3 \
    -cpu "$CPU" \
    -smp "$SMP" \
    -m 4G \
    -bios "$OVMF" \
    -drive file="$WORK/boot.img",if=virtio,format=raw \
    -display none \
    -serial file:"$LOG" &
QEMU_PID=$!

i=0
while [ $i -lt $((TIMEOUT * 5)) ]; do
    grep -aq '^\[counter_test\] done' "$LOG" && break
    kill -0 "$QEMU_PID" 2>/dev/null || break
    sleep 0.2
    i=$((i + 1))
done

grep -a '^Counter frequency\|CPUs present\|^Userland cores\|^\[counter_test\]' "$LOG"

if grep -aq '^\[counter_test\] done: [0-9]* checks, 0 failure(s)' "$LOG"; then
    exit 0
fi
if ! grep -aq '^\[counter_test\] done' "$LOG"; then
    echo "counter_test did not finish within $TIMEOUT seconds" >&2
fi
exit 1
