#!/usr/bin/env python3
"""Procedurally generate six 120x120 abstract album covers (RGB565 C arrays).

Everything is deterministic: each cover uses its own ``random.Random(seed)``.
Covers are painted at 4x and downsampled with LANCZOS so edges are clean.

Outputs:
    assets/demo_art/art_0.png .. art_5.png      (human-viewable proof)
    lib/ui_demo_assets/art_0.c .. art_5.c       (const gfx_image_t demo_art_N)
    lib/ui_demo_assets/demo_art.h               (externs + demo_art[6])
    lib/ui_demo_assets/demo_art.c               (pointer table)

Conversion to RGB565 is done with scripts/gen_image.py (imported).

Usage:
    python -I scripts/gen_demo_art.py
"""

from __future__ import annotations

import argparse
import math
import os
import random
import sys

sys.dont_write_bytecode = True  # keep the repo free of __pycache__
from typing import Callable, List, Sequence, Tuple

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_image  # noqa: E402  (local module, resolved above)

Color = Tuple[int, int, int]
RGBA = Tuple[int, int, int, int]
Stops = Sequence[Tuple[float, Color]]

SIZE = 120
SS = 4
W = SIZE * SS
SEED_BASE = 3620


# --------------------------------------------------------------------------- #
# Painting helpers (all coordinates are given in 120-px space and scaled by SS)
# --------------------------------------------------------------------------- #


def u(v: float) -> int:
    return int(round(v * SS))


def clamp8(v: float) -> int:
    return 0 if v <= 0 else 255 if v >= 255 else int(v)


def lerp(a: float, b: float, t: float) -> float:
    return a + (b - a) * t


def mix(c0: Color, c1: Color, t: float) -> Color:
    return (
        int(round(lerp(c0[0], c1[0], t))),
        int(round(lerp(c0[1], c1[1], t))),
        int(round(lerp(c0[2], c1[2], t))),
    )


def sample_stops(stops: Stops, t: float) -> Color:
    t = min(1.0, max(0.0, t))
    for i in range(len(stops) - 1):
        p0, c0 = stops[i]
        p1, c1 = stops[i + 1]
        if p0 <= t <= p1:
            span = (p1 - p0) or 1.0
            return mix(c0, c1, (t - p0) / span)
    return stops[-1][1]


def vertical_gradient(stops: Stops, size: int = W) -> Image.Image:
    img = Image.new("RGB", (size, size))
    draw = ImageDraw.Draw(img)
    for y in range(size):
        draw.line([(0, y), (size, y)], fill=sample_stops(stops, y / (size - 1)))
    return img


def overlay(img: Image.Image, paint: Callable[[ImageDraw.ImageDraw], None]) -> Image.Image:
    """Composite one translucent RGBA layer onto ``img``."""
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    paint(ImageDraw.Draw(layer))
    return Image.alpha_composite(img.convert("RGBA"), layer).convert("RGB")


def finish(
    img: Image.Image,
    rng: random.Random,
    grain: float = 5.0,
    vignette: float = 0.16,
) -> Image.Image:
    """Downsample to 120x120, then add deterministic grain + vignette."""
    img = img.resize((SIZE, SIZE), Image.Resampling.LANCZOS)
    px = img.load()
    cx = (SIZE - 1) / 2.0
    cy = (SIZE - 1) / 2.0
    maxd = math.hypot(cx, cy)
    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b = px[x, y]
            d = math.hypot(x - cx, y - cy) / maxd
            f = 1.0 - vignette * (d * d)
            n = rng.uniform(-grain, grain)
            px[x, y] = (clamp8(r * f + n), clamp8(g * f + n), clamp8(b * f + n))
    return img


# --------------------------------------------------------------------------- #
# 0: Golden hour (warm synth sunset)
# --------------------------------------------------------------------------- #


def art_sunset(rng: random.Random) -> Image.Image:
    img = vertical_gradient(
        [
            (0.00, (22, 14, 46)),
            (0.26, (66, 22, 80)),
            (0.48, (150, 40, 88)),
            (0.66, (222, 92, 76)),
            (0.84, (247, 160, 78)),
            (1.00, (255, 216, 128)),
        ]
    )
    cx, cy, radius = 60.0, 70.0, 31.0

    def glow(d: ImageDraw.ImageDraw) -> None:
        for i in range(10, 0, -1):
            rr = radius + i * 2.2
            alpha = int(7 + i * 3)
            d.ellipse(
                [u(cx - rr), u(cy - rr), u(cx + rr), u(cy + rr)],
                fill=(255, 220, 150, alpha),
            )

    img = overlay(img, glow)

    # Sun disc with horizontal cut-outs below the midline.
    d = ImageDraw.Draw(img)
    top = int(cy - radius)
    bottom = int(cy + radius)
    for ypx in range(top * SS, bottom * SS):
        y120 = ypx / SS
        dy = y120 - cy
        if abs(dy) > radius:
            continue
        half = math.sqrt(max(0.0, radius * radius - dy * dy))
        if dy > 0 and int((dy) // 2.05) % 3 == 2:
            continue  # stripe gap
        shade = mix((255, 240, 196), (255, 178, 120), max(0.0, dy / radius))
        d.line([(u(cx - half), ypx), (u(cx + half), ypx)], fill=shade)

    # Sea band: darkens the bottom third, with horizontal glints.
    def sea(d: ImageDraw.ImageDraw) -> None:
        d.rectangle([0, u(96), W, W], fill=(38, 18, 62, 110))
        for i, y120 in enumerate((100, 104, 109, 115)):
            half = 26 - i * 5
            d.line([(u(60 - half), u(y120)), (u(60 + half), u(y120))], fill=(255, 206, 140, 90), width=SS)

    img = overlay(img, sea)
    return finish(img, rng, grain=4.0)


# --------------------------------------------------------------------------- #
# 1: Deep field (teal rings + planet)
# --------------------------------------------------------------------------- #


def art_deep_field(rng: random.Random) -> Image.Image:
    img = vertical_gradient(
        [
            (0.00, (12, 56, 66)),
            (0.42, (8, 30, 44)),
            (0.72, (5, 16, 28)),
            (1.00, (3, 8, 16)),
        ]
    )
    ox, oy = 34.0, 40.0

    def rings(d: ImageDraw.ImageDraw) -> None:
        for i, r in enumerate(range(6, 150, 7)):
            t = i / 20.0
            col = mix((90, 235, 214), (36, 88, 140), t)
            alpha = int(120 * (1.0 - 0.75 * t))
            d.ellipse(
                [u(ox - r), u(oy - r), u(ox + r), u(oy + r)],
                outline=col + (alpha,),
                width=max(1, int(SS * 0.8)),
            )

    img = overlay(img, rings)

    def planet(d: ImageDraw.ImageDraw) -> None:
        px, py, pr = 92.0, 90.0, 27.0
        # Soft halo, dark disc, then a thin sunlit rim on the upper-left limb.
        for i in range(12, 0, -1):
            rr = pr + i * 1.4
            d.ellipse(
                [u(px - rr), u(py - rr), u(px + rr), u(py + rr)],
                fill=(40, 120, 150, 5 + i // 2),
            )
        d.ellipse([u(px - pr), u(py - pr), u(px + pr), u(py + pr)], fill=(8, 14, 28, 250))
        d.arc(
            [u(px - pr), u(py - pr), u(px + pr), u(py + pr)],
            start=170,
            end=330,
            fill=(110, 224, 255, 190),
            width=int(SS * 1.6),
        )
        d.arc(
            [u(px - pr * 0.86), u(py - pr * 0.86), u(px + pr * 0.86), u(py + pr * 0.86)],
            start=182,
            end=312,
            fill=(60, 150, 190, 70),
            width=max(1, SS // 2),
        )

    img = overlay(img, planet)

    def bubbles(d: ImageDraw.ImageDraw) -> None:
        for _ in range(28):
            x = rng.uniform(2, 118)
            y = rng.uniform(4, 116)
            r = rng.uniform(0.4, 1.9)
            a = int(rng.uniform(30, 120))
            d.ellipse([u(x - r), u(y - r), u(x + r), u(y + r)], fill=(180, 240, 255, a))

    img = overlay(img, bubbles)
    return finish(img, rng, grain=3.0)


# --------------------------------------------------------------------------- #
# 2: Bauhaus (flat paper geometry)
# --------------------------------------------------------------------------- #


def art_bauhaus(rng: random.Random) -> Image.Image:
    img = Image.new("RGB", (W, W), (243, 234, 216))
    d = ImageDraw.Draw(img)

    d.ellipse([u(-6), u(-8), u(52), u(50)], fill=(212, 84, 60))
    d.rectangle([u(64), u(64), u(114), u(114)], fill=(30, 58, 95))
    d.polygon([(u(70), u(18)), (u(112), u(18)), (u(112), u(58))], fill=(224, 178, 60))
    d.rectangle([u(18), u(76), u(52), u(112)], fill=(58, 92, 72))
    d.line([(u(8), u(118)), (u(112), u(8))], fill=(26, 24, 22), width=int(SS * 1.6))
    d.ellipse([u(44), u(84), u(58), u(98)], fill=(238, 232, 218))

    img = overlay(
        img,
        lambda ld: ld.ellipse([u(30), u(28), u(74), u(72)], outline=(243, 234, 216, 210), width=int(SS * 1.4)),
    )
    return finish(img, rng, grain=3.5, vignette=0.10)


# --------------------------------------------------------------------------- #
# 3: Neon grid (synthwave)
# --------------------------------------------------------------------------- #


def art_neon_grid(rng: random.Random) -> Image.Image:
    horizon = 86.0
    img = vertical_gradient(
        [
            (0.00, (20, 7, 40)),
            (0.35, (58, 12, 68)),
            (0.62, (146, 22, 96)),
            (0.72, (240, 58, 110)),
            (0.7201, (12, 5, 26)),
            (1.00, (5, 2, 14)),
        ]
    )
    cx = 60.0

    def sun(d: ImageDraw.ImageDraw) -> None:
        r = 27.0
        cy = 66.0
        top, bottom = int((cy - r) * SS), int((cy + r) * SS)
        for ypx in range(top, bottom):
            y120 = ypx / SS
            dy = y120 - cy
            if abs(dy) > r or y120 > horizon:
                continue
            half = math.sqrt(max(0.0, r * r - dy * dy))
            t = (dy + r) / (2 * r)
            col = mix((255, 236, 150), (255, 92, 130), t)
            if dy > 0 and int(dy // 1.9) % 3 == 2:
                continue
            d.line([(u(cx - half), ypx), (u(cx + half), ypx)], fill=col)
        # horizon glow
        for i in range(9, 0, -1):
            d.rectangle(
                [0, u(horizon - i * 0.8), W, u(horizon + i * 0.8)],
                fill=(255, 120, 170, 12 + i * 2),
            )

    img = overlay(img, sun)

    def grid(d: ImageDraw.ImageDraw) -> None:
        vpx, vpy = u(cx), u(horizon)
        for x120 in range(-80, 201, 20):
            d.line([(vpx, vpy), (u(x120), W)], fill=(90, 244, 240, 130), width=max(1, SS // 2))
        d.line([(0, vpy), (W, vpy)], fill=(255, 214, 236, 200), width=SS)
        # depth rows: spacing grows quadratically away from the horizon
        for k in range(1, 13):
            y120 = horizon + (k * k) * 0.42
            if y120 >= 120:
                break
            alpha = 150 - k * 8
            d.line([(0, u(y120)), (W, u(y120))], fill=(90, 244, 240, max(40, alpha)), width=max(1, SS // 2))
        # side mountains
        d.polygon([(0, vpy), (u(26), u(56)), (u(46), vpy)], fill=(16, 6, 30, 255))
        d.polygon([(u(74), vpy), (u(96), u(62)), (u(120), vpy)], fill=(16, 6, 30, 255))

    img = overlay(img, grid)

    # A few stars above the horizon.
    def stars(d: ImageDraw.ImageDraw) -> None:
        for _ in range(40):
            x = rng.uniform(0, 120)
            y = rng.uniform(0, horizon - 8)
            r = rng.uniform(0.25, 0.9)
            d.ellipse([u(x - r), u(y - r), u(x + r), u(y + r)], fill=(255, 245, 255, int(rng.uniform(60, 200))))

    img = overlay(img, stars)
    return finish(img, rng, grain=3.5)


# --------------------------------------------------------------------------- #
# 4: Aurora (sine ribbons)
# --------------------------------------------------------------------------- #


def art_aurora(rng: random.Random) -> Image.Image:
    img = vertical_gradient(
        [
            (0.00, (2, 10, 18)),
            (0.35, (4, 24, 28)),
            (0.62, (8, 38, 40)),
            (0.80, (4, 16, 24)),
            (1.00, (2, 6, 12)),
        ]
    )

    def stars(d: ImageDraw.ImageDraw) -> None:
        for _ in range(70):
            x = rng.uniform(0, 120)
            y = rng.uniform(0, 110)
            r = rng.uniform(0.2, 0.8)
            a = int(rng.uniform(40, 190))
            d.ellipse([u(x - r), u(y - r), u(x + r), u(y + r)], fill=(220, 245, 255, a))

    img = overlay(img, stars)

    ribbons = [
        ((60, 232, 160), 26, 46.0, 0.0, 17.0, 120),
        ((60, 196, 230), 30, 62.0, 1.3, 13.0, 110),
        ((150, 110, 245), 22, 34.0, 2.4, 9.5, 95),
        ((255, 120, 200), 16, 78.0, 0.7, 6.0, 70),
    ]
    for color, width, base, phase, amp, alpha in ribbons:
        def band(
            d: ImageDraw.ImageDraw,
            color: Color = color,
            width: int = width,
            base: float = base,
            phase: float = phase,
            amp: float = amp,
            alpha: int = alpha,
        ) -> None:
            pts: List[Tuple[int, int]] = []
            for i in range(-8, 129):
                y = base + amp * math.sin(i / 26.0 + phase) + 3.0 * math.sin(i / 7.0 + phase * 2)
                pts.append((u(i), u(y)))
            d.line(pts, fill=color + (alpha,), width=u(width / 22.0) + u(1), joint="curve")

        img = overlay(img, band)

    def fade(d: ImageDraw.ImageDraw) -> None:
        for i in range(24):
            y = 96 + i
            d.line([(0, u(y)), (W, u(y))], fill=(2, 6, 12, 8 + i * 2))

    img = overlay(img, fade)
    return finish(img, rng, grain=4.5)


# --------------------------------------------------------------------------- #
# 5: Prism (radial wedges)
# --------------------------------------------------------------------------- #


def art_prism(rng: random.Random) -> Image.Image:
    img = Image.new("RGB", (W, W), (14, 17, 22))
    d = ImageDraw.Draw(img)
    cx = cy = 60.0
    palette = [
        (255, 107, 107),
        (255, 179, 71),
        (255, 217, 61),
        (107, 203, 119),
        (77, 150, 255),
        (155, 131, 255),
        (255, 122, 191),
    ]
    n = len(palette)
    gap = 1.6  # degrees of background between wedges
    radius = 96.0
    for i, col in enumerate(palette):
        a0 = -90.0 + i * (360.0 / n) + 11.0 + gap / 2
        a1 = a0 + (360.0 / n) - gap
        pts = [(u(cx), u(cy))]
        steps = max(2, int((a1 - a0) / 3))
        for s in range(steps + 1):
            a = math.radians(a0 + (a1 - a0) * s / steps)
            pts.append((u(cx + radius * math.cos(a)), u(cy + radius * math.sin(a))))
        d.polygon(pts, fill=col)

    def rings(d: ImageDraw.ImageDraw) -> None:
        d.ellipse([u(cx - 62), u(cy - 62), u(cx + 62), u(cy + 62)], outline=(10, 12, 16, 130), width=int(SS * 1.2))
        d.ellipse([u(cx - 40), u(cy - 40), u(cx + 40), u(cy + 40)], outline=(255, 255, 255, 70), width=max(1, SS // 2))
        d.ellipse([u(cx - 18), u(cy - 18), u(cx + 18), u(cy + 18)], fill=(14, 17, 22, 255))
        d.ellipse([u(cx - 18), u(cy - 18), u(cx + 18), u(cy + 18)], outline=(245, 245, 245, 90), width=max(1, SS // 2))
        d.ellipse([u(cx - 4), u(cy - 4), u(cx + 4), u(cy + 4)], fill=(245, 245, 245, 230))

    img = overlay(img, rings)
    return finish(img, rng, grain=3.5, vignette=0.22)


# --------------------------------------------------------------------------- #
# Driver
# --------------------------------------------------------------------------- #

ARTISTS: Sequence[Tuple[str, Callable[[random.Random], Image.Image], str]] = (
    ("Golden Hour", art_sunset, "vertical sunset gradient, banded sun, sea glints"),
    ("Deep Field", art_deep_field, "teal concentric rings, planet rim, bubbles"),
    ("Bauhaus", art_bauhaus, "flat paper geometry: circle, block, triangle, line"),
    ("Neon Grid", art_neon_grid, "synthwave horizon, perspective grid, banded sun"),
    ("Aurora", art_aurora, "sine ribbons over a night gradient with stars"),
    ("Prism", art_prism, "radial colour wedges and concentric rings"),
)


def build_covers(png_dir: str) -> List[Image.Image]:
    os.makedirs(png_dir, exist_ok=True)
    covers: List[Image.Image] = []
    for i, (title, fn, _desc) in enumerate(ARTISTS):
        rng = random.Random(SEED_BASE + i * 7919)
        img = fn(rng)
        path = os.path.join(png_dir, f"art_{i}.png")
        img.save(path, "PNG", optimize=True)
        covers.append(img)
        print(f"art_{i}: {title} -> {path}")
    return covers


def write_demo_header(path: str) -> None:
    count = len(ARTISTS)
    lines = [
        "/*",
        " * Demo album art: procedurally generated abstract covers (no third-party",
        " * imagery). 120x120 RGB565 each.",
        " *",
        " * Generated by scripts/gen_demo_art.py -- do not edit.",
        " */",
        "",
        "#ifndef UI_DEMO_ASSETS_DEMO_ART_H",
        "#define UI_DEMO_ASSETS_DEMO_ART_H",
        "",
        '#include "gfx_image.h"',
        "",
        f"#define DEMO_ART_COUNT {count}",
        "",
    ]
    for i, (title, _fn, desc) in enumerate(ARTISTS):
        lines.append(f"extern const gfx_image_t demo_art_{i}; /* {title}: {desc} */")
    lines += [
        "",
        "/* Pointer table for indexing the covers. */",
        f"extern const gfx_image_t *const demo_art[{count}];",
        "",
        "#endif /* UI_DEMO_ASSETS_DEMO_ART_H */",
        "",
    ]
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))


def write_demo_source(path: str) -> None:
    count = len(ARTISTS)
    lines = [
        "/*",
        " * Pointer table for the generated demo album art.",
        " * Generated by scripts/gen_demo_art.py -- do not edit.",
        " */",
        "",
        '#include "demo_art.h"',
        "",
        f"const gfx_image_t *const demo_art[{count}] = {{",
    ]
    for i in range(count):
        lines.append(f"    &demo_art_{i},")
    lines += ["};", ""]
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))


def main(argv: "List[str] | None" = None) -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    ap = argparse.ArgumentParser(description="Generate demo album art (PNG + RGB565 C).")
    ap.add_argument("--png-dir", default=os.path.join(root, "assets", "demo_art"))
    ap.add_argument("--out-dir", default=os.path.join(root, "lib", "ui_demo_assets"))
    ap.add_argument("--dither", choices=("none", "ordered", "fs"), default="fs")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    covers = build_covers(args.png_dir)
    os.makedirs(args.out_dir, exist_ok=True)
    total = 0
    for i, img in enumerate(covers):
        name = f"demo_art_{i}"
        out_path = os.path.join(args.out_dir, f"art_{i}.c")
        w, h, nbytes = gen_image.convert_file(
            img, name, out_path, size=(SIZE, SIZE), dither=args.dither, strength=0.85
        )
        total += nbytes
        if not args.quiet:
            print(f"{name}: {w}x{h}, {nbytes} B -> {out_path}")

    write_demo_header(os.path.join(args.out_dir, "demo_art.h"))
    write_demo_source(os.path.join(args.out_dir, "demo_art.c"))
    print(f"demo art: {len(covers)} covers, {total} B of pixels total")
    return 0


if __name__ == "__main__":
    sys.exit(main())
