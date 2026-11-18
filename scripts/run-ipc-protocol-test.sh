#!/bin/sh
# Host-side tests for bounded HIPC envelope inspection.
set -eu
cd "$(dirname "$0")/.."
: "${DEVKITPRO:?set DEVKITPRO to the devkitPro installation directory}"
CXX=${CXX:-c++}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/horizon-ipc-protocol.XXXXXX")
trap 'rm -rf "$WORK"' EXIT INT TERM
"$CXX" -std=gnu++20 -Wno-invalid-constexpr -fsanitize=address,undefined -fno-sanitize-recover=all -I"$DEVKITPRO/libnx/include" -Isysmodules/common/include tests/ipc/protocol_test.cpp -o "$WORK/protocol_test"
"$WORK/protocol_test"
