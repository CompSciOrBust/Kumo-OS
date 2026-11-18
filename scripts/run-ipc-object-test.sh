#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
: "${DEVKITPRO:?set DEVKITPRO to the devkitPro installation directory}"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/kumo-ipc-object.XXXXXX")
trap 'rm -rf "$WORK"' EXIT INT TERM
${CXX:-c++} -std=c++23 -Wno-invalid-constexpr -fsanitize=address,undefined -fno-sanitize-recover=all -g \
 -Itests/ipc/mock -I"$DEVKITPRO/libnx/include" -Isysmodules/common/include \
 tests/ipc/object_server_test.cpp sysmodules/common/source/ipc/kumo_ipc_object_server.cpp -o "$WORK/test"
"$WORK/test"
