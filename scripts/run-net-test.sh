#!/bin/sh
# Usage: [TIMEOUT=90] [SMP=4] [QEMU_NET_DEVICE=virtio-net-pci] [BSD_KIP=path] scripts/run-net-test.sh
#   SMP is the number of CPUs QEMU gives the machine (default 4). The kernel
#   uses at most four and runs on fewer.
#   BSD_KIP optionally selects a different BSD module build for the test.
#
# Runs sysmodules/tests/net_test against sysmodules/bsdsockets in QEMU and reports
# the result. Exits 0 only if the test finished with no failures.
#
# Unlike run-qemu.sh this touches nothing shared: it builds its own boot image
# in a temporary folder from build/BOOTAA64.EFI, build/kernel.bin and the KIPs
# it needs straight from their build folders, so kips/ and build/disk.img are
# left alone. It starts sysmodules/tests/net_test/host_server.py for the duration.
#
# Build first:
#   make bootloader && make kernel
#   make -C vendor/atmosphere/stratosphere/sm nx_release ATMOSPHERE_CHECKED_LIBSTRATOSPHERE=1
#   make -C vendor/atmosphere/stratosphere/pm nx_release ATMOSPHERE_CHECKED_LIBSTRATOSPHERE=1
#   make -C sysmodules/fs         nx_release ATMOSPHERE_CHECKED_LIBSTRATOSPHERE=1
#   make -C sysmodules/bsdsockets nx_release ATMOSPHERE_CHECKED_LIBSTRATOSPHERE=1
#   make -C sysmodules/tests/net_test
cd "$(dirname "$0")/.."

: "${OVMF:?set OVMF to the AArch64 UEFI firmware file}"
TIMEOUT=${TIMEOUT:-90}
SMP=${SMP:-4}
DEVICE=${QEMU_NET_DEVICE:-virtio-net-pci}
OUT=out/nintendo_nx_arm64_armv8a/release
BSD_KIP=${BSD_KIP:-sysmodules/bsdsockets/$OUT/bsdsockets.kip}

KIPS="vendor/atmosphere/stratosphere/sm/$OUT/sm.kip
vendor/atmosphere/stratosphere/pm/$OUT/pm.kip
sysmodules/fs/$OUT/fs.kip
$BSD_KIP
sysmodules/tests/net_test/$OUT/net_test.kip"

for f in "$OVMF" build/BOOTAA64.EFI build/kernel.bin $KIPS; do
    if [ ! -f "$f" ]; then
        echo "missing $f (see the build steps at the top of this script)" >&2
        exit 2
    fi
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/horizon-net-test.XXXXXX")
HOST_PID=""
QEMU_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null
    [ -n "$HOST_PID" ] && kill "$HOST_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

# The boot image: an EFI system partition holding the bootloader, the kernel and the KIPs.
mkdir -p "$WORK/root/EFI/BOOT" "$WORK/root/kips"
cp build/BOOTAA64.EFI "$WORK/root/EFI/BOOT/BOOTAA64.EFI"
cp build/kernel.bin "$WORK/root/kernel.bin"
for k in $KIPS; do cp "$k" "$WORK/root/kips/"; done
scripts/hzdisk.py create "$WORK/boot.img" --size 96M --layout "ESP:rest:esp" >/dev/null || exit 2
scripts/hzdisk.py put "$WORK/boot.img" ESP "$WORK/root" / >/dev/null || exit 2

# The host side of the test.
sysmodules/tests/net_test/host_server.py > "$WORK/host.log" 2>&1 &
HOST_PID=$!
sleep 0.5
if ! kill -0 "$HOST_PID" 2>/dev/null; then
    echo "host_server.py did not start:" >&2
    cat "$WORK/host.log" >&2
    exit 2
fi

LOG=$WORK/serial.log
: > "$LOG"
qemu-system-aarch64 \
    -machine virt,gic-version=3 \
    -cpu cortex-a57 \
    -smp "$SMP" \
    -m 4G \
    -bios "$OVMF" \
    -drive file="$WORK/boot.img",format=raw,if=virtio \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:47010-:7010,hostfwd=udp:127.0.0.1:47011-:7011 \
    -device "$DEVICE",netdev=net0 \
    -display none \
    -serial file:"$LOG" &
QEMU_PID=$!

# Wait for the test's summary line.
i=0
while [ $i -lt $((TIMEOUT * 5)) ]; do
    grep -aq '^\[net_test\] done' "$LOG" && break
    kill -0 "$QEMU_PID" 2>/dev/null || break
    sleep 0.2
    i=$((i + 1))
done

grep -a '^\[bsdsockets\]\|^\[net_test\]' "$LOG"
echo "--- host_server.py ---"
cat "$WORK/host.log"

if grep -aq '^\[net_test\] done: [0-9]* checks, 0 failure(s), 0 skipped' "$LOG"; then
    exit 0
fi
if ! grep -aq '^\[net_test\] done' "$LOG"; then
    echo "net_test did not finish within $TIMEOUT seconds" >&2
fi
exit 1
