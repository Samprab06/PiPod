#!/usr/bin/env python3
"""Generate the precomputed UI assets used by the direct-layer (flash->panel) path.

The renderer can send an opaque image straight from flash to the panel with
the RP2350 XIP stream unit (lib/flashstream + ili9341_blit_flash_async()). For
that to be safe the precomputed pixels must be *identical* to what the strip
renderer would draw, so this script re-implements the exact integer maths of
lib/gfx (gradients, 4bpp text blending, fills) and reuses the same rasteriser
as the device fonts (scripts/gen_font.py, imported, not re-parsed).

Assets (all `const`, so they live in flash, 4-byte aligned for the streamer):

  (a) status bar title area        UI_ASSET_TITLE_W x UI_ASSET_TITLE_H at
                                   (UI_ASSET_TITLE_X, UI_ASSET_TITLE_Y)
      One per static screen title. Only the title box is an asset; the left
      (play glyph / FPS) and right (shuffle / repeat / battery) icon areas stay
      CPU-drawn because icons are drawn on top of their background.

  (b) static menu rows             320 x 27 (a whole row), normal + selected
      For the Main and Music menus and the static "About" row of Settings.
      Settings rows with a dynamic subtitle (backlight %, On/Off, clock) are
      NOT precomputed.

The status bar and the rows use the palette/geometry from lib/ui (mirrored
below, with the C symbols named in the comments); run the host simulator
(bash sim/build.sh) to prove pixel-identity: it renders every screenshot once
through the direct path and once with gfx_direct_set_enabled(false) and
compares the framebuffers.

Usage:
    python -I scripts/gen_ui_assets.py                 # write lib/ui/assets/
    python -I scripts/gen_ui_assets.py --out-dir DIR
"""

from __future__ import annotations

import argparse
import os
import sys
from typing import Dict, List, Tuple

sys.dont_write_bytecode = True

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import gen_font  # noqa: E402  (same rasteriser as the device fonts)
from font_specs import FONT_TTF  # noqa: E402

# --------------------------------------------------------------------------- #
# Mirrored constants (keep in sync with lib/ui)
# --------------------------------------------------------------------------- #

# lib/gfx/gfx.h GFX_RGB
def rgb(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


# lib/ui/ui_widgets.h palette
COL_BG = rgb(255, 255, 255)
COL_TEXT = rgb(20, 20, 24)
COL_TEXT_2 = rgb(110, 110, 118)
COL_STATUS_TOP = rgb(250, 250, 252)
COL_STATUS_BOT = rgb(214, 216, 222)
COL_SEL_TOP = rgb(74, 144, 226)
COL_SEL_BOT = rgb(32, 98, 196)
COL_SEL_TEXT = rgb(255, 255, 255)
COL_CHEVRON = rgb(150, 150, 158)

# lib/ui/ui_internal.h / lib/ui/ui_widgets.c geometry
SCREEN_W = 320
STATUS_H = 22
TITLE_X = 85          # ui_rect_title()
TITLE_Y = 2
TITLE_W = 150
TITLE_H = 18
TITLE_BASELINE = 15   # status_title_baseline() for font_inter_14 (ascent 14)
ROW_W = 320
ROW_H = 27
ROW_TEXT_X = 12
ROW_TEXT_BASELINE = 20
ROW_CHEVRON_X = 300
ROW_CHEVRON_Y = 8

# Titles shown in the status bar (ui_screen_title()).
STATIC_TITLES = [
    "PiPod",
    "Music",
    "Settings",
    "Now Playing",
    "About",
    "Playlists",
    "Artists",
    "Albums",
    "Songs",
]

# (title, chevron) for every *static* row that is precomputed.
# Main menu rows are "Music >", "Now Playing >", "Shuffle Songs", "Settings >"
# (with/without a track only changes which of them are visible).
# Music menu rows are all chevron rows. Settings: only About is static.
STATIC_ROWS: List[Tuple[str, bool]] = [
    ("Music", True),
    ("Now Playing", True),
    ("Shuffle Songs", False),
    ("Settings", True),
    ("Playlists", True),
    ("Artists", True),
    ("Albums", True),
    ("Songs", True),
    ("About", True),
]

FONT_TITLE = ("font_inter_14", 14, 400)
FONT_ROW = ("font_inter_16sb", 16, 600)


# --------------------------------------------------------------------------- #
# lib/gfx primitives, re-implemented with the same integer maths
# --------------------------------------------------------------------------- #


def expand_r(c: int) -> int:
    v = (c >> 11) & 0x1F
    return (v << 3) | (v >> 2)


def expand_g(c: int) -> int:
    v = (c >> 5) & 0x3F
    return (v << 2) | (v >> 4)


def expand_b(c: int) -> int:
    v = c & 0x1F
    return (v << 3) | (v >> 2)


def pack_rgb(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def blend(fg: int, bg: int, alpha: int) -> int:
    """gfx_blend(): alpha 0 -> bg, 255 -> fg, otherwise 8-bit blend."""
    if alpha == 0:
        return bg
    if alpha == 255:
        return fg
    a = alpha
    ia = 255 - a
    fr, fgn, fb = expand_r(fg), expand_g(fg), expand_b(fg)
    br, bgn, bb = expand_r(bg), expand_g(bg), expand_b(bg)
    r = (fr * a + br * ia + 127) // 255
    g = (fgn * a + bgn * ia + 127) // 255
    b = (fb * a + bb * ia + 127) // 255
    return pack_rgb(r, g, b)


class Frame:
    """A small RGB565 surface with the lib/gfx drawing calls we need."""

    def __init__(self, w: int, h: int, color: int = 0) -> None:
        self.w = w
        self.h = h
        self.px = [color] * (w * h)

    def fill_rect(self, x: int, y: int, w: int, h: int, color: int) -> None:
        x0 = max(0, x)
        y0 = max(0, y)
        x1 = min(self.w, x + w)
        y1 = min(self.h, y + h)
        for yy in range(y0, y1):
            base = yy * self.w
            for xx in range(x0, x1):
                self.px[base + xx] = color

    def fill_gradient_v(self, x: int, y: int, w: int, h: int, top: int, bottom: int) -> None:
        """gfx_fill_gradient_v(): 16.16 row interpolation in 8-bit RGB."""
        if h <= 1:
            self.fill_rect(x, y, w, h, top)
            return
        tr, tg, tb = expand_r(top), expand_g(top), expand_b(top)
        br, bg, bb = expand_r(bottom), expand_g(bottom), expand_b(bottom)
        den = h - 1
        for row in range(h):
            sy = y + row
            if sy < 0 or sy >= self.h:
                continue
            t = (row * 65535) // den
            it = 65535 - t
            rr = (tr * it + br * t + 32767) // 65535
            gg = (tg * it + bg * t + 32767) // 65535
            bv = (tb * it + bb * t + 32767) // 65535
            color = pack_rgb(rr, gg, bv)
            self.fill_rect(x, sy, w, 1, color)

    def draw_text(self, font: "gen_font.FontData", x: int, baseline: int, s: str,
                  color: int) -> int:
        """gfx_draw_text(): per-glyph 4bpp alpha, high nibble first."""
        pen = x
        for ch in s:
            cp = ord(ch)
            if cp < font.first or cp > font.last:
                continue
            g = font.glyphs[cp - font.first]
            if g.width and g.height:
                bx = pen + g.x_offset
                by = baseline + g.y_offset
                alphas = g.alphas
                for gy in range(g.height):
                    py = by + gy
                    if py < 0 or py >= self.h:
                        continue
                    base = py * self.w
                    row = gy * g.width
                    for gx in range(g.width):
                        nib = alphas[row + gx]
                        if nib == 0:
                            continue
                        px = bx + gx
                        if px < 0 or px >= self.w:
                            continue
                        if nib == 15:
                            self.px[base + px] = color
                        else:
                            self.px[base + px] = blend(color, self.px[base + px], nib * 17)
            pen += g.x_advance
        return pen

    def text_width(self, font: "gen_font.FontData", s: str) -> int:
        return sum(font.glyphs[ord(c) - font.first].x_advance for c in s
                   if font.first <= ord(c) <= font.last)

    def draw_chevron(self, x: int, y: int, color: int) -> None:
        """ui_draw_glyph_chevron(): 12 rows, 2 px wide, mirrored after row 6."""
        for r in range(12):
            dx = r if r < 6 else 11 - r
            self.fill_rect(x + dx, y + r, 2, 1, color)


# --------------------------------------------------------------------------- #
# Asset builders
# --------------------------------------------------------------------------- #


def build_title_asset(font: "gen_font.FontData", title: str) -> Frame:
    """Status bar title box: gradient rows 2..19 plus the centred title."""
    fb = Frame(TITLE_W, TITLE_H)
    # Gradient is computed in screen space (h = UI_STATUS_H), then cropped.
    for row in range(TITLE_H):
        sy = TITLE_Y + row
        den = STATUS_H - 1
        t = (sy * 65535) // den
        it = 65535 - t
        tr, tg, tb = expand_r(COL_STATUS_TOP), expand_g(COL_STATUS_TOP), expand_b(COL_STATUS_TOP)
        br, bg, bb = expand_r(COL_STATUS_BOT), expand_g(COL_STATUS_BOT), expand_b(COL_STATUS_BOT)
        color = pack_rgb((tr * it + br * t + 32767) // 65535,
                         (tg * it + bg * t + 32767) // 65535,
                         (tb * it + bb * t + 32767) // 65535)
        fb.fill_rect(0, row, TITLE_W, 1, color)

    # ui_text_center(font_inter_14, cx = 160, baseline 15, max_w 150).
    width = fb.text_width(font, title)
    x0 = (SCREEN_W // 2) - width // 2 - TITLE_X
    fb.draw_text(font, x0, TITLE_BASELINE - TITLE_Y, title, COL_TEXT)
    return fb


def build_row_asset(font: "gen_font.FontData", title: str, chevron: bool,
                    selected: bool) -> Frame:
    """One 320x27 menu row exactly as ui_menu_draw() paints it."""
    fb = Frame(ROW_W, ROW_H)
    if selected:
        fb.fill_gradient_v(0, 0, ROW_W, ROW_H, COL_SEL_TOP, COL_SEL_BOT)
    else:
        fb.fill_rect(0, 0, ROW_W, ROW_H, COL_BG)

    title_col = COL_SEL_TEXT if selected else COL_TEXT
    chev_col = COL_SEL_TEXT if selected else COL_CHEVRON

    # ui_menu_draw(): text_right is 292 with a chevron, 306 without.
    text_right = 292 if chevron else 306
    _ = text_right  # max_w only matters for truncation; all titles fit
    fb.draw_text(font, ROW_TEXT_X, ROW_TEXT_BASELINE, title, title_col)
    if chevron:
        fb.draw_chevron(ROW_CHEVRON_X, ROW_CHEVRON_Y, chev_col)
    return fb


# --------------------------------------------------------------------------- #
# C emitter
# --------------------------------------------------------------------------- #

BANNER = "Generated by scripts/gen_ui_assets.py -- do not edit."


def emit_c_array(lines: List[str], name: str, frame: Frame) -> None:
    lines.append(f"/* {name}: {frame.w}x{frame.h} RGB565, row-major. */")
    lines.append(f"static const uint16_t {name}[{frame.w * frame.h}] __attribute__((aligned(4))) = {{")
    row: List[str] = []
    for value in frame.px:
        row.append(f"0x{value:04X},")
        if len(row) == 12:
            lines.append("    " + " ".join(row))
            row = []
    if row:
        lines.append("    " + " ".join(row))
    lines.append("};")
    lines.append("")


def slug(name: str) -> str:
    out = []
    for ch in name.lower():
        out.append(ch if ch.isalnum() else "_")
    return "".join(out)


def generate(out_dir: str) -> int:
    ttf = os.path.join(ROOT, FONT_TTF)
    if not os.path.isfile(ttf):
        raise SystemExit(f"missing source font: {ttf}")

    title_font = gen_font.build_font(ttf, FONT_TITLE[1], FONT_TITLE[2], None, 0x20, 0x7E,
                                    FONT_TITLE[0])
    row_font = gen_font.build_font(ttf, FONT_ROW[1], FONT_ROW[2], None, 0x20, 0x7E, FONT_ROW[0])
    gen_font.verify_font(title_font)
    gen_font.verify_font(row_font)

    titles: List[Tuple[str, str, Frame]] = []
    for title in STATIC_TITLES:
        titles.append((title, "s_title_" + slug(title), build_title_asset(title_font, title)))

    rows: List[Tuple[str, bool, str, str, Frame, Frame]] = []
    for title, chevron in STATIC_ROWS:
        base = "s_row_" + slug(title)
        normal = build_row_asset(row_font, title, chevron, False)
        selected = build_row_asset(row_font, title, chevron, True)
        rows.append((title, chevron, base + "_normal", base + "_selected", normal, selected))

    os.makedirs(out_dir, exist_ok=True)
    c_path = os.path.join(out_dir, "ui_assets.c")
    h_path = os.path.join(out_dir, "ui_assets.h")

    flash_bytes = sum(f.w * f.h * 2 for _, _, f in titles)
    flash_bytes += sum(f.w * f.h * 2 for _, _, _, _, n, s in rows for f in (n, s))

    # --- header ------------------------------------------------------------
    h: List[str] = []
    h.append("/*")
    h.append(" * Precomputed UI assets for the flash -> panel direct layer path.")
    h.append(" *")
    h.append(" * Pixel data matches what lib/ui draws into the strip buffers, so the")
    h.append(" * renderer may send it with ili9341_blit_flash_async() instead.")
    h.append(" *")
    h.append(f" * {BANNER}")
    h.append(" */")
    h.append("")
    h.append("#ifndef UI_ASSETS_H")
    h.append("#define UI_ASSETS_H")
    h.append("")
    h.append("#include <stdbool.h>")
    h.append("")
    h.append('#include "gfx_image.h"')
    h.append("")
    h.append("#ifdef __cplusplus")
    h.append('extern "C" {')
    h.append("#endif")
    h.append("")
    h.append("/* Status bar title box: the only status-bar rectangle that is an asset. */")
    h.append(f"#define UI_ASSET_TITLE_X {TITLE_X}")
    h.append(f"#define UI_ASSET_TITLE_Y {TITLE_Y}")
    h.append(f"#define UI_ASSET_TITLE_W {TITLE_W}")
    h.append(f"#define UI_ASSET_TITLE_H {TITLE_H}")
    h.append("")
    h.append("/* Static menu row (full width, one row high). */")
    h.append(f"#define UI_ASSET_ROW_W {ROW_W}")
    h.append(f"#define UI_ASSET_ROW_H {ROW_H}")
    h.append("")
    h.append(f"#define UI_ASSETS_TITLE_COUNT {len(titles)}u")
    h.append(f"#define UI_ASSETS_ROW_COUNT {len(rows)}u")
    h.append(f"/* Bytes of pixel data; the gfx_image_t descriptors add "
             f"{8 * (len(titles) + 2 * len(rows))} B. */")
    h.append(f"#define UI_ASSETS_FLASH_BYTES {flash_bytes}u")
    h.append("")
    h.append("/**")
    h.append(" * \\brief Precomputed status bar title box for a screen title.")
    h.append(" *")
    h.append(" * \\param title ui_screen_title() result.")
    h.append(" * \\return 150x18 image to place at (UI_ASSET_TITLE_X, UI_ASSET_TITLE_Y),")
    h.append(" *         or NULL when the title has no asset (the caller must draw it).")
    h.append(" */")
    h.append("const gfx_image_t *ui_asset_status_title(const char *title);")
    h.append("")
    h.append("/**")
    h.append(" * \\brief Precomputed static menu row.")
    h.append(" *")
    h.append(" * \\param title Row label.")
    h.append(" * \\param chevron True when the row draws a chevron.")
    h.append(" * \\param selected True for the highlighted variant.")
    h.append(" * \\return 320x27 image, or NULL when the row is not precomputed (dynamic")
    h.append(" *         subtitle, library list row, ...).")
    h.append(" */")
    h.append("const gfx_image_t *ui_asset_menu_row(const char *title, bool chevron, bool selected);")
    h.append("")
    h.append("#ifdef __cplusplus")
    h.append("}")
    h.append("#endif")
    h.append("")
    h.append("#endif /* UI_ASSETS_H */")
    h.append("")

    with open(h_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(h))

    # --- source ------------------------------------------------------------
    c: List[str] = []
    c.append("/*")
    c.append(" * Precomputed UI assets for the flash -> panel direct layer path.")
    c.append(" *")
    c.append(" * 4-byte aligned `const` arrays: they live in XIP flash and can be")
    c.append(" * streamed by the RP2350 XIP stream unit without a CPU copy.")
    c.append(" *")
    c.append(f" * {BANNER}")
    c.append(" */")
    c.append("")
    c.append("#include <stddef.h>")
    c.append("#include <string.h>")
    c.append("")
    c.append('#include "ui_assets.h"')
    c.append("")

    for _title, name, frame in titles:
        emit_c_array(c, name, frame)
    for _title, _chevron, nname, sname, normal, selected in rows:
        emit_c_array(c, nname, normal)
        emit_c_array(c, sname, selected)

    c.append("/* Image descriptors (8 B each on the target). */")
    for _title, name, frame in titles:
        c.append(f"static const gfx_image_t s_img_{name[2:]} = "
                 f"{{ {frame.w}u, {frame.h}u, {name} }};")
    c.append("")
    for _title, _chevron, nname, sname, normal, selected in rows:
        c.append(f"static const gfx_image_t s_img_{nname[2:]} = "
                 f"{{ {normal.w}u, {normal.h}u, {nname} }};")
        c.append(f"static const gfx_image_t s_img_{sname[2:]} = "
                 f"{{ {selected.w}u, {selected.h}u, {sname} }};")
    c.append("")

    c.append("typedef struct {")
    c.append("    const char *title;")
    c.append("    const gfx_image_t *image;")
    c.append("} ui_asset_title_entry_t;")
    c.append("")
    c.append("static const ui_asset_title_entry_t s_titles[] = {")
    for title, name, _frame in titles:
        c.append(f'    {{ "{title}", &s_img_{name[2:]} }},')
    c.append("};")
    c.append("")
    c.append("typedef struct {")
    c.append("    const char *title;")
    c.append("    bool chevron;")
    c.append("    const gfx_image_t *normal;")
    c.append("    const gfx_image_t *selected;")
    c.append("} ui_asset_row_entry_t;")
    c.append("")
    c.append("static const ui_asset_row_entry_t s_rows[] = {")
    for title, chevron, nname, sname, _n, _s in rows:
        flag = "true" if chevron else "false"
        c.append(f'    {{ "{title}", {flag}, &s_img_{nname[2:]}, &s_img_{sname[2:]} }},')
    c.append("};")
    c.append("")
    c.append("const gfx_image_t *ui_asset_status_title(const char *title) {")
    c.append("    if (title == NULL) {")
    c.append("        return NULL;")
    c.append("    }")
    c.append("    for (size_t i = 0; i < sizeof s_titles / sizeof s_titles[0]; i++) {")
    c.append("        if (strcmp(s_titles[i].title, title) == 0) {")
    c.append("            return s_titles[i].image;")
    c.append("        }")
    c.append("    }")
    c.append("    return NULL;")
    c.append("}")
    c.append("")
    c.append("const gfx_image_t *ui_asset_menu_row(const char *title, bool chevron, bool selected) {")
    c.append("    if (title == NULL) {")
    c.append("        return NULL;")
    c.append("    }")
    c.append("    for (size_t i = 0; i < sizeof s_rows / sizeof s_rows[0]; i++) {")
    c.append("        if (s_rows[i].chevron == chevron && strcmp(s_rows[i].title, title) == 0) {")
    c.append("            return selected ? s_rows[i].selected : s_rows[i].normal;")
    c.append("        }")
    c.append("    }")
    c.append("    return NULL;")
    c.append("}")
    c.append("")

    with open(c_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(c))

    print(f"ui_assets: {len(titles)} titles ({TITLE_W}x{TITLE_H}), {len(rows)} rows "
          f"({ROW_W}x{ROW_H}, normal + selected)")
    print(f"ui_assets: {flash_bytes} B of pixel data ({flash_bytes / 1024.0:.1f} KB), "
          f"{8 * (len(titles) + 2 * len(rows))} B of descriptors")
    print(f"ui_assets: {c_path}")
    print(f"ui_assets: {h_path}")
    return 0


def main(argv: "List[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Generate lib/ui/assets/ui_assets.c/.h")
    ap.add_argument("--out-dir", default=os.path.join(ROOT, "lib", "ui", "assets"))
    args = ap.parse_args(argv)
    return generate(args.out_dir)


if __name__ == "__main__":
    sys.exit(main())
