#!/bin/sh
# Compile the shared hz library without Atmosphère's include paths or libraries.
set -eu
cd "$(dirname "$0")/.."
: "${DEVKITPRO:?set DEVKITPRO to the devkitPro installation directory}"
CXX="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-g++"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/horizon-hz-check.XXXXXX")
trap 'rm -rf "$WORK"' EXIT INT TERM
for src in sysmodules/common/source/platform/*.cpp sysmodules/common/source/pci/*.cpp \
           sysmodules/common/source/dma/*.cpp sysmodules/common/source/virtio/*.cpp \
           sysmodules/common/source/ipc/*.cpp sysmodules/common/source/fs/*.cpp; do
    obj="$WORK/$(basename "${src%.cpp}").o"
    "$CXX" -std=gnu++23 -Wall -Wextra -Werror -Wno-missing-field-initializers \
        -I"$DEVKITPRO/libnx/include" -Isysmodules/common/include -c "$src" -o "$obj"
done
for header in sysmodules/common/include/kumo/*.hpp sysmodules/common/include/kumo/ipc/*.hpp sysmodules/common/include/kumo/fs/*.hpp; do
    relative=${header#sysmodules/common/include/}
    printf '#include <%s>\n' "$relative" > "$WORK/header.cpp"
    "$CXX" -std=gnu++23 -Wall -Wextra -Werror -Wno-missing-field-initializers \
        -I"$DEVKITPRO/libnx/include" -Isysmodules/common/include -fsyntax-only "$WORK/header.cpp"
done
"$CXX" -std=gnu++23 -Wall -Wextra -Werror -Wno-missing-field-initializers \
    -I"$DEVKITPRO/libnx/include" -Isysmodules/common/include -fsyntax-only tests/ipc/result_test.cpp
printf '%s\n' 'Shared hz sources and headers compile without Atmosphere.'
