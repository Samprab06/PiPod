#!/usr/bin/env python3
"""Convert a PNG/JPG into a C ``gfx_image_t`` of RGB565 pixels.

Matches ``lib/gfx/gfx_image.h`` (docs/design.md, section "Image format"):

    typedef struct { uint16_t width, height; const uint16_t *pixels; } gfx_image_t;

Pixels are native little-endian ``uint16_t`` values in 0xRRRRRGGGGGGBBBBB
layout, row-major, top row first. Optional Floyd-Steinberg or 4x4 ordered
dithering before the 5/6/5 quantisation.

The module API is imported by scripts/gen_demo_art.py.

Usage:
    python -I scripts/gen_image.py --in cover.png --name demo_art_0 \\
        --size 120x120 --dither fs --out-dir lib/ui_demo_assets
"""

from __future__ import annotations

import argparse
import math
import os
import sys
from typing import List, Optional, Sequence, Tuple

from PIL import Image

Scale = Tuple[int, int]

# --------------------------------------------------------------------------- #
# Preparation
# --------------------------------------------------------------------------- #


def flatten_rgb(img: Image.Image, background: Tuple[int, int, int] = (0, 0, 0)) -> Image.Image:
    """Convert any mode to RGB, compositing alpha over ``background``."""
    if img.mode == "RGB":
        return img
    if img.mode in ("RGBA", "LA", "PA") or (img.mode == "P" and "transparency" in img.info):
        rgba = img.convert("RGBA")
        base = Image.new("RGBA", rgba.size, background + (255,))
        return Image.alpha_composite(base, rgba).convert("RGB")
    return img.convert("RGB")


def fit_cover(img: Image.Image, size: Scale) -> Image.Image:
    """Scale with LANCZOS to cover ``size``, then center-crop to exactly it."""
    target_w, target_h = size
    src_w, src_h = img.size
    if src_w <= 0 or src_h <= 0:
        raise ValueError("empty source image")
    scale = max(target_w / src_w, target_h / src_h)
    new_w = max(target_w, int(math.ceil(src_w * scale)))
    new_h = max(target_h, int(math.ceil(src_h * scale)))
    resized = img.resize((new_w, new_h), Image.Resampling.LANCZOS)
    left = (new_w - target_w) // 2
    top = (new_h - target_h) // 2
    return resized.crop((left, top, left + target_w, top + target_h))


def prepare_image(
    source: "str | Image.Image",
    size: Optional[Scale] = None,
    background: Tuple[int, int, int] = (0, 0, 0),
) -> Image.Image:
    """Load (if a path) and normalise to an RGB image of the requested size."""
    img = Image.open(source) if isinstance(source, str) else source
    img.load()
    img = flatten_rgb(img, background)
    if size is not None:
        img = fit_cover(img, size)
    return img


# --------------------------------------------------------------------------- #
# RGB565 quantisation
# --------------------------------------------------------------------------- #

BAYER4 = (
    (0, 8, 2, 10),
    (12, 4, 14, 6),
    (3, 11, 1, 9),
    (15, 7, 13, 5),
)


def _clamp8(v: float) -> int:
    if v <= 0:
        return 0
    if v >= 255:
        return 255
    return int(v)


def pack565(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def _expand565(v: int) -> Tuple[int, int, int]:
    """Expand a 565 value back to 8-bit RGB (bit replication)."""
    r = (v >> 11) & 0x1F
    g = (v >> 5) & 0x3F
    b = v & 0x1F
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def quantise(
    img: Image.Image,
    dither: str = "none",
    strength: float = 1.0,
) -> List[int]:
    """Return row-major RGB565 values for ``img`` (an RGB image).

    ``dither`` is ``"none"``, ``"ordered"`` (4x4 Bayer) or ``"fs"``
    (Floyd-Steinberg error diffusion).
    """
    if img.mode != "RGB":
        img = flatten_rgb(img)
    w, h = img.size
    src = list(img.tobytes())  # flat r,g,b triplets

    if dither == "fs":
        buf = [float(v) for v in src]
        out = [0] * (w * h)
        for y in range(h):
            for x in range(w):
                i = (y * w + x) * 3
                r = _clamp8(buf[i])
                g = _clamp8(buf[i + 1])
                b = _clamp8(buf[i + 2])
                v = pack565(r, g, b)
                out[y * w + x] = v
                er, eg, eb = _expand565(v)
                dr = (r - er) * strength
                dg = (g - eg) * strength
                db = (b - eb) * strength
                if x + 1 < w:
                    j = i + 3
                    buf[j] += dr * 7 / 16
                    buf[j + 1] += dg * 7 / 16
                    buf[j + 2] += db * 7 / 16
                if y + 1 < h:
                    if x > 0:
                        j = i - 3 + w * 3
                        buf[j] += dr * 3 / 16
                        buf[j + 1] += dg * 3 / 16
                        buf[j + 2] += db * 3 / 16
                    j = i + w * 3
                    buf[j] += dr * 5 / 16
                    buf[j + 1] += dg * 5 / 16
                    buf[j + 2] += db * 5 / 16
                    if x + 1 < w:
                        j = i + 3 + w * 3
                        buf[j] += dr * 1 / 16
                        buf[j + 1] += dg * 1 / 16
                        buf[j + 2] += db * 1 / 16
        return out

    if dither == "ordered":
        out = [0] * (w * h)
        for y in range(h):
            for x in range(w):
                i = (y * w + x) * 3
                t = (BAYER4[y & 3][x & 3] + 0.5) / 16.0 - 0.5
                r = _clamp8(src[i] + t * 8.0 * strength)
                g = _clamp8(src[i + 1] + t * 4.0 * strength)
                b = _clamp8(src[i + 2] + t * 8.0 * strength)
                out[y * w + x] = pack565(r, g, b)
        return out

    if dither != "none":
        raise ValueError(f"unknown dither mode: {dither}")
    return [pack565(src[i], src[i + 1], src[i + 2]) for i in range(0, len(src), 3)]


# --------------------------------------------------------------------------- #
# C emitters
# --------------------------------------------------------------------------- #


def _c_comment_line(lines: List[str], title: str, source_note: str, extra: str) -> None:
    lines.append("/*")
    lines.append(f" * {title}")
    lines.append(f" * {source_note}")
    if extra:
        lines.append(f" * {extra}")
    lines.append(" * Generated by scripts/gen_image.py -- do not edit.")
    lines.append(" */")


def emit_image_c(
    path: str,
    name: str,
    width: int,
    height: int,
    pixels: Sequence[int],
    source_note: str = "",
    extra: str = "",
    per_line: int = 12,
) -> None:
    """Write ``const gfx_image_t <name>`` plus its static pixel array."""
    lines: List[str] = []
    _c_comment_line(lines, f"{name}: {width}x{height} RGB565.", source_note, extra)
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append('#include "gfx_image.h"')
    lines.append("")
    lines.append("/* Row-major RGB565, native uint16_t (0xRRRRRGGGGGGBBBBB). */")
    lines.append(f"static const uint16_t {name}_pixels[] = {{")
    for i in range(0, len(pixels), per_line):
        chunk = pixels[i : i + per_line]
        lines.append("    " + " ".join(f"0x{v:04X}," for v in chunk))
    lines.append("};")
    lines.append("")
    lines.append(
        f"_Static_assert(sizeof({name}_pixels) / sizeof({name}_pixels[0]) == "
        f"{width * height}u, \"{name}: pixel count must match width*height\");"
    )
    lines.append("")
    lines.append(f"const gfx_image_t {name} = {{")
    lines.append(f"    {width},")
    lines.append(f"    {height},")
    lines.append(f"    {name}_pixels,")
    lines.append("};")
    lines.append("")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))


def emit_image_header(path: str, names: Sequence[str], guard: str) -> None:
    """Write a small header declaring ``extern const gfx_image_t <name>;``."""
    lines = [
        "/*",
        " * Declarations for generated RGB565 images.",
        " * Generated by scripts/gen_image.py -- do not edit.",
        " */",
        "",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        '#include "gfx_image.h"',
        "",
    ]
    for name in names:
        lines.append(f"extern const gfx_image_t {name};")
    lines.append("")
    lines.append(f"#endif /* {guard} */")
    lines.append("")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))


def convert_file(
    src_path: "str | Image.Image",
    name: str,
    out_path: str,
    size: Optional[Scale] = None,
    dither: str = "none",
    strength: float = 1.0,
) -> Tuple[int, int, int]:
    """Load, resize, quantise and emit one image. Returns (w, h, flash bytes)."""
    img = prepare_image(src_path, size)
    pixels = quantise(img, dither, strength)
    w, h = img.size
    note = (
        f"Source image: {src_path.replace(chr(92), '/')}"
        if isinstance(src_path, str)
        else "Source image: generated procedurally (scripts/gen_demo_art.py)"
    )
    extra = f"Resized with LANCZOS + center-crop; dither={dither}."
    emit_image_c(out_path, name, w, h, pixels, note, extra)
    return w, h, len(pixels) * 2


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #


def _parse_size(text: str) -> Scale:
    try:
        w, h = text.lower().split("x")
        return int(w), int(h)
    except Exception as exc:  # noqa: BLE001 - argparse error path
        raise argparse.ArgumentTypeError(f"expected WxH, got {text!r}") from exc


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description="Convert a PNG/JPG to a gfx_image_t RGB565 C file.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--in", dest="src", required=True, help="input PNG/JPG")
    ap.add_argument("--name", required=True, help="C identifier for the gfx_image_t")
    ap.add_argument("--out-dir", default=None, help="directory for <name>.c")
    ap.add_argument("--out", default=None, help="explicit output .c path")
    ap.add_argument("--size", type=_parse_size, default=None, help="resize WxH (cover + center-crop)")
    ap.add_argument(
        "--dither",
        choices=("none", "ordered", "fs"),
        default="none",
        help="dithering before 565 quantisation (default none)",
    )
    ap.add_argument("--dither-strength", type=float, default=1.0)
    ap.add_argument("--header", default=None, help="optional .h to emit an extern declaration")
    ap.add_argument("--guard", default=None, help="include guard for --header")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    if not os.path.isfile(args.src):
        raise SystemExit(f"image not found: {args.src}")
    if not args.out and not args.out_dir:
        raise SystemExit("either --out or --out-dir is required")

    if args.out:
        out_path = args.out
    else:
        os.makedirs(args.out_dir, exist_ok=True)
        out_path = os.path.join(args.out_dir, f"{args.name}.c")

    w, h, nbytes = convert_file(
        args.src, args.name, out_path, args.size, args.dither, args.dither_strength
    )
    if args.header:
        guard = args.guard or (os.path.basename(args.header).upper().replace(".", "_").replace("-", "_") + "_")
        emit_image_header(args.header, [args.name], guard)
    if not args.quiet:
        print(f"{args.name}: {w}x{h}, {nbytes} B pixels -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
