#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
: "${DEVKITPRO:?set DEVKITPRO to the devkitPro installation directory}"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/kumo-ipc-server.XXXXXX")
trap 'rm -rf "$WORK"' EXIT INT TERM
${CXX:-c++} -std=c++23 -Wno-invalid-constexpr -fsanitize=address,undefined -fno-sanitize-recover=all -g \
 -Itests/ipc/mock -I"$DEVKITPRO/libnx/include" -Isysmodules/common/include \
 tests/ipc/server_test.cpp sysmodules/common/source/ipc/hz_ipc_concurrent_server.cpp sysmodules/common/source/ipc/hz_ipc_server.cpp -o "$WORK/test"
"$WORK/test"

# The valid fixture must compile before testing deliberate declaration failures.
check_contract() {
    ${CXX:-c++} -std=c++23 -Wno-invalid-constexpr -fsyntax-only \
      -Itests/ipc/mock -I"$DEVKITPRO/libnx/include" -Isysmodules/common/include \
      "$@" tests/ipc/compile_contract_test.cpp
}
check_contract
for rejection in DUPLICATE HANDLES CAPACITY WIRE; do
    if check_contract "-DTEST_$rejection" > "$WORK/rejection.log" 2>&1; then
        echo "Invalid $rejection declaration unexpectedly compiled" >&2
        exit 1
    fi
done
echo 'Command declaration rejection tests passed'
