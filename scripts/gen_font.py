#!/usr/bin/env python3
"""Generate a 4bpp anti-aliased C font from a TrueType/OpenType font.

The emitted table matches ``gfx_font_t`` / ``gfx_glyph_t`` from
``lib/gfx/gfx_font.h`` (see docs/design.md, section "Font format"):

    bitmap        4bpp alpha, 2 px per byte, HIGH nibble = left pixel,
                  every row padded to a whole byte
    glyphs        glyphs[codepoint - first], one entry per codepoint
    first/last    inclusive codepoint range (default 0x20..0x7E)
    line_height   baseline-to-baseline distance (ascent + descent + leading)
    ascent        font ascender above the baseline

The same rasteriser is importable (``build_font`` / ``render_text_mask``) so
scripts/gen_font_preview.py and scripts/gen_demo_art.py reuse it instead of
parsing the generated C.

Usage:
    python -I scripts/gen_font.py --ttf assets/fonts/Inter-Variable.ttf \\
        --size 16 --weight 600 --name font_inter_16sb --out-dir lib/gfx/fonts \\
        --verify

Requires Pillow (FreeType). No numpy.
"""

from __future__ import annotations

import argparse
import math
import os
import sys
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

from PIL import Image, ImageDraw, ImageFont

# --------------------------------------------------------------------------- #
# Data model
# --------------------------------------------------------------------------- #


@dataclass
class Glyph:
    codepoint: int
    width: int
    height: int
    x_advance: int
    x_offset: int
    y_offset: int
    #: packed 4bpp rows (width*height nibbles, row padded to a whole byte)
    bitmap: bytes = b""
    #: unpacked 4bpp values, one byte per pixel in 0..15 (kept for --verify)
    alphas: bytes = field(default=b"", repr=False)

    @property
    def stride(self) -> int:
        """Packed bytes per bitmap row (rows are padded to a whole byte)."""
        return (self.width + 1) // 2

    def unpack(self) -> bytearray:
        """Decode ``bitmap`` back to one 0..15 value per pixel."""
        out = bytearray(self.width * self.height)
        stride = self.stride
        for y in range(self.height):
            row = self.bitmap[y * stride : (y + 1) * stride]
            base = y * self.width
            for x in range(self.width):
                byte = row[x >> 1]
                out[base + x] = (byte >> 4) if (x & 1) == 0 else (byte & 0x0F)
        return out


@dataclass
class FontData:
    name: str
    source: str
    size: int
    weight: int
    opsz: float
    first: int
    last: int
    line_height: int
    ascent: int
    glyphs: List[Glyph]

    @property
    def bitmap(self) -> bytes:
        return b"".join(g.bitmap for g in self.glyphs)

    @property
    def bitmap_bytes(self) -> int:
        return len(self.bitmap)

    @property
    def glyph_bytes(self) -> int:
        # sizeof(gfx_glyph_t): uint32_t (4) + 5 x uint8_t/int8_t (5) = 9,
        # rounded up to the 4-byte alignment of the leading uint32_t -> 12.
        return len(self.glyphs) * 12


# --------------------------------------------------------------------------- #
# Rasteriser
# --------------------------------------------------------------------------- #

QUANT_LEVELS = 15  # 4bpp


def _round_half_up(value: float) -> int:
    """Round to nearest, ties away from zero (C-like, not banker's rounding)."""
    return int(math.floor(value + 0.5)) if value >= 0 else -int(math.floor(-value + 0.5))


def _open_font(ttf: str, size: int, weight: int, opsz: float) -> ImageFont.FreeTypeFont:
    font = ImageFont.truetype(ttf, size)
    axes = font.get_variation_axes()
    if not axes:
        raise SystemExit(f"{ttf}: not a variable font, cannot select weight {weight}")
    names = [a["name"].decode("ascii", "replace").lower() for a in axes]
    # Inter exposes [Optical size, Weight]; honour the declared order.
    if names == ["optical size", "weight"]:
        font.set_variation_by_axes([float(opsz), int(weight)])
    elif names == ["weight", "optical size"]:
        font.set_variation_by_axes([int(weight), float(opsz)])
    elif len(axes) == 1 and names[0] == "weight":
        font.set_variation_by_axes([int(weight)])
    else:
        raise SystemExit(f"{ttf}: unsupported variation axes {names}")
    return font


def _quantise(values: bytes) -> List[int]:
    """8-bit alpha -> 4-bit level with round-to-nearest (17*k)."""
    return [(v * QUANT_LEVELS + 127) // 255 for v in values]


def _pack(row_vals: List[int], width: int) -> bytes:
    """Pack one row of 0..15 nibbles, HIGH nibble first, pad to whole byte."""
    out = bytearray((width + 1) // 2)
    for x in range(0, width, 2):
        hi = row_vals[x] & 0x0F
        lo = (row_vals[x + 1] & 0x0F) if (x + 1) < width else 0
        out[x >> 1] = (hi << 4) | lo
    return bytes(out)


def rasterise_glyph(font: ImageFont.FreeTypeFont, ch: str, codepoint: int) -> Glyph:
    """Render one glyph, crop to its tight ink box and pack the 4bpp alpha."""
    ascent, descent = font.getmetrics()
    advance = _round_half_up(font.getlength(ch))

    # Generous canvas: FreeType may bleed a pixel or two outside the metrics box.
    pad = 4
    pad_x = max(pad, int(font.size) // 4)
    pad_y = max(pad, int(font.size) // 4)
    width = max(1, int(math.ceil(font.getlength(ch))) + 2 * pad_x)
    height = max(1, ascent + descent + 2 * pad_y)

    canvas = Image.new("L", (width, height), 0)
    draw = ImageDraw.Draw(canvas)
    # anchor "la" = left / ascender: the pen sits at (pad_x, pad_y + ascent).
    draw.text((pad_x, pad_y), ch, font=font, fill=255, anchor="la")

    ink = canvas.getbbox()
    if ink is None:  # whitespace: no ink, keep the advance only
        return Glyph(codepoint, 0, 0, advance, 0, 0, b"", b"")

    crop = canvas.crop(ink)
    cw, ch_px = crop.size
    levels = _quantise(crop.tobytes())

    # Quantisation may zero the outer AA pixels; trim so the glyph is tight.
    def row_nonzero(y: int) -> bool:
        return any(levels[y * cw : (y + 1) * cw])

    def col_nonzero(x: int) -> bool:
        return any(levels[y * cw + x] for y in range(ch_px))

    top = 0
    while top < ch_px and not row_nonzero(top):
        top += 1
    if top == ch_px:  # everything quantised away (extremely faint glyph)
        return Glyph(codepoint, 0, 0, advance, 0, 0, b"", b"")
    bottom = ch_px - 1
    while not row_nonzero(bottom):
        bottom -= 1
    left = 0
    while left < cw and not col_nonzero(left):
        left += 1
    right = cw - 1
    while not col_nonzero(right):
        right -= 1

    w = right - left + 1
    h = bottom - top + 1
    rows: List[int] = []
    for y in range(top, bottom + 1):
        rows.extend(levels[y * cw + left : y * cw + right + 1])
    bitmap = b"".join(_pack(rows[y * w : (y + 1) * w], w) for y in range(h))

    x_offset = (ink[0] + left) - pad_x
    y_offset = (ink[1] + top) - pad_y - ascent
    return Glyph(codepoint, w, h, advance, x_offset, y_offset, bitmap, bytes(rows))


def build_font(
    ttf: str,
    size: int,
    weight: int,
    opsz: Optional[float] = None,
    first: int = 0x20,
    last: int = 0x7E,
    name: str = "font_generated",
) -> FontData:
    """Rasterise every codepoint in ``first..last`` and return the atlas."""
    if opsz is None:
        opsz = float(min(32, max(14, size)))
    font = _open_font(ttf, size, weight, opsz)
    ascent, descent = font.getmetrics()
    leading = max(1, _round_half_up(size * 0.08))
    line_height = ascent + descent + leading
    if line_height > 255:
        raise SystemExit(f"line_height {line_height} does not fit in uint8_t")
    glyphs = [rasterise_glyph(font, chr(cp), cp) for cp in range(first, last + 1)]
    return FontData(
        name=name,
        source=ttf,
        size=size,
        weight=weight,
        opsz=float(opsz),
        first=first,
        last=last,
        line_height=line_height,
        ascent=ascent,
        glyphs=glyphs,
    )


# --------------------------------------------------------------------------- #
# Verification
# --------------------------------------------------------------------------- #


def verify_font(data: FontData) -> None:
    """Decode every packed bitmap and assert it matches the quantised raster."""
    expected = data.last - data.first + 1
    assert len(data.glyphs) == expected, (
        f"{data.name}: {len(data.glyphs)} glyphs, expected {expected}"
    )
    for idx, g in enumerate(data.glyphs):
        assert g.codepoint == data.first + idx, f"{data.name}: glyph order broken at {idx}"
        assert 0 <= g.x_advance <= 255, f"{data.name}: U+{g.codepoint:04X} bad advance"
        assert -128 <= g.x_offset <= 127, f"{data.name}: U+{g.codepoint:04X} bad x_offset"
        assert -128 <= g.y_offset <= 127, f"{data.name}: U+{g.codepoint:04X} bad y_offset"
        assert 0 <= g.width <= 255 and 0 <= g.height <= 255
        if g.width == 0 or g.height == 0:
            assert g.bitmap == b"", f"{data.name}: U+{g.codepoint:04X} empty but packed"
            continue
        assert len(g.bitmap) == g.stride * g.height, (
            f"{data.name}: U+{g.codepoint:04X} bitmap is {len(g.bitmap)} bytes, "
            f"expected {g.stride * g.height}"
        )
        decoded = g.unpack()
        assert len(g.alphas) == g.width * g.height
        for i, (a, b) in enumerate(zip(decoded, g.alphas)):
            assert a == b, (
                f"{data.name}: U+{g.codepoint:04X} decode mismatch at pixel {i}: "
                f"{a} != {b}"
            )
        # Re-packing must reproduce the stored bytes bit for bit.
        repacked = b"".join(
            _pack(list(decoded[y * g.width : (y + 1) * g.width]), g.width)
            for y in range(g.height)
        )
        assert repacked == g.bitmap, f"{data.name}: U+{g.codepoint:04X} repack mismatch"
    # Offsets must keep ink inside the nominal line box (sane, not clippable).
    for g in data.glyphs:
        if g.height == 0:
            continue
        top = g.y_offset + data.ascent  # distance from line top
        bottom = top + g.height
        assert top >= -8, f"{data.name}: U+{g.codepoint:04X} ink above line box ({top})"
        assert bottom <= data.line_height + 8, (
            f"{data.name}: U+{g.codepoint:04X} ink below line box ({bottom})"
        )


# --------------------------------------------------------------------------- #
# C emitter
# --------------------------------------------------------------------------- #


def _cpp_char(cp: int) -> str:
    return chr(cp) if 0x20 <= cp < 0x7F else "?"


def _glyph_comment(cp: int) -> str:
    if 0x20 <= cp < 0x7F:
        return f"/* U+{cp:04X} '{_cpp_char(cp)}' */"
    return f"/* U+{cp:04X} */"


def emit_c(data: FontData, out_path: str, source_label: str, font_path: str) -> None:
    name = data.name
    lines: List[str] = []
    lines.append("/*")
    lines.append(f" * {name}: {source_label}, {data.size} px, weight {data.weight},")
    lines.append(f" * optical size {data.opsz:g}. Codepoints U+{data.first:04X}..U+{data.last:04X}.")
    lines.append(" *")
    lines.append(f" * Source font: {font_path}")
    lines.append(" * License: SIL Open Font License 1.1 (see assets/fonts/OFL.txt)")
    lines.append(" * Generated by scripts/gen_font.py -- do not edit.")
    lines.append(" */")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append('#include "gfx_font.h"')
    lines.append("")
    lines.append("/* 4bpp alpha: 2 px per byte, high nibble first, rows padded to a byte. */")
    lines.append(f"static const uint8_t {name}_bitmap[] = {{")
    bmp = data.bitmap
    for i in range(0, len(bmp), 16):
        chunk = bmp[i : i + 16]
        lines.append("    " + " ".join(f"0x{b:02X}," for b in chunk))
    lines.append("};")
    lines.append("")
    lines.append("/* bitmap_offset, width, height, x_advance, x_offset, y_offset */")
    lines.append(f"static const gfx_glyph_t {name}_glyphs[] = {{")
    offset = 0
    for g in data.glyphs:
        lines.append(
            f"    {{ {offset:5d}, {g.width:3d}, {g.height:3d}, "
            f"{g.x_advance:3d}, {g.x_offset:4d}, {g.y_offset:4d} }}, "
            f"{_glyph_comment(g.codepoint)}"
        )
        offset += len(g.bitmap)
    lines.append("};")
    lines.append("")
    lines.append(
        f"_Static_assert(sizeof({name}_glyphs) / sizeof({name}_glyphs[0]) == "
        f"{len(data.glyphs)}u, \"glyph count must match first..last\");"
    )
    lines.append("")
    lines.append(f"const gfx_font_t {name} = {{")
    lines.append(f"    {name}_bitmap,")
    lines.append(f"    {name}_glyphs,")
    lines.append(f"    0x{data.first:04X},")
    lines.append(f"    0x{data.last:04X},")
    lines.append(f"    {data.line_height},")
    lines.append(f"    {data.ascent},")
    lines.append("};")
    lines.append("")

    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))


# --------------------------------------------------------------------------- #
# Tiny text compositor (used by the preview script)
# --------------------------------------------------------------------------- #


def render_text_mask(data: FontData, text: str) -> Tuple[Image.Image, int]:
    """Compose a single line of text into an 8-bit coverage mask.

    Uses exactly the stored metrics (x_advance / x_offset / y_offset), so a
    preview proves the generated tables are self-consistent.
    """
    pen = 0
    for ch in text:
        cp = ord(ch)
        if cp < data.first or cp > data.last:
            raise ValueError(f"U+{cp:04X} outside {data.first:04X}..{data.last:04X}")
        pen += data.glyphs[cp - data.first].x_advance
    width = max(1, pen + 2)
    height = max(1, data.line_height)
    mask = Image.new("L", (width, height), 0)
    px = mask.load()
    pen = 0
    for ch in text:
        g = data.glyphs[ord(ch) - data.first]
        if g.width and g.height:
            alpha = g.unpack()
            top = data.ascent + g.y_offset
            left = pen + g.x_offset
            for y in range(g.height):
                yy = top + y
                if yy < 0 or yy >= height:
                    continue
                row = y * g.width
                for x in range(g.width):
                    xx = left + x
                    if xx < 0 or xx >= width:
                        continue
                    level = alpha[row + x]
                    if level:
                        px[xx, yy] = max(px[xx, yy], level * 17)
        pen += g.x_advance
    return mask, pen


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #


def _clamp_opsz(size: int) -> float:
    return float(min(32, max(14, size)))


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ttf", required=True, help="source variable TTF/OTF")
    ap.add_argument("--size", type=int, required=True, help="pixel size (EM size)")
    ap.add_argument(
        "--weight",
        type=int,
        default=400,
        help="variation weight axis value (100..900, default 400)",
    )
    ap.add_argument(
        "--opsz",
        type=float,
        default=None,
        help="optical size axis value (default: clamp(size, 14..32))",
    )
    ap.add_argument("--name", required=True, help="C identifier for the gfx_font_t")
    ap.add_argument("--out-dir", required=True, help="directory for <name>.c")
    ap.add_argument("--first", type=lambda s: int(s, 0), default=0x20)
    ap.add_argument("--last", type=lambda s: int(s, 0), default=0x7E)
    ap.add_argument(
        "--source-label",
        default=None,
        help="human readable source name used in the header comment (default: derived from --ttf)",
    )
    ap.add_argument("--verify", action="store_true", help="decode and re-check the packed bitmap")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    if not os.path.isfile(args.ttf):
        raise SystemExit(f"font not found: {args.ttf}")
    opsz = args.opsz if args.opsz is not None else _clamp_opsz(args.size)
    source_label = args.source_label or os.path.basename(args.ttf)
    font_path = args.ttf.replace("\\", "/")

    data = build_font(
        args.ttf,
        args.size,
        args.weight,
        opsz,
        args.first,
        args.last,
        args.name,
    )
    if args.verify:
        verify_font(data)

    os.makedirs(args.out_dir, exist_ok=True)
    out_path = os.path.join(args.out_dir, f"{args.name}.c")
    emit_c(data, out_path, source_label, font_path)

    if not args.quiet:
        total = data.bitmap_bytes + data.glyph_bytes
        print(
            f"{args.name}: {len(data.glyphs)} glyphs, bitmap {data.bitmap_bytes} B, "
            f"glyphs {data.glyph_bytes} B, total {total} B -> {out_path}"
        )
        if args.verify:
            print(f"{args.name}: verify OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
