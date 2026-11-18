#!/bin/sh
# Uses a fresh scratch image. Prerequisites: bootloader, kernel, sm, pm and FS builds.
set -eu
cd "$(dirname "$0")/.."
WORK=$(mktemp -d "${TMPDIR:-/tmp}/kumo-object-qemu.XXXXXX")
QEMU_PID=""
cleanup() { if [ -n "$QEMU_PID" ]; then kill "$QEMU_PID" 2>/dev/null || true; wait "$QEMU_PID" 2>/dev/null || true; fi; rm -rf "$WORK"; }
trap cleanup EXIT INT TERM
: "${OVMF:?set OVMF to the AArch64 UEFI firmware file}"
make -C sysmodules/tests/ipc_object_test > "$WORK/module-build.log" 2>&1 || { cat "$WORK/module-build.log" >&2; exit 1; }
python3 scripts/build-os.py configs/ipc-object-test.conf --no-kernel --no-modules --out "$WORK/image" > "$WORK/build.log" 2>&1
qemu-system-aarch64 -machine virt,gic-version=3 -cpu cortex-a57 -smp 4 -m 4G \
 -bios "$OVMF" \
 -drive "file=$WORK/image/disk.img,if=none,id=disk,format=raw" \
 -device nvme,drive=disk,serial=kumo-test -nic none -display none -serial "file:$WORK/serial.log" &
QEMU_PID=$!
i=0
while [ "$i" -lt "${TIMEOUT:-60}" ]; do
    if grep -aq '\[ipc_object_test\] done:' "$WORK/serial.log" 2>/dev/null; then break; fi
    kill -0 "$QEMU_PID" 2>/dev/null || break
    sleep 1
    i=$((i+1))
done
grep -a '\[ipc_object_test\]' "$WORK/serial.log" || true
grep -aq '\[ipc_object_test\] done: [0-9]* checks, 0 failure(s)' "$WORK/serial.log"
