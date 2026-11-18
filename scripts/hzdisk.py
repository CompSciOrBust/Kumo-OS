#!/usr/bin/env python3
"""Create and edit Kumo system disk images.

Kumo's FS system module looks for GPT partitions named after the Switch's eMMC
partitions (SYSTEM, USER, SAFE, PRODINFO, PRODINFOF, ...) plus SDCARD, which
stands in for the SD card slot. These names and standard GPT/FAT filesystems do
not make the resulting image a general Horizon disk image: its boot layout and
content are for Kumo and may not boot on other Horizon-compatible systems.

    hzdisk.py create disk.img --size 1G
    hzdisk.py create disk.img --size 256M --layout "PRODINFO:4M:raw,SYSTEM:64M:fat,SDCARD:rest:blank"
    hzdisk.py create disk.img --size 1G --sector-size 4096
    hzdisk.py create disk.img --size 1G --layout "ESP:64M:esp,SYSTEM:256M:fat,SDCARD:rest:fat"
    hzdisk.py list   disk.img
    hzdisk.py format disk.img SDCARD
    hzdisk.py put    disk.img SYSTEM ./sysroot /        # copy a file or a directory tree in
    hzdisk.py get    disk.img SDCARD /log.txt ./log.txt # copy one file out
    hzdisk.py ls     disk.img SDCARD [/dir]

FAT work is done by tools/fatimg, which links the same vendored FatFs as the
FS system module. It is compiled on first use (needs a C compiler).
"""

import argparse
import os
import struct
import subprocess
import sys
import uuid
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FATIMG = os.path.join(ROOT, "build", "tools", "fatimg")
FATIMG_SOURCES = [
    os.path.join(ROOT, "tools", "fatimg", "fatimg.c"),
    os.path.join(ROOT, "tools", "fatimg", "hostdir.c"),
    os.path.join(ROOT, "vendor", "fatfs", "source", "ff.c"),
    os.path.join(ROOT, "vendor", "fatfs", "source", "ffunicode.c"),
]
FATIMG_INCLUDES = [
    os.path.join(ROOT, "tools", "fatimg"),
    os.path.join(ROOT, "vendor", "fatfs", "source"),
]
FATIMG_HEADERS = [
    os.path.join(ROOT, "tools", "fatimg", "ffconf.h"),
    os.path.join(ROOT, "tools", "fatimg", "hostdir.h"),
    os.path.join(ROOT, "sysmodules", "fs", "source", "fat", "ffconf.h"),
]

SECTOR_SIZES = (512, 4096)
SECTOR_SIZE = 512                # set per image: by --sector-size on create, detected otherwise
ALIGNMENT = 1024 * 1024          # partitions start on 1 MiB boundaries
GPT_ENTRY_COUNT = 128
GPT_ENTRY_SIZE = 128
GPT_HEADER_SIZE = 92
GPT_SIGNATURE = b"EFI PART"

# "Microsoft basic data": other operating systems will offer to mount these.
BASIC_DATA_TYPE = uuid.UUID("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7")
# EFI system partition: where UEFI firmware looks for the bootloader.
EFI_SYSTEM_TYPE = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")

# kind is fat (formatted), blank (left empty for the OS to format), raw (not a filesystem)
# or esp (an EFI system partition, formatted FAT32).
KINDS = ("fat", "blank", "raw", "esp")
DEFAULT_LAYOUT = "PRODINFO:4M:raw,PRODINFOF:4M:fat,SAFE:64M:fat,SYSTEM:256M:fat,USER:256M:fat,SDCARD:rest:fat"


def fail(message):
    sys.exit(f"hzdisk: {message}")


def parse_size(text):
    units = {"K": 1024, "M": 1024 ** 2, "G": 1024 ** 3, "T": 1024 ** 4}
    text = text.strip().upper().removesuffix("IB").removesuffix("B")
    if text and text[-1] in units:
        return int(float(text[:-1]) * units[text[-1]])
    return int(text)


def parse_layout(text):
    layout = []
    for item in text.split(","):
        fields = item.strip().split(":")
        if len(fields) not in (2, 3):
            fail(f"bad layout entry '{item}' (want NAME:SIZE[:{'|'.join(KINDS)}])")
        name, size = fields[0], fields[1]
        kind = fields[2] if len(fields) == 3 else "fat"
        if kind not in KINDS:
            fail(f"bad partition kind '{kind}' for {name}")
        if not name or len(name) > 36:
            fail(f"partition name '{name}' must be 1-36 characters")
        layout.append((name, None if size == "rest" else parse_size(size), kind))
    if [size for _, size, _ in layout].count(None) > 1:
        fail("only one partition can use 'rest'")
    if len({name for name, _, _ in layout}) != len(layout):
        fail("partition names must be unique")
    return layout


# ----- GPT -----------------------------------------------------------------

def entry_sectors():
    return GPT_ENTRY_COUNT * GPT_ENTRY_SIZE // SECTOR_SIZE


def detect_sector_size(image):
    """The GPT header sits in the second sector, so its position gives the sector size."""
    global SECTOR_SIZE
    with open(image, "rb") as f:
        for size in SECTOR_SIZES:
            f.seek(size)
            if f.read(8) == GPT_SIGNATURE:
                SECTOR_SIZE = size
                return
    fail(f"{image} has no GPT")


def build_header(my_lba, alternate_lba, first_usable, last_usable, disk_guid, entries_lba, entries_crc):
    def pack(header_crc):
        return struct.pack("<8sIIIIQQQQ16sQIII", GPT_SIGNATURE, 0x00010000, GPT_HEADER_SIZE, header_crc, 0,
                           my_lba, alternate_lba, first_usable, last_usable, disk_guid.bytes_le,
                           entries_lba, GPT_ENTRY_COUNT, GPT_ENTRY_SIZE, entries_crc)
    header = pack(zlib.crc32(pack(0)))
    return header + bytes(SECTOR_SIZE - len(header))


def write_gpt(image, partitions):
    """partitions: list of (name, first_lba, last_lba, type_guid)."""
    total_sectors = os.path.getsize(image) // SECTOR_SIZE
    last_lba = total_sectors - 1
    first_usable = 2 + entry_sectors()
    last_usable = last_lba - entry_sectors() - 1

    entries = bytearray(GPT_ENTRY_COUNT * GPT_ENTRY_SIZE)
    for index, (name, first, last, type_guid) in enumerate(partitions):
        entry = struct.pack("<16s16sQQQ72s", type_guid.bytes_le, uuid.uuid4().bytes_le, first, last, 0,
                            name.encode("utf-16-le"))
        entries[index * GPT_ENTRY_SIZE:(index + 1) * GPT_ENTRY_SIZE] = entry
    entries_crc = zlib.crc32(entries)
    disk_guid = uuid.uuid4()

    # Protective MBR: one partition of type 0xEE covering the disk.
    mbr = bytearray(SECTOR_SIZE)
    mbr[446:462] = struct.pack("<B3sB3sII", 0, b"\x00\x02\x00", 0xEE, b"\xff\xff\xff", 1, min(last_lba, 0xFFFFFFFF))
    mbr[510:512] = b"\x55\xaa"

    with open(image, "r+b") as f:
        f.write(mbr)
        f.write(build_header(1, last_lba, first_usable, last_usable, disk_guid, 2, entries_crc))
        f.write(entries)
        f.seek((last_usable + 1) * SECTOR_SIZE)
        f.write(entries)
        f.write(build_header(last_lba, 1, first_usable, last_usable, disk_guid, last_usable + 1, entries_crc))


def read_gpt(image):
    """Returns a list of (name, offset_bytes, size_bytes)."""
    detect_sector_size(image)
    with open(image, "rb") as f:
        f.seek(SECTOR_SIZE)
        header = f.read(SECTOR_SIZE)
        entries_lba, count, size = struct.unpack_from("<QII", header, 72)
        f.seek(entries_lba * SECTOR_SIZE)
        table = f.read(count * size)

    partitions = []
    for index in range(count):
        entry = table[index * size:(index + 1) * size]
        if entry[:16] == bytes(16):
            continue
        first, last = struct.unpack_from("<QQ", entry, 32)
        name = entry[56:128].decode("utf-16-le").split("\x00")[0]
        partitions.append((name, first * SECTOR_SIZE, (last - first + 1) * SECTOR_SIZE))
    return partitions


def find_partition(image, name):
    for part_name, offset, size in read_gpt(image):
        if part_name == name:
            return offset, size
    fail(f"{image} has no partition named '{name}'")


# ----- FAT, through tools/fatimg ---------------------------------------------

def build_fatimg():
    inputs = FATIMG_SOURCES + FATIMG_HEADERS
    if os.path.exists(FATIMG) and all(os.path.getmtime(FATIMG) >= os.path.getmtime(p) for p in inputs):
        return
    os.makedirs(os.path.dirname(FATIMG), exist_ok=True)
    command = [os.environ.get("CC", "cc"), "-O2", "-w"]
    command += [f"-I{path}" for path in FATIMG_INCLUDES]
    command += ["-o", FATIMG] + FATIMG_SOURCES
    if subprocess.run(command).returncode != 0:
        fail("could not compile tools/fatimg")


def fatimg(image, partition, *arguments):
    build_fatimg()
    offset, size = find_partition(image, partition)
    command = [FATIMG, image, str(offset), str(size), str(SECTOR_SIZE)] + list(arguments)
    if subprocess.run(command).returncode != 0:
        sys.exit(1)


# ----- Commands --------------------------------------------------------------

def cmd_create(args):
    global SECTOR_SIZE
    SECTOR_SIZE = args.sector_size
    layout = parse_layout(args.layout)
    size = parse_size(args.size)
    if size % SECTOR_SIZE != 0:
        fail(f"image size must be a multiple of {SECTOR_SIZE} bytes")

    # Usable space runs from the first 1 MiB boundary to the last one before the backup GPT.
    start = ALIGNMENT
    end = (size - (entry_sectors() + 1) * SECTOR_SIZE) // ALIGNMENT * ALIGNMENT
    fixed = sum(-(-part_size // ALIGNMENT) * ALIGNMENT for _, part_size, _ in layout if part_size is not None)
    rest = end - start - fixed
    if rest < 0 or (rest == 0 and any(part_size is None for _, part_size, _ in layout)):
        fail(f"layout needs more than the {args.size} image provides")

    partitions = []
    position = start
    for name, part_size, kind in layout:
        part_size = rest if part_size is None else -(-part_size // ALIGNMENT) * ALIGNMENT
        type_guid = EFI_SYSTEM_TYPE if kind == "esp" else BASIC_DATA_TYPE
        partitions.append((name, position // SECTOR_SIZE, (position + part_size) // SECTOR_SIZE - 1, type_guid))
        position += part_size

    with open(args.image, "wb") as f:
        f.truncate(size)
    write_gpt(args.image, partitions)

    for name, _, kind in layout:
        if kind == "fat":
            fatimg(args.image, name, "format")
        elif kind == "esp":
            fatimg(args.image, name, "format", "fat32")

    cmd_list(args)


def cmd_list(args):
    for name, offset, size in read_gpt(args.image):
        print(f"{name:<24} offset {offset:>12}  size {size:>12}  ({size / 1024 / 1024:.0f} MiB)")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    create = commands.add_parser("create", help="create a new image with a GPT")
    create.add_argument("image")
    create.add_argument("--size", required=True, help="image size, e.g. 512M or 2G")
    create.add_argument("--sector-size", type=int, choices=SECTOR_SIZES, default=512,
                        help="logical sector size of the disk the image is for (default: 512)")
    create.add_argument("--layout", default=DEFAULT_LAYOUT,
                        help=f"comma-separated NAME:SIZE[:{'|'.join(KINDS)}]; SIZE may be 'rest' once (default: {DEFAULT_LAYOUT})")
    create.set_defaults(run=cmd_create)

    listing = commands.add_parser("list", help="list the image's partitions")
    listing.add_argument("image")
    listing.set_defaults(run=cmd_list)

    fmt = commands.add_parser("format", help="create an empty FAT/exFAT filesystem on a partition")
    fmt.add_argument("image")
    fmt.add_argument("partition")
    fmt.set_defaults(run=lambda a: fatimg(a.image, a.partition, "format"))

    put = commands.add_parser("put", help="copy a host file or directory tree into a partition")
    put.add_argument("image")
    put.add_argument("partition")
    put.add_argument("source")
    put.add_argument("destination", help="path inside the partition, starting with /")
    put.set_defaults(run=lambda a: fatimg(a.image, a.partition, "put", a.source, a.destination))

    get = commands.add_parser("get", help="copy one file out of a partition")
    get.add_argument("image")
    get.add_argument("partition")
    get.add_argument("source", help="path inside the partition, starting with /")
    get.add_argument("destination")
    get.set_defaults(run=lambda a: fatimg(a.image, a.partition, "get", a.source, a.destination))

    ls = commands.add_parser("ls", help="list a partition's files, recursively")
    ls.add_argument("image")
    ls.add_argument("partition")
    ls.add_argument("path", nargs="?", default="/")
    ls.set_defaults(run=lambda a: fatimg(a.image, a.partition, "ls", a.path))

    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
