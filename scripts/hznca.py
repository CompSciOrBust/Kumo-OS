#!/usr/bin/env python3
"""Package programs as NCAs for a Kumo system disk.

Kumo uses Horizon-style content archives (NCAs) in a content storage, where
ncm indexes them and the loader runs them. This tool builds and installs the
two NCAs used to package a program for Kumo:

    hznca.py program --id 0x0100000000001000 --code ./exefs --out ./ncas
    hznca.py install disk.img ./ncas                 # into SYSTEM's content storage
    hznca.py install disk.img ./ncas --storage sd    # or USER / the SD card
    hznca.py info    ./ncas/<content id>.nca         # show and verify an NCA

"program" takes a directory holding main, main.npdm and any rtld / sdk /
subsdk* modules, and writes:

  * a Program NCA, whose section 0 is that directory as a partition filesystem;
  * a Meta NCA, holding the content metadata (.cnmt) that tells ncm the
    program's ID, version and which content belongs to it.

Both are named <content id>.nca, where the content ID is the first 16 bytes
of the file's SHA-256, as ncm expects.

The NCAs follow the Horizon container layout but use a plaintext header,
unencrypted sections and no signatures, as supported by Kumo's FS module
(sysmodules/fs/source/nca). Section data is covered by hierarchical SHA-256
hashes. These Kumo-specific archives are not general Horizon-compatible NCAs;
they will not load on a Switch and may not work on other Horizon-compatible
systems. Kumo does not support loading retail encrypted Switch NCAs.
"""

import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HZDISK = os.path.join(ROOT, "scripts", "hzdisk.py")

SECTOR = 0x200
NCA_HEADER_SIZE = 0x400
FS_HEADER_SIZE = 0x200
FS_HEADER_COUNT = 4
BODY_OFFSET = NCA_HEADER_SIZE + FS_HEADER_SIZE * FS_HEADER_COUNT

# The oldest SDK add-on version the reader accepts is 0.11.0.0.
SDK_ADDON_VERSION = 0x000C0000

# fssystem::NcaHeader::ContentType
NCA_TYPE_PROGRAM = 0
NCA_TYPE_META = 1
NCA_TYPE_NAMES = {0: "Program", 1: "Meta", 2: "Control", 3: "Manual", 4: "Data", 5: "PublicData"}

# fssystem::NcaFsHeader
FS_TYPE_PARTITION = 1
HASH_TYPE_HIERARCHICAL_SHA256 = 2
ENCRYPTION_TYPE_NONE = 1

# ncm::ContentMetaType, ncm::ContentType
META_TYPE_SYSTEM_PROGRAM = 0x01
CONTENT_TYPE_PROGRAM = 1

# Hash blocks start at 4 KiB and double until a section's hash table fits in this.
# FS keeps the table of every open section in memory.
MIN_HASH_BLOCK_SIZE = 0x1000
MAX_HASH_TABLE_SIZE = 0x8000

# Where each content storage lives, and how ncm lays out files below "registered".
STORAGES = {
    "system": ("SYSTEM", "Contents", "flat"),
    "user": ("USER", "Contents", "hashed"),
    "sd": ("SDCARD", "Nintendo/Contents", "hashed"),
}


def fail(message):
    print(f"hznca: {message}", file=sys.stderr)
    sys.exit(1)


def align_up(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def sha256(data):
    return hashlib.sha256(data).digest()


def build_partition_fs(files):
    """A PFS0 image of (name, data) pairs."""
    names = b""
    entries = b""
    offset = 0
    for name, data in files:
        entries += struct.pack("<QQII", offset, len(data), len(names), 0)
        names += name.encode("utf-8") + b"\0"
        offset += len(data)

    # Pad the name table so that file data starts on a 32-byte boundary.
    header_size = 0x10 + len(entries) + len(names)
    names += b"\0" * (align_up(header_size, 0x20) - header_size)

    header = struct.pack("<4sIII", b"PFS0", len(files), len(names), 0)
    return header + entries + names + b"".join(data for _, data in files)


def build_hashed_section(data):
    """Wrap data in a hierarchical SHA-256 section. Returns (section bytes, fs header)."""
    block_size = MIN_HASH_BLOCK_SIZE
    while (len(data) + block_size - 1) // block_size * 0x20 > MAX_HASH_TABLE_SIZE:
        block_size *= 2

    table = b"".join(sha256(data[i:i + block_size]) for i in range(0, len(data), block_size))
    data_offset = align_up(len(table), SECTOR)

    section = table.ljust(data_offset, b"\0") + data
    section = section.ljust(align_up(len(section), SECTOR), b"\0")

    hash_data = sha256(table) + struct.pack("<ii", block_size, 2)
    hash_data += struct.pack("<qq", 0, len(table))
    hash_data += struct.pack("<qq", data_offset, len(data))

    header = struct.pack("<HBBBB2x", 2, FS_TYPE_PARTITION, HASH_TYPE_HIERARCHICAL_SHA256, ENCRYPTION_TYPE_NONE, 0)
    header += hash_data
    return section, header.ljust(FS_HEADER_SIZE, b"\0")


def build_nca(content_type, program_id, files):
    """An NCA whose section 0 is a partition filesystem of files."""
    section, fs_header = build_hashed_section(build_partition_fs(files))
    content_size = BODY_OFFSET + len(section)

    fs_info = struct.pack("<IIII", BODY_OFFSET // SECTOR, content_size // SECTOR, 0, 0)

    header = b"\0" * 0x200                                      # the two signatures: unsigned
    header += struct.pack("<4sBBBB", b"NCA3", 0, content_type, 0, 0)
    header += struct.pack("<QQII", content_size, program_id, 0, SDK_ADDON_VERSION)
    header += b"\0" * 0x10                                      # key generations, reserved
    header += b"\0" * 0x10                                      # rights ID: none
    header += fs_info.ljust(0x10 * FS_HEADER_COUNT, b"\0")
    header += sha256(fs_header).ljust(0x20 * FS_HEADER_COUNT, b"\0")
    header += b"\0" * 0x100                                     # key area: unused
    assert len(header) == NCA_HEADER_SIZE

    return header + fs_header.ljust(FS_HEADER_SIZE * FS_HEADER_COUNT, b"\0") + section


def content_id(nca):
    return sha256(nca)[:16]


def build_content_meta(program_id, version, program_nca):
    """The packaged content metadata (.cnmt) of a system program with one Program content."""
    header = struct.pack("<QIBBHHHBBBBI4x",
                         program_id, version, META_TYPE_SYSTEM_PROGRAM, 0,
                         0,          # extended header size: system programs have none
                         1,          # content count
                         0,          # content meta count
                         0, 0, 0, 0, 0)
    size = len(program_nca)
    info = sha256(program_nca) + content_id(program_nca)
    info += struct.pack("<IBBBB", size & 0xFFFFFFFF, size >> 32, 0, CONTENT_TYPE_PROGRAM, 0)
    digest = b"\0" * 0x20
    return header + info + digest


def read_code_directory(path):
    if not os.path.isdir(path):
        fail(f"{path} is not a directory")
    names = sorted(name for name in os.listdir(path) if os.path.isfile(os.path.join(path, name)) and not name.startswith("."))
    for required in ("main", "main.npdm"):
        if required not in names:
            fail(f"{path} has no {required}")
    files = []
    for name in names:
        with open(os.path.join(path, name), "rb") as f:
            files.append((name, f.read()))
    return files


def cmd_program(args):
    program_id = int(args.id, 0)
    program_nca = build_nca(NCA_TYPE_PROGRAM, program_id, read_code_directory(args.code))

    meta_name = f"SystemProgram_{program_id:016x}.cnmt"
    meta_nca = build_nca(NCA_TYPE_META, program_id, [(meta_name, build_content_meta(program_id, args.version, program_nca))])

    os.makedirs(args.out, exist_ok=True)
    for kind, nca in (("program", program_nca), ("meta", meta_nca)):
        path = os.path.join(args.out, content_id(nca).hex() + ".nca")
        with open(path, "wb") as f:
            f.write(nca)
        print(f"{kind:8} {path}")


def registered_path(name, layout):
    """Where ncm keeps the content file `name` below "registered"."""
    if layout == "flat":
        return name
    # ncm's 16 KiB-cluster layout: one directory per first byte of the content ID's SHA-256.
    cid = bytes.fromhex(name[:-len(".nca")])
    return f"{sha256(cid)[0]:08X}/{name}"


def cmd_install(args):
    partition, directory, layout = STORAGES[args.storage]

    sources = []
    for source in args.ncas:
        if os.path.isdir(source):
            sources += sorted(os.path.join(source, name) for name in os.listdir(source) if name.endswith(".nca"))
        else:
            sources.append(source)
    if not sources:
        fail("nothing to install")

    with tempfile.TemporaryDirectory() as stage:
        for source in sources:
            with open(source, "rb") as f:
                nca = f.read()
            # Name the file by its content, whatever it was called on the host.
            name = content_id(nca).hex() + ".nca"
            target = os.path.join(stage, directory, "registered", registered_path(name, layout))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            shutil.copyfile(source, target)
            print(f"{partition}:/{os.path.relpath(target, stage)}")

        subprocess.run([sys.executable, HZDISK, "put", args.image, partition, stage, "/"], check=True, stdout=subprocess.DEVNULL)


def cmd_info(args):
    with open(args.nca, "rb") as f:
        nca = f.read()
    if len(nca) < BODY_OFFSET or nca[0x200:0x204] != b"NCA3":
        fail(f"{args.nca} is not a plaintext NCA")

    content_type = nca[0x205]
    content_size, program_id = struct.unpack_from("<QQ", nca, 0x208)
    print(f"content id   {content_id(nca).hex()}")
    print(f"type         {NCA_TYPE_NAMES.get(content_type, content_type)}")
    print(f"program id   {program_id:016x}")
    print(f"size         {content_size}" + ("" if content_size == len(nca) else f" (file is {len(nca)})"))

    ok = content_size == len(nca)
    for index in range(FS_HEADER_COUNT):
        start, end = struct.unpack_from("<II", nca, 0x240 + 0x10 * index)
        if start == 0 and end == 0:
            continue
        fs_header = nca[NCA_HEADER_SIZE + FS_HEADER_SIZE * index:][:FS_HEADER_SIZE]
        header_ok = sha256(fs_header) == nca[0x280 + 0x20 * index:][:0x20]

        section = nca[start * SECTOR:end * SECTOR]
        block_size, layers = struct.unpack_from("<ii", fs_header, 0x28)
        table_offset, table_size, data_offset, data_size = struct.unpack_from("<qqqq", fs_header, 0x30)
        table = section[table_offset:table_offset + table_size]
        data = section[data_offset:data_offset + data_size]
        data_ok = (fs_header[3] == HASH_TYPE_HIERARCHICAL_SHA256 and layers == 2 and
                   sha256(table) == fs_header[0x8:0x28] and
                   table == b"".join(sha256(data[i:i + block_size]) for i in range(0, len(data), block_size)))
        ok = ok and header_ok and data_ok
        print(f"section {index}    {len(section)} bytes, hash block {block_size:#x}, "
              f"header {'ok' if header_ok else 'BAD'}, data {'ok' if data_ok else 'BAD'}")

        if data[:4] == b"PFS0":
            count, names_size = struct.unpack_from("<II", data, 4)
            names = data[0x10 + 0x18 * count:][:names_size]
            for i in range(count):
                _, size, name_offset, _ = struct.unpack_from("<QQII", data, 0x10 + 0x18 * i)
                name = names[name_offset:names.index(b"\0", name_offset)].decode("utf-8")
                print(f"  {size:>10}  {name}")

    if not ok:
        fail("verification failed")


def main():
    parser = argparse.ArgumentParser(description="Package programs as NCAs for a Kumo system disk.")
    commands = parser.add_subparsers(dest="command", required=True)

    program = commands.add_parser("program", help="build the Program and Meta NCAs of a system program")
    program.add_argument("--id", required=True, help="program ID, e.g. 0x0100000000001000")
    program.add_argument("--code", required=True, help="directory with main, main.npdm and other modules")
    program.add_argument("--version", type=lambda text: int(text, 0), default=0, help="content version (default 0)")
    program.add_argument("--out", required=True, help="directory to write the NCAs to")
    program.set_defaults(handler=cmd_program)

    install = commands.add_parser("install", help="copy NCAs into a disk image's content storage")
    install.add_argument("image")
    install.add_argument("ncas", nargs="+", help="NCA files, or directories of them")
    install.add_argument("--storage", choices=sorted(STORAGES), default="system")
    install.set_defaults(handler=cmd_install)

    info = commands.add_parser("info", help="show an NCA's header and contents, and verify its hashes")
    info.add_argument("nca")
    info.set_defaults(handler=cmd_info)

    args = parser.parse_args()
    args.handler(args)


if __name__ == "__main__":
    main()
