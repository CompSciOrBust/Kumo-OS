#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
: "${DEVKITPRO:?set DEVKITPRO to the devkitPro installation directory}"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/kumo-fs.XXXXXX")
trap 'rm -rf "$WORK"' EXIT INT TERM

PYTHONDONTWRITEBYTECODE=1 python3 - "$WORK" <<'PY'
from hashlib import sha256
from pathlib import Path
import struct
import sys

sys.path.insert(0, "scripts")
from hznca import align_up, build_hashed_section, build_nca, build_partition_fs

root = Path(sys.argv[1])
files = [("main", bytes(i % 251 for i in range(5000))), ("empty", b"")]
(root / "package.pfs").write_bytes(build_partition_fs(files))
(root / "program.nca").write_bytes(build_nca(0, 0x0100000000001000, files))

# One hash bucket of each kind. The root owns "sub", which owns "data".
end = 0xffffffff
directory_entries = struct.pack("<6I", 0, end, 24, end, end, 0)
directory_entries += struct.pack("<6I", 0, end, end, 0, 0, 3) + b"sub\0"
payload = b"hello from RomFS"
file_entries = struct.pack("<IIQQII", 24, end, 0, len(payload), end, 4) + b"data"
body_offset = 0x50
offset = body_offset + len(payload)
rom_header = struct.pack("<Q", body_offset)
rom_tables = b""
for table in (struct.pack("<I", 24), directory_entries, struct.pack("<I", 0), file_entries):
    rom_header += struct.pack("<QQ", offset, len(table))
    rom_tables += table
    offset += len(table)
romfs = rom_header + struct.pack("<Q", body_offset) + payload + rom_tables
(root / "romfs.bin").write_bytes(romfs)
# Exercise the generated Program NCA with both ExeFS and a multi-level RomFS hash tree.
program_romfs = romfs.ljust(0x80001, b"\0")
(root / "program-romfs.nca").write_bytes(build_nca(0, 0x0100000000001000, files, program_romfs))

# A short top layer exercises IVFC's zero padding at both levels.
block_size = 0x200
digest = lambda data: sha256(data.ljust(block_size, b"\0")).digest()
hash_table = digest(romfs)
fs_header = struct.pack("<HBBBB2x4sIII", 2, 0, 3, 1, 0, b"IVFC", 0x20000, 32, 3)
fs_header += struct.pack("<QQI4xQQI4x", 0, len(hash_table), 9, block_size, len(romfs), 9)
fs_header = (fs_header.ljust(0xc8, b"\0") + digest(hash_table)).ljust(0x200, b"\0")
section = hash_table.ljust(block_size, b"\0") + romfs
section = section.ljust(align_up(len(section), 0x200), b"\0")
nca_header = bytearray((root / "program.nca").read_bytes()[:0x400])
struct.pack_into("<Q", nca_header, 0x208, 0xc00 + len(section))
struct.pack_into("<II", nca_header, 0x240, 6, 6 + len(section) // 0x200)
nca_header[0x280:0x2a0] = sha256(fs_header).digest()
(root / "romfs.nca").write_bytes(nca_header + fs_header.ljust(0x800, b"\0") + section)

def bucket_tree(entries, logical_size):
    node_size = 0x4000
    index = struct.pack("<IIQQ", 0, 1, logical_size, 0)
    leaf = struct.pack("<IIQ", 0, len(entries), logical_size) + b"".join(entries)
    return index.ljust(node_size, b"\0") + leaf.ljust(node_size, b"\0")

def write_nca(name, header, body):
    body = body.ljust(align_up(len(body), 0x200), b"\0")
    struct.pack_into("<Q", nca_header, 0x208, 0xc00 + len(body))
    struct.pack_into("<II", nca_header, 0x240, 6, 6 + len(body) // 0x200)
    nca_header[0x280:0x2a0] = sha256(header).digest()
    (root / name).write_bytes(nca_header + header.ljust(0x800, b"\0") + body)

def bucket_header(offset, table, count):
    return struct.pack("<QQ4sIII", offset, len(table), b"BKTR", 1, count, 0)

# Logical PFS0 data spans a raw extent, a zero extent and an LZ4 block.
source = build_partition_fs([("mixed", b"123" + bytes(7) + b"A" * 11 + b"abcde")])
raw = source[:-23]
compressed_offset = align_up(len(raw), 16)
encoded = b"\x16A\x01\x00\x50abcde"
table_offset = 0x200
entries = [struct.pack("<QQII", 0, 0, 0, len(raw)),
           struct.pack("<QQII", len(raw), 0, 1, 0),
           struct.pack("<QQII", len(raw) + 7, compressed_offset, 3, len(encoded))]
table = bucket_tree(entries, len(source))
data = (raw.ljust(compressed_offset, b"\0") + encoded).ljust(table_offset, b"\0") + table
body, header = build_hashed_section(data)
header = bytearray(header)
header[0x178:0x198] = bucket_header(table_offset, table, len(entries))
write_nca("compression.nca", header, body)

# A patch-only NCA stores its first two logical sectors in reverse order.
# The existing SHA-256 layer authenticates the address-translated data.
body, header = build_hashed_section(build_partition_fs(files))
patched = body[0x200:0x400] + body[:0x200] + body[0x400:]
entries = [struct.pack("<QQI", 0, 0x200, 1),
           struct.pack("<QQI", 0x200, 0, 1),
           struct.pack("<QQI", 0x400, 0x400, 1)]
table = bucket_tree(entries, len(body))
header = bytearray(header)
header[0x100:0x120] = bucket_header(len(body), table, len(entries))
write_nca("patch.nca", header, patched + table)
PY

if [ "$(uname -s)" = Darwin ]; then
    set --
else
    set -- -lcrypto
fi
${CXX:-c++} -std=c++23 -Wno-invalid-constexpr -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -include switch/crypto/sha256.h -Itests/ipc/mock -I"$DEVKITPRO/libnx/include" -Isysmodules/common/include \
    tests/fs/kumo_fs_test.cpp sysmodules/common/source/fs/*.cpp "$@" -o "$WORK/test"
"$WORK/test" "$WORK/package.pfs" "$WORK/program.nca" "$WORK/romfs.bin" "$WORK/romfs.nca" "$WORK/compression.nca" "$WORK/patch.nca" "$WORK/program-romfs.nca"
