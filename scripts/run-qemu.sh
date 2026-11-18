#!/bin/sh
set -e

# Usage: [MEM=4G] [KIPS=kips] [NVME=system.img] [NET=1] scripts/run-qemu.sh [kernel.bin]
#   MEM  sets the VM's RAM size (QEMU -m syntax, default 4G).
#   KIPS is the folder whose *.kip files go on the disk (default kips).
#   NVME attaches a raw image as an NVMe disk: the system storage the FS
#        module looks for. Create one with scripts/hzdisk.py. Default: none.
#   NET  set to 1, attaches a virtio network card on QEMU's user-mode network,
#        which the bsdsockets module drives. The guest gets 10.0.2.15 by DHCP
#        and reaches the host's loopback as 10.0.2.2. Host ports 47010 (TCP)
#        and 47011 (UDP) are forwarded to guest ports 7010 and 7011, which is
#        what sysmodules/tests/net_test expects. Default: QEMU's own default card,
#        which is the same kind on the same network with nothing forwarded.
# Builds the bootloader, packs a FAT disk image and boots it in QEMU.
# Run from anywhere; paths are relative to the project root.
cd "$(dirname "$0")/.."

BUILD=build
EFI=BOOTAA64.EFI
DISK=$BUILD/disk.img
: "${OVMF:?set OVMF to the AArch64 UEFI firmware file}"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/horizonuefi.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
TMP=$WORK/disk
KERNEL_BIN=${1:-$BUILD/kernel.bin}
MEM=${MEM:-4G}
KIPS=${KIPS:-kips}

if [ ! -f "$OVMF" ]; then
    echo "OVMF not found at $OVMF; set OVMF to the AArch64 UEFI firmware file"
    exit 1
fi

make

# Build FAT32 disk image with the EFI app at EFI/BOOT/BOOTAA64.EFI
rm -f "$TMP.dmg" "$DISK"
hdiutil create -size 64m -fs MS-DOS -volname BOOT -o "$TMP" >/dev/null
VOL=$(hdiutil attach "$TMP.dmg" | grep -oE '/Volumes/[^[:space:]]+')
mkdir -p "$VOL/EFI/BOOT"
cp "$BUILD/$EFI" "$VOL/EFI/BOOT/$EFI"
if [ -f "$KERNEL_BIN" ]; then
    cp "$KERNEL_BIN" "$VOL/kernel.bin"
else
    echo "Warning: $KERNEL_BIN not found, skipping"
fi
if [ -d "$KIPS" ] && [ -n "$(ls "$KIPS"/*.kip* 2>/dev/null)" ]; then
    mkdir -p "$VOL/kips"
    cp "$KIPS"/*.kip* "$VOL/kips/"
fi
hdiutil detach "$VOL" >/dev/null
hdiutil convert "$TMP.dmg" -format UDTO -o "$TMP" >/dev/null
mv "$TMP.cdr" "$DISK"
rm "$TMP.dmg"

# Optional system disk on an emulated NVMe controller.
NVME_ARGS=""
if [ -n "$NVME" ]; then
    if [ ! -f "$NVME" ]; then
        echo "NVME image $NVME not found"
        exit 1
    fi
    NVME_ARGS="-drive file=$NVME,if=none,id=nvme0,format=raw -device nvme,drive=nvme0,serial=horizon0"
fi

# Network card with port forwards. Without NET=1 nothing is passed, and QEMU adds its default
# card: also virtio on the user-mode network, but with no ports forwarded.
NET_ARGS=""
if [ "$NET" = "1" ]; then
    NET_ARGS="-netdev user,id=net0,hostfwd=tcp:127.0.0.1:47010-:7010,hostfwd=udp:127.0.0.1:47011-:7011 -device virtio-net-pci,netdev=net0"
fi

# $NVME_ARGS and $NET_ARGS are left unquoted on purpose: each is several arguments, or none.
qemu-system-aarch64 \
    -machine virt,gic-version=3 \
    -cpu cortex-a57 \
    -smp 4 \
    -m "$MEM" \
    -bios "$OVMF" \
    -drive file="$DISK",format=raw,if=virtio \
    $NVME_ARGS \
    $NET_ARGS \
    -display none \
    -serial stdio
