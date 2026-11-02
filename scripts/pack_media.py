#!/usr/bin/env python3
"""Build a PPAK media pack (and/or a UF2 image) from a directory of audio files.

Format (little-endian, see docs/media_design.md):

    header (32 bytes): "PPAK", u16 version=1, u16 file_count, u32 total_size,
                       u32 toc_offset, u32 toc_crc32, u8 reserved[12]
    toc entry (80 bytes): char name[64], u32 offset, u32 size, u32 crc32,
                          u32 flags

File offsets are relative to the start of the pack and 4096-aligned. Files are
packed in sorted relative-POSIX-path order, so the output is deterministic.

Examples:

    python scripts/pack_media.py --in test_media --out pack.bin --uf2 pack.uf2
    python scripts/pack_media.py --list pack.bin
"""

import argparse
import os
import struct
import sys
import zlib

HEADER_SIZE = 32
TOC_ENTRY_SIZE = 80
NAME_SIZE = 64
ALIGN = 4096
VERSION = 1
FLASH_SIZE = 16 * 1024 * 1024
XIP_BASE = 0x10000000
PACK_OFFSET_DEFAULT = 0x00100000
PACK_MAX_DEFAULT = 0x00E00000

# RP2350 ARM secure boot family ID, used by the generated UF2's family flag.
RP2350_ARM_SECURE_FAMILY = 0xE48BFF59

UF2_MAGIC0 = 0x0A324655
UF2_MAGIC1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_ID = 0x00002000
UF2_PAYLOAD = 256
UF2_BLOCK_SIZE = 512

AUDIO_EXTENSIONS = {".mp3", ".flac", ".wav", ".ogg", ".opus", ".m4a", ".aac"}


def align_up(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def human(n):
    if n >= 1024 * 1024:
        return f"{n / (1024 * 1024):.2f} MiB"
    if n >= 1024:
        return f"{n / 1024:.1f} KiB"
    return f"{n} B"


def parse_int(text):
    try:
        return int(text, 0)
    except ValueError:
        raise argparse.ArgumentTypeError(f"not an integer: {text!r}")


def collect_files(root):
    """Return [(relative_posix_path, absolute_path)] sorted deterministically."""
    found = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for filename in sorted(filenames):
            ext = os.path.splitext(filename)[1].lower()
            if ext not in AUDIO_EXTENSIONS:
                continue
            full = os.path.join(dirpath, filename)
            if not os.path.isfile(full):
                continue
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            found.append((rel, full))
    found.sort(key=lambda item: item[0].encode("utf-8"))
    return found


def build_pack(root, max_size, flash_offset):
    files = collect_files(root)
    if len(files) > 0xFFFF:
        raise SystemExit(f"error: too many files ({len(files)}); the TOC holds at most 65535")

    toc_offset = HEADER_SIZE
    data_start = align_up(toc_offset + len(files) * TOC_ENTRY_SIZE, ALIGN)

    toc = bytearray()
    blobs = []
    offset = data_start
    for name, path in files:
        name_bytes = name.encode("utf-8")
        if not name_bytes:
            raise SystemExit(f"error: empty file name for {path}")
        if len(name_bytes) >= NAME_SIZE:
            raise SystemExit(
                f"error: name too long for the pack ({len(name_bytes)} bytes, max "
                f"{NAME_SIZE - 1}): {name}"
            )
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError as exc:
            raise SystemExit(f"error: cannot read {path}: {exc}")

        crc = zlib.crc32(data) & 0xFFFFFFFF
        toc += name_bytes + b"\0" * (NAME_SIZE - len(name_bytes))
        toc += struct.pack("<IIII", offset, len(data), crc, 0)
        blobs.append((offset, data))
        offset = align_up(offset + len(data), ALIGN)

    total_size = offset
    toc_crc = zlib.crc32(toc) & 0xFFFFFFFF
    header = (
        b"PPAK"
        + struct.pack("<HHIII", VERSION, len(files), total_size, toc_offset, toc_crc)
        + b"\0" * 12
    )
    assert len(header) == HEADER_SIZE

    if total_size > max_size:
        raise SystemExit(
            f"error: media pack does not fit: need {total_size} bytes "
            f"({human(total_size)}), limit {max_size} bytes ({human(max_size)}), "
            f"over by {total_size - max_size} bytes ({human(total_size - max_size)})"
        )
    if flash_offset + total_size > FLASH_SIZE:
        raise SystemExit(
            f"error: media pack at 0x{flash_offset:08X} would end at "
            f"0x{flash_offset + total_size:08X}, past the {human(FLASH_SIZE)} flash"
        )

    image = bytearray(total_size)
    image[0:HEADER_SIZE] = header
    image[toc_offset:toc_offset + len(toc)] = toc
    for file_offset, data in blobs:
        image[file_offset:file_offset + len(data)] = data

    return bytes(image), files, toc_crc


def write_uf2(path, image, flash_offset, family_id):
    target_base = XIP_BASE + flash_offset
    payload = image + b"\0" * (align_up(len(image), UF2_PAYLOAD) - len(image))
    blocks = len(payload) // UF2_PAYLOAD
    if blocks > 0xFFFFFFFF:
        raise SystemExit("error: media pack too large for UF2")
    with open(path, "wb") as f:
        for index in range(blocks):
            header = struct.pack(
                "<IIIIIIII",
                UF2_MAGIC0,
                UF2_MAGIC1,
                UF2_FLAG_FAMILY_ID,
                target_base + index * UF2_PAYLOAD,
                UF2_PAYLOAD,
                index,
                blocks,
                family_id,
            )
            chunk = payload[index * UF2_PAYLOAD:(index + 1) * UF2_PAYLOAD]
            # 32-byte header + 256-byte payload + padding + 4-byte end magic
            # = one 512-byte UF2 block (data field is 476 bytes at offset 32).
            padding = b"\0" * (UF2_BLOCK_SIZE - 32 - UF2_PAYLOAD - 4)
            f.write(header + chunk + padding + struct.pack("<I", UF2_MAGIC_END))
    return blocks, payload


def dump_pack(path):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as exc:
        raise SystemExit(f"error: cannot read {path}: {exc}")
    if len(data) < HEADER_SIZE:
        raise SystemExit(f"error: {path} is too short to be a PPAK image")
    magic, version, count, total_size, toc_offset, toc_crc = struct.unpack_from(
        "<4sHHIII", data, 0
    )
    if magic != b"PPAK":
        raise SystemExit(f"error: {path}: bad magic {magic!r} (expected b'PPAK')")
    if version != VERSION:
        raise SystemExit(f"error: {path}: unsupported PPAK version {version}")
    toc_bytes = count * TOC_ENTRY_SIZE
    if toc_offset + toc_bytes > len(data):
        raise SystemExit(f"error: {path}: TOC runs past the end of the file")
    toc = data[toc_offset:toc_offset + toc_bytes]
    actual_toc_crc = zlib.crc32(toc) & 0xFFFFFFFF
    print(f"{path}: {len(data)} bytes ({human(len(data))}), {count} files")
    print(
        f"  version {version}, total_size {total_size}, TOC @ 0x{toc_offset:X} "
        f"({toc_bytes} bytes), toc_crc 0x{toc_crc:08X}"
    )
    if actual_toc_crc != toc_crc:
        print(f"  ERROR: TOC CRC mismatch (stored 0x{toc_crc:08X}, computed 0x{actual_toc_crc:08X})")
        return 1
    if total_size > len(data):
        print(f"  WARNING: total_size {total_size} exceeds file size {len(data)}")
    if total_size != len(data):
        print(f"  note: file size differs from total_size ({len(data)} vs {total_size})")

    bad = 0
    print(f"  {'#':>4}  {'offset':>9}  {'size':>9}  {'crc32':>10}  name")
    for index in range(count):
        entry = toc[index * TOC_ENTRY_SIZE:(index + 1) * TOC_ENTRY_SIZE]
        name = entry[:NAME_SIZE].split(b"\0", 1)[0].decode("utf-8", "replace")
        offset, size, crc, flags = struct.unpack_from("<IIII", entry, NAME_SIZE)
        status = ""
        if offset + size > len(data):
            status = "  OUT-OF-BOUNDS"
            bad += 1
        elif zlib.crc32(data[offset:offset + size]) & 0xFFFFFFFF != crc:
            status = "  CRC MISMATCH"
            bad += 1
        print(f"  {index:>4}  0x{offset:07X}  {size:>9}  0x{crc:08X}  {name}{status}")
        if flags != 0:
            print(f"        (flags 0x{flags:08X})")
    if bad:
        print(f"  ERROR: {bad} file(s) failed verification")
        return 1
    print("  all file CRCs valid")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Pack audio files into a PPAK media pack (and/or UF2).",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--in", dest="indir", metavar="DIR", help="input directory (recursive)")
    parser.add_argument("--out", metavar="FILE", help="write the PPAK image here")
    parser.add_argument("--uf2", metavar="FILE", help="also write a UF2 image here")
    parser.add_argument(
        "--offset",
        type=parse_int,
        default=PACK_OFFSET_DEFAULT,
        help="flash offset the pack is written to",
    )
    parser.add_argument(
        "--max",
        type=parse_int,
        default=PACK_MAX_DEFAULT,
        dest="max_size",
        help="maximum pack size in bytes",
    )
    parser.add_argument(
        "--family",
        type=parse_int,
        default=RP2350_ARM_SECURE_FAMILY,
        help="UF2 family ID",
    )
    parser.add_argument("--list", metavar="FILE", help="dump and verify a pack instead")
    args = parser.parse_args(argv)

    if args.list:
        if args.indir or args.out or args.uf2:
            parser.error("--list cannot be combined with --in/--out/--uf2")
        return dump_pack(args.list)

    if not args.indir:
        parser.error("--in DIR is required (or use --list FILE)")
    if not os.path.isdir(args.indir):
        raise SystemExit(f"error: --in {args.indir!r} is not a directory")
    if not args.out and not args.uf2:
        parser.error("at least one of --out or --uf2 is required")

    image, files, toc_crc = build_pack(args.indir, args.max_size, args.offset)

    if args.out:
        try:
            with open(args.out, "wb") as f:
                f.write(image)
        except OSError as exc:
            raise SystemExit(f"error: cannot write {args.out}: {exc}")

    blocks = 0
    if args.uf2:
        try:
            blocks, payload = write_uf2(args.uf2, image, args.offset, args.family)
        except OSError as exc:
            raise SystemExit(f"error: cannot write {args.uf2}: {exc}")
        assert len(payload) == blocks * UF2_PAYLOAD

    used = len(image)
    limit = args.max_size
    print(f"packed {len(files)} file(s)")
    print(f"  pack size    {used} bytes ({human(used)})")
    print(
        f"  flash range  0x{args.offset:08X}..0x{args.offset + used:08X} "
        f"(limit 0x{limit:X} = {human(limit)})"
    )
    if limit:
        print(
            f"  capacity     {100.0 * used / limit:.1f}% used, "
            f"{human(limit - used)} free"
        )
    print(f"  TOC crc      0x{toc_crc:08X} (offset 0x{HEADER_SIZE:X})")
    if args.out:
        print(f"  wrote {args.out}")
    if args.uf2:
        print(f"  wrote {args.uf2} ({blocks} UF2 blocks, family 0x{args.family:08X})")
    print()
    print("To flash with picotool (verified against tools/picotool 'help load'):")
    if args.out:
        print(f"  picotool load -v -x \"{args.out}\" -o 0x{XIP_BASE + args.offset:08X}")
    if args.uf2:
        print(f"  picotool load -v -x \"{args.uf2}\"")
    return 0


if __name__ == "__main__":
    sys.exit(main())
