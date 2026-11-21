#!/bin/sh
# Usage: [TIMEOUT=90] [SMP=4] [PSC_KIP=path/to/psc.kip] scripts/run-psc-test.sh
#   SMP is the number of CPUs QEMU gives the machine (default 4). The kernel
#   uses at most four and runs on fewer.
#
# Runs sysmodules/tests/psc_test against sysmodules/psc in QEMU and reports the
# result. Exits 0 only if the test finished with no failures.
#
# Like run-net-test.sh it touches nothing shared: it builds its own boot image
# in a temporary folder from build/BOOTAA64.EFI, build/kernel.bin and the KIPs
# it needs straight from their build folders, so kips/ and build/disk.img are
# left alone. The test needs only sm, pm, psc and itself; no disk partitions
# beyond the EFI one, and no network.
#
# Build first:
#   make bootloader && make kernel
#   make -C vendor/atmosphere/stratosphere/sm nx_release ATMOSPHERE_CHECKED_LIBSTRATOSPHERE=1
#   make -C vendor/atmosphere/stratosphere/pm nx_release ATMOSPHERE_CHECKED_LIBSTRATOSPHERE=1
#   make -C sysmodules/psc               nx_release ATMOSPHERE_CHECKED_LIBSTRATOSPHERE=1
#   make -C sysmodules/tests/psc_test
cd "$(dirname "$0")/.."

: "${OVMF:?set OVMF to the AArch64 UEFI firmware file}"
TIMEOUT=${TIMEOUT:-90}
SMP=${SMP:-4}
OUT=out/nintendo_nx_arm64_armv8a/release
PSC_KIP=${PSC_KIP:-sysmodules/psc/$OUT/psc.kip}

KIPS="vendor/atmosphere/stratosphere/sm/$OUT/sm.kip
vendor/atmosphere/stratosphere/pm/$OUT/pm.kip
sysmodules/tests/psc_test/$OUT/psc_test.kip"

for f in "$OVMF" build/BOOTAA64.EFI build/kernel.bin "$PSC_KIP" $KIPS; do
    if [ ! -f "$f" ]; then
        echo "missing $f (see the build steps at the top of this script)" >&2
        exit 2
    fi
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/horizon-psc-test.XXXXXX")
QEMU_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

# The boot image: an EFI system partition holding the bootloader, the kernel and the KIPs.
mkdir -p "$WORK/root/EFI/BOOT" "$WORK/root/kips"
cp build/BOOTAA64.EFI "$WORK/root/EFI/BOOT/BOOTAA64.EFI"
cp build/kernel.bin "$WORK/root/kernel.bin"
for k in $KIPS; do cp "$k" "$WORK/root/kips/"; done
cp "$PSC_KIP" "$WORK/root/kips/psc.kip"
scripts/hzdisk.py create "$WORK/boot.img" --size 96M --layout "ESP:rest:esp" >/dev/null || exit 2
scripts/hzdisk.py put "$WORK/boot.img" ESP "$WORK/root" / >/dev/null || exit 2

LOG=$WORK/serial.log
: > "$LOG"
qemu-system-aarch64 \
    -device virtio-rng-pci \
    -machine virt,gic-version=3 \
    -cpu cortex-a57 \
    -smp "$SMP" \
    -m 4G \
    -bios "$OVMF" \
    -drive file="$WORK/boot.img",format=raw,if=virtio \
    -nic none \
    -display none \
    -serial file:"$LOG" &
QEMU_PID=$!

# Wait for the test's summary line.
i=0
while [ $i -lt $((TIMEOUT * 5)) ]; do
    grep -aq '^\[psc_test\] done' "$LOG" && break
    kill -0 "$QEMU_PID" 2>/dev/null || break
    sleep 0.2
    i=$((i + 1))
done

grep -a '^\[psc\]\|^\[psc_test\]' "$LOG"

if grep -aq '^\[psc_test\] done: [0-9]* checks, 0 failure(s)' "$LOG"; then
    exit 0
fi
if ! grep -aq '^\[psc_test\] done' "$LOG"; then
    echo "psc_test did not finish within $TIMEOUT seconds" >&2
    echo "--- last lines of the serial log ---" >&2
    tail -n 40 "$LOG" >&2
fi
exit 1
