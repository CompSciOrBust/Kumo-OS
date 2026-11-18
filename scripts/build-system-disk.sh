#!/bin/sh
set -e

# Usage: [KIPS=kips] [LAYOUT=...] scripts/build-system-disk.sh <image> [size]
#   Builds one bootable disk image that holds both the EFI system partition
#   (bootloader, kernel, KIPs) and the partitions Kumo's FS module uses.
#   This is the layout for machines whose only disk is the boot disk, such as
#   cloud instances. size defaults to 1G.
#   KIPS   is the folder whose *.kip files go on the disk (default kips).
#   LAYOUT overrides the partitions after the EFI system partition
#          (scripts/hzdisk.py --layout syntax).
# Build the bootloader and kernel first (make bootloader, make kernel).
# Run from anywhere; paths are relative to the project root.
cd "$(dirname "$0")/.."

IMAGE=$1
SIZE=${2:-1G}
KIPS=${KIPS:-kips}
LAYOUT=${LAYOUT:-PRODINFO:4M:raw,PRODINFOF:4M:fat,SAFE:64M:fat,SYSTEM:256M:fat,USER:256M:fat,SDCARD:rest:fat}
BUILD=build
EFI=BOOTAA64.EFI
HZDISK=scripts/hzdisk.py

if [ -z "$IMAGE" ]; then
    echo "usage: scripts/build-system-disk.sh <image> [size]"
    exit 1
fi
for f in "$BUILD/$EFI" "$BUILD/kernel.bin"; do
    if [ ! -f "$f" ]; then
        echo "$f not found: build it first"
        exit 1
    fi
done

python3 "$HZDISK" create "$IMAGE" --size "$SIZE" --layout "ESP:64M:esp,$LAYOUT"

# The firmware starts EFI/BOOT/BOOTAA64.EFI, which loads kernel.bin and kips/ from the same partition.
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/EFI/BOOT" "$STAGE/kips"
cp "$BUILD/$EFI" "$STAGE/EFI/BOOT/$EFI"
cp "$BUILD/kernel.bin" "$STAGE/kernel.bin"
if [ -d "$KIPS" ] && [ -n "$(ls "$KIPS"/*.kip* 2>/dev/null)" ]; then
    cp "$KIPS"/*.kip* "$STAGE/kips/"
else
    echo "Warning: no KIPs in $KIPS"
fi
python3 "$HZDISK" put "$IMAGE" ESP "$STAGE" /

echo "Contents of the EFI system partition:"
python3 "$HZDISK" ls "$IMAGE" ESP
