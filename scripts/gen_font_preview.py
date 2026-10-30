#!/usr/bin/env python3
"""Render build/font_preview.png from the same rasteriser used by gen_font.py.

This is a human verification aid: it re-runs the Python rasteriser (a module
import of gen_font.py, not a parse of the generated C) and composites each
generated font's own metrics onto a dark canvas, so the preview shows exactly
what the firmware will draw.

Usage:
    python -I scripts/gen_font_preview.py [--out build/font_preview.png]
"""

from __future__ import annotations

import argparse
import os
import sys

sys.dont_write_bytecode = True  # keep the repo free of __pycache__
from typing import List, Tuple

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_font  # noqa: E402
from font_specs import FONT_SPECS, FONT_TTF  # noqa: E402

LINES = (
    "The quick brown fox jumps over 13 lazy dogs.",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz",
    "0123456789 .,;:!?'\"()[]{}@#$%&*+-=/<>_|~^`",
)
BG = (16, 18, 24)
FG = (236, 238, 242)
DIM = (128, 150, 170)
ACCENT = (110, 200, 255)


def _blit(canvas: Image.Image, mask: Image.Image, xy: Tuple[int, int], color) -> None:
    canvas.paste(tuple(color), xy, mask)


def main(argv: "List[str] | None" = None) -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    ap = argparse.ArgumentParser(description="Render a preview sheet of the generated fonts.")
    ap.add_argument("--ttf", default=os.path.join(root, FONT_TTF))
    ap.add_argument("--out", default=os.path.join(root, "build", "font_preview.png"))
    ap.add_argument("--width", type=int, default=960)
    args = ap.parse_args(argv)

    margin = 24
    width = args.width
    rendered: List[Tuple[str, gen_font.FontData, int]] = []
    for spec in FONT_SPECS:
        data = gen_font.build_font(
            args.ttf, spec["size"], spec["weight"], None, 0x20, 0x7E, spec["name"]
        )
        gen_font.verify_font(data)
        rendered.append((spec["note"], data, spec["size"]))

    # Measure the layout first.
    blocks: List[Tuple[gen_font.FontData, str, int, int]] = []
    total_h = margin
    for note, data, _size in rendered:
        header_h = 18
        body_h = data.line_height * len(LINES)
        blocks.append((data, note, header_h, body_h))
        total_h += header_h + body_h + 26
    total_h += margin

    canvas = Image.new("RGB", (width, total_h), BG)
    draw = ImageDraw.Draw(canvas)
    y = margin

    for data, note, header_h, body_h in blocks:
        # Header uses a small bitmap caption drawn with the 12px face if present.
        cap = f"{data.name}  {data.size}px / weight {data.weight} / opsz {data.opsz:g}  -  {note}"
        cap_font = rendered[0][1]
        mask, _ = gen_font.render_text_mask(cap_font, cap)
        _blit(canvas, mask, (margin, y + 2), ACCENT)
        y += header_h
        draw.line([(margin, y - 2), (width - margin, y - 2)], fill=(46, 52, 64), width=1)
        for line in LINES:
            mask, used = gen_font.render_text_mask(data, line)
            if margin + used < width - 8:
                _blit(canvas, mask, (margin, y), FG)
            else:  # wrap: shrink nothing, just clip is not allowed -> scale? keep simple
                _blit(canvas, mask, (margin, y), FG)
            y += data.line_height
        y += 26

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    canvas.save(args.out, "PNG", optimize=True)
    print(f"font preview: {width}x{total_h} -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
