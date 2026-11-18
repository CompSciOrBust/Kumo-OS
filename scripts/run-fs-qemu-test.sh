#!/bin/sh
# Usage: [SECTOR_SIZE=512] [TIMEOUT=90] [OVMF=path] scripts/run-fs-qemu-test.sh fs_test|content_fs_test|content_test|launch_test [scratch-nvme-image]
# fs_test writes raw storage and formats SDCARD. Use only a scratch NVMe image.
# With no NVMe image, fs_test exercises the module's RAM-disk fallback.
set -eu
case "${1:-}" in
    fs_test|content_fs_test|content_test|launch_test) TEST=$1 ;;
    *) echo "Specify fs_test, content_fs_test, content_test or launch_test." >&2; exit 2 ;;
esac
NVME=${2:-}
if [ -n "$NVME" ]; then
    NVME="$(cd "$(dirname "$NVME")" && pwd)/$(basename "$NVME")"
    [ -f "$NVME" ] || { echo "Missing scratch NVMe image $NVME" >&2; exit 2; }
elif [ "$TEST" != fs_test ]; then
    echo "$TEST needs a scratch NVMe image with installed content." >&2
    exit 2
fi

cd "$(dirname "$0")/.."
OUT=out/nintendo_nx_arm64_armv8a/release
: "${OVMF:?set OVMF to the AArch64 UEFI firmware file}"
KIPS="vendor/atmosphere/stratosphere/sm/$OUT/sm.kip
vendor/atmosphere/stratosphere/pm/$OUT/pm.kip
sysmodules/fs/$OUT/fs.kip
sysmodules/tests/$TEST/$OUT/$TEST.kip"
if [ "$TEST" = launch_test ]; then
    for module in spl ncm loader; do
        KIPS="$KIPS vendor/atmosphere/stratosphere/$module/$OUT/$module.kip"
    done
fi
for file in "$OVMF" build/BOOTAA64.EFI build/kernel.bin $KIPS; do
    [ -f "$file" ] || { echo "Missing $file. Build the bootloader, kernel and required modules first." >&2; exit 2; }
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/horizon-fs-test.XXXXXX")
QEMU_PID=
cleanup() {
    if [ -n "$QEMU_PID" ]; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$WORK/root/EFI/BOOT" "$WORK/root/kips"
cp build/BOOTAA64.EFI "$WORK/root/EFI/BOOT/"
cp build/kernel.bin "$WORK/root/"
for kip in $KIPS; do cp "$kip" "$WORK/root/kips/"; done
python3 scripts/hzdisk.py create "$WORK/boot.img" --size 96M --layout 'ESP:rest:esp' >/dev/null
python3 scripts/hzdisk.py put "$WORK/boot.img" ESP "$WORK/root" / >/dev/null

set --
if [ -n "$NVME" ]; then
    sector_size=${SECTOR_SIZE:-512}
    set -- -drive "file=$NVME,if=none,id=fsdisk,format=raw" \
        -device "nvme,drive=fsdisk,serial=fs-test,logical_block_size=$sector_size,physical_block_size=$sector_size"
fi
LOG=$WORK/serial.log
: > "$LOG"
qemu-system-aarch64 -machine virt,gic-version=3 -cpu cortex-a57 -smp 4 -m 4G \
    -bios "$OVMF" -drive "file=$WORK/boot.img,format=raw,if=virtio" "$@" \
    -nic none -display none -serial "file:$LOG" &
QEMU_PID=$!

finished() {
    if [ "$TEST" = launch_test ]; then
        grep -aq '^\[launch_test\] done' "$LOG" && grep -aq '^\[hello_program\].*read back.*\(PASS\|FAIL\)' "$LOG"
    else
        grep -aq "^\[$TEST\] done:" "$LOG"
    fi
}
elapsed=0
while [ "$elapsed" -lt "${TIMEOUT:-90}" ]; do
    if finished; then break; fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then break; fi
    sleep 1
    elapsed=$((elapsed + 1))
done
grep -a "^\[fs\]\|^\[$TEST\]\|^\[hello_program\]" "$LOG" || true
if ! finished; then
    echo "$TEST did not finish." >&2
    tail -n 40 "$LOG" >&2
    exit 1
fi
if [ "$TEST" = launch_test ]; then
    grep -aq '^\[hello_program\].*read back.*PASS' "$LOG" &&
        ! grep -aq '^\[launch_test\] FAIL' "$LOG"
else
    grep -aq "^\[$TEST\] done: [0-9]* checks, 0 failure(s)" "$LOG"
fi
