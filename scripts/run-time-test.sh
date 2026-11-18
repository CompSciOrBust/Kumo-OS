#!/bin/sh
# Usage: [TIMEOUT=150] [SMP=4] [NTP=internet|fake|none] [QEMU_NET_DEVICE=virtio-net-pci] [PSC_KIP=path/to/psc.kip] scripts/run-time-test.sh
#   SMP is the number of CPUs QEMU gives the machine (default 4). The kernel
#   uses at most four and runs on fewer.
#   NTP picks what the SNTP client in psc can reach:
#     internet (default)  QEMU's user-mode network, with psc as built: it asks
#                         169.254.169.254 (does not exist here) and then
#                         216.239.35.0 (Google's public NTP), which works if the
#                         host is online. Otherwise the network checks are SKIPped.
#     fake                the same, but with sysmodules/tests/time_test/host_ntp.py
#                         answering on the host at 10.0.2.2:47123. psc must have
#                         been built to ask there:
#                           make -C sysmodules/psc clean
#                           make -C sysmodules/psc nx_release ATMOSPHERE_CHECKED_LIBSTRATOSPHERE=1 KUMO_NTP_SERVERS="10.0.2.2:47123"
#     none                no network device: the network checks are SKIPped.
#
# Runs sysmodules/tests/time_test against psc (the time core) and glue in QEMU
# and reports the result. Exits 0 only if the test finished with no failures
# (skipped checks are not failures), and, when a network time was read, the
# host's clock agreed with it to 10 seconds.
#
# Like run-net-test.sh it touches nothing shared: it builds its own boot image
# in a temporary folder from build/BOOTAA64.EFI, build/kernel.bin and the KIPs
# it needs straight from their build folders, so kips/ and build/disk.img are
# left alone. No disk partitions beyond the EFI one.
#
# Build first:
#   make bootloader && make kernel
#   make -C vendor/atmosphere/stratosphere/sm nx_release ATMOSPHERE_CHECKED_LIBSTRATOSPHERE=1
#   make -C vendor/atmosphere/stratosphere/pm nx_release ATMOSPHERE_CHECKED_LIBSTRATOSPHERE=1
#   make -C sysmodules/fs         nx_release ATMOSPHERE_CHECKED_LIBSTRATOSPHERE=1
#   make -C sysmodules/bsdsockets nx_release ATMOSPHERE_CHECKED_LIBSTRATOSPHERE=1
#   make -C sysmodules/psc        nx_release ATMOSPHERE_CHECKED_LIBSTRATOSPHERE=1
#   make -C sysmodules/glue       nx_release ATMOSPHERE_CHECKED_LIBSTRATOSPHERE=1
#   make -C sysmodules/tests/time_test
cd "$(dirname "$0")/.."

: "${OVMF:?set OVMF to the AArch64 UEFI firmware file}"
TIMEOUT=${TIMEOUT:-150}
SMP=${SMP:-4}
NTP=${NTP:-internet}
DEVICE=${QEMU_NET_DEVICE:-virtio-net-pci}
OUT=out/nintendo_nx_arm64_armv8a/release
PSC_KIP=${PSC_KIP:-sysmodules/psc/$OUT/psc.kip}

KIPS="vendor/atmosphere/stratosphere/sm/$OUT/sm.kip
vendor/atmosphere/stratosphere/pm/$OUT/pm.kip
sysmodules/fs/$OUT/fs.kip
sysmodules/bsdsockets/$OUT/bsdsockets.kip
sysmodules/glue/$OUT/glue.kip
sysmodules/tests/time_test/$OUT/time_test.kip"

case "$NTP" in
    internet|fake|none) ;;
    *) echo "NTP must be internet, fake or none" >&2; exit 2 ;;
esac

for f in "$OVMF" build/BOOTAA64.EFI build/kernel.bin "$PSC_KIP" $KIPS; do
    if [ ! -f "$f" ]; then
        echo "missing $f (see the build steps at the top of this script)" >&2
        exit 2
    fi
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/horizon-time-test.XXXXXX")
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
cp "$PSC_KIP" "$WORK/root/kips/psc.kip"
scripts/hzdisk.py create "$WORK/boot.img" --size 96M --layout "ESP:rest:esp" >/dev/null || exit 2
scripts/hzdisk.py put "$WORK/boot.img" ESP "$WORK/root" / >/dev/null || exit 2

# The host side: a fake NTP server, if asked for.
if [ "$NTP" = fake ]; then
    sysmodules/tests/time_test/host_ntp.py 47123 > "$WORK/host.log" 2>&1 &
    HOST_PID=$!
    sleep 0.5
    if ! kill -0 "$HOST_PID" 2>/dev/null; then
        echo "host_ntp.py did not start:" >&2
        cat "$WORK/host.log" >&2
        exit 2
    fi
    echo "NTP=fake: psc must have been built with KUMO_NTP_SERVERS=\"10.0.2.2:47123\" (see the top of this script)" >&2
fi

if [ "$NTP" = none ]; then
    NETWORK="-nic none"
else
    NETWORK="-netdev user,id=net0 -device $DEVICE,netdev=net0"
fi

LOG=$WORK/serial.log
: > "$LOG"
# shellcheck disable=SC2086
qemu-system-aarch64 \
    -machine virt,gic-version=3 \
    -cpu cortex-a57 \
    -smp "$SMP" \
    -m 4G \
    -bios "$OVMF" \
    -drive file="$WORK/boot.img",format=raw,if=virtio \
    $NETWORK \
    -display none \
    -serial file:"$LOG" &
QEMU_PID=$!

# Wait for the test's summary line.
i=0
while [ $i -lt $((TIMEOUT * 5)) ]; do
    grep -aq '^\[time_test\] done' "$LOG" && break
    kill -0 "$QEMU_PID" 2>/dev/null || break
    sleep 0.2
    i=$((i + 1))
done
HOST_NOW=$(date -u +%s)

grep -a '^\[psc\]\|^\[glue\]\|^\[bsdsockets\]\|^\[time_test\]' "$LOG"
if [ -n "$HOST_PID" ]; then
    echo "--- host_ntp.py ---"
    cat "$WORK/host.log"
fi

if ! grep -aq '^\[time_test\] done' "$LOG"; then
    echo "time_test did not finish within $TIMEOUT seconds" >&2
    echo "--- last lines of the serial log ---" >&2
    tail -n 40 "$LOG" >&2
    exit 1
fi

if ! grep -aq '^\[time_test\] done: [0-9]* checks, 0 failure(s)' "$LOG"; then
    exit 1
fi

# The network time the test read, advanced to the moment the summary appeared (a fraction of a second after it), against the host's clock.
NET=$(grep -a '^\[time_test\] final network time ' "$LOG" | tail -n 1 | tr -d '\r' | sed 's/.* //')
if [ -n "$NET" ]; then
    DIFF=$((HOST_NOW - NET))
    [ "$DIFF" -lt 0 ] && DIFF=$((-DIFF))
    echo "network time $NET, host time $HOST_NOW, difference ${DIFF}s"
    if [ "$DIFF" -gt 10 ]; then
        echo "the network clock is more than 10 seconds from the host's" >&2
        exit 1
    fi
else
    echo "no network time was read (the network checks were skipped)"
fi
exit 0
