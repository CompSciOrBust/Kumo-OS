#!/bin/sh
set -e

# Usage: [MEM=4G] [NET=1] [CPU=cortex-a57] [SMP=4] [TIMEOUT=seconds] scripts/run-os.sh [image]
#   Boots a disk image built by scripts/build-os.py in QEMU. The image is the
#   machine's only disk, an NVMe one, as on a cloud instance: the firmware
#   starts the bootloader from its EFI partition and FS finds the Horizon
#   partitions on the same disk. Default image: build/os/default/disk.img.
#   OVMF    path to the AArch64 UEFI firmware file (required).
#   NET     defaults to 1: forward host ports 47010 (TCP) and 47011 (UDP)
#           to guest ports 7010 and 7011. Set NET=0 to disable forwarding.
#   CPU     QEMU CPU model and properties, e.g. "cortex-a57,pmu=off" or
#           "neoverse-n1".
#   MACHINE extra QEMU machine properties, e.g. "highmem=off", which puts the
#           PCI configuration space below 4 GiB.
#   SMP     number of CPUs (default 4). The kernel uses at most four and
#           runs on fewer.
#   TIMEOUT stop QEMU after this many seconds (default: run until interrupted).
# The image is written to: the guest's changes persist across runs.
cd "$(dirname "$0")/.."

IMAGE=${1:-build/os/default/disk.img}
: "${OVMF:?set OVMF to the AArch64 UEFI firmware file}"
MEM=${MEM:-4G}
CPU=${CPU:-cortex-a57}
SMP=${SMP:-4}
NET=${NET:-1}

[ -f "$IMAGE" ] || { echo "$IMAGE not found: build it with scripts/build-os.py"; exit 1; }
[ -f "$OVMF" ] || { echo "OVMF not found at $OVMF; set OVMF to the AArch64 UEFI firmware file"; exit 1; }

NET_ARGS="-netdev user,id=net0 -device virtio-net-pci,netdev=net0"
if [ "$NET" = "1" ]; then
    NET_ARGS="-netdev user,id=net0,hostfwd=tcp:127.0.0.1:47010-:7010,hostfwd=udp:127.0.0.1:47011-:7011 -device virtio-net-pci,netdev=net0"
fi

# $NET_ARGS is left unquoted on purpose: it is several arguments.
set -- qemu-system-aarch64 \
    -machine "virt,gic-version=3${MACHINE:+,$MACHINE}" \
    -cpu "$CPU" \
    -smp "$SMP" \
    -m "$MEM" \
    -bios "$OVMF" \
    -drive file="$IMAGE",if=none,id=nvme0,format=raw \
    -device nvme,drive=nvme0,serial=horizon0 \
    $NET_ARGS \
    -display none \
    -serial stdio

if [ -n "$TIMEOUT" ]; then
    "$@" < /dev/null &
    QEMU=$!
    ( sleep "$TIMEOUT"; kill "$QEMU" 2>/dev/null ) &
    wait "$QEMU" 2>/dev/null || true
else
    exec "$@"
fi
