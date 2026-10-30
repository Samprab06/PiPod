# Asset generators

Python scripts that turn a source font / images into the C tables the firmware
links. Everything under `lib/gfx/fonts/` and `lib/ui_demo_assets/` (except the
CMake files' hand-written scaffolding) is generated -- **do not edit generated
files by hand**, re-run the generator instead.

The two formats are `gfx_font_t`/`gfx_glyph_t` (4bpp anti-aliased alpha) and
`gfx_image_t` (RGB565, row-major, native `uint16_t`); the authoritative
definitions are in `lib/gfx/gfx_font.h` and `lib/gfx/gfx_image.h`.

## Prerequisites

- Python **3.11** with **Pillow 11.2** (`python -I -c "import PIL; print(PIL.__version__)"`).
- No numpy; everything is pure Python + Pillow.
- All commands below use `python -I` (isolated mode: ignores `PYTHONPATH` and
  user site-packages, so the run is reproducible).
- Font source: `assets/fonts/Inter-Variable.ttf`, license `assets/fonts/OFL.txt`
  (SIL Open Font License 1.1). The OFL requires the license text to travel with
  the font, so `OFL.txt` stays next to the TTF.

## Regenerate everything (the normal workflow)

```bash
python -I scripts/regen_assets.py            # fonts + demo art + preview sheet
python -I scripts/regen_assets.py --fonts    # fonts only (+ fonts.h / fonts.cmake)
python -I scripts/regen_assets.py --art      # demo art only (PNG + C)
python -I scripts/regen_assets.py --preview  # build/font_preview.png only
python -I scripts/regen_assets.py --no-verify
```

Outputs:

| Output | Producer |
|---|---|
| `lib/gfx/fonts/font_inter_12.c`, `..._14.c`, `..._16sb.c`, `..._20b.c`, `..._28b.c` | `gen_font.py` via `regen_assets.py` |
| `lib/gfx/fonts/fonts.h`, `lib/gfx/fonts/fonts.cmake` | `regen_assets.py` (from `font_specs.py`) |
| `assets/demo_art/art_0.png` .. `art_5.png` | `gen_demo_art.py` |
| `lib/ui_demo_assets/art_0.c` .. `art_5.c`, `demo_art.h`, `demo_art.c` | `gen_demo_art.py` |
| `build/font_preview.png` | `gen_font_preview.py` |

`scripts/font_specs.py` is the single source of truth for the font instances
(name, px size, weight, purpose); `regen_assets.py` and `gen_font_preview.py`
both import it.

## gen_font.py

Rasterises a variable TTF with FreeType (`ImageFont.truetype` +
`set_variation_by_axes([optical size, weight])`), quantises the 8-bit alpha to
4 bits and emits one `const gfx_font_t`.

```bash
python -I scripts/gen_font.py \
    --ttf assets/fonts/Inter-Variable.ttf \
    --size 16 --weight 600 --name font_inter_16sb \
    --out-dir lib/gfx/fonts --verify
```

| Flag | Meaning |
|---|---|
| `--ttf PATH` | source font (variable or static) |
| `--size PX` | pixel EM size |
| `--weight W` | variable weight axis (100..900) |
| `--opsz N` | optical size axis, default `clamp(size, 14, 32)` |
| `--name C_IDENT` | name of the `gfx_font_t` and of its two static arrays |
| `--out-dir DIR` | writes `DIR/<name>.c` |
| `--first 0x20 --last 0x7E` | inclusive codepoint range (default ASCII) |
| `--verify` | decode the packed 4bpp bitmap and assert it matches the quantised raster for every glyph, check offsets/advance/ranges |
| `--source-label` | text used in the header comment (default: TTF file name) |

Raster details:

- Each glyph is drawn with anti-aliasing into an `L` canvas anchored at the
  pen/baseline, cropped to the **tight ink bbox**, then the alpha is quantised to
  4 bits with round-to-nearest (`round(a / 17)`), and any edge row/column that
  rounded to zero is trimmed again.
- Space (or any glyph with no ink) gets `width = height = 0`, `x_offset = y_offset = 0`
  and keeps its advance.
- `x_advance = round(font.getlength(ch))`; `x_offset` is pen-x to bitmap-left;
  `y_offset` is baseline to bitmap-top (**negative = above the baseline**),
  computed as `ink_top - ascent`.
- `ascent = font.getmetrics()[0]`, `line_height = ascent + descent + max(1, round(0.08 * size))`.
- Packing: 2 px per byte, **high nibble first**, every row padded to a whole byte.

### Layout of the emitted file

```c
static const uint8_t     <name>_bitmap[];   /* 4bpp alpha          */
static const gfx_glyph_t <name>_glyphs[];   /* <last>-<first>+1    */
const gfx_font_t <name> = { bitmap, glyphs, first, last, line_height, ascent };
```

`sizeof(gfx_glyph_t) == 12` bytes (4-byte `bitmap_offset` + 5 x `uint8_t`/`int8_t`
padded to the struct's 4-byte alignment), `sizeof(gfx_font_t) == 16` on the
32-bit target.

### Measured flash cost (Inter, U+0020..U+007E, 95 glyphs)

| font | bitmap | glyph table | arrays | + struct (target) |
|---|---:|---:|---:|---:|
| `font_inter_12` | 2780 B | 1140 B | 3920 B | 3936 B |
| `font_inter_14` | 3703 B | 1140 B | 4843 B | 4859 B |
| `font_inter_16sb` | 4844 B | 1140 B | 5984 B | 6000 B |
| `font_inter_20b` | 7469 B | 1140 B | 8609 B | 8625 B |
| `font_inter_28b` | 13767 B | 1140 B | 14907 B | 14923 B |
| **total** | 32563 B | 5700 B | **38263 B** | **38343 B** |

The generated set is **38,343 B (~38 KB)** of flash, but `pipod` links only the
four faces it uses — **23,420 B (~23 KB)** — because `font_inter_28b` (the 28 px
face) is unreferenced and dropped by `--gc-sections`.

## gen_font_preview.py

Renders `build/font_preview.png` for human inspection. It **imports
`gen_font.py`** and re-runs the rasteriser, then composites each face using the
generated metrics (`x_advance`, `x_offset`, `y_offset`) -- it does not parse the
C. If the preview looks right, the packed tables are right.

```bash
python -I scripts/gen_font_preview.py --out build/font_preview.png
```

## gen_image.py

Converts any PNG/JPG into `const gfx_image_t <name>`.

```bash
python -I scripts/gen_image.py --in cover.jpg --name album_cover \
    --size 240x240 --dither fs --out-dir lib/ui_demo_assets
```

| Flag | Meaning |
|---|---|
| `--in PATH` | input PNG/JPG (alpha is composited over black) |
| `--name C_IDENT` | name of the `gfx_image_t` / static pixel array |
| `--out-dir DIR` | writes `DIR/<name>.c` (or `--out FILE.c` for an explicit path) |
| `--size WxH` | LANCZOS scale-to-cover + center-crop to exactly `WxH` |
| `--dither none\|ordered\|fs` | no dithering (default), 4x4 Bayer ordered, Floyd-Steinberg |
| `--dither-strength F` | error/threshold multiplier, default `1.0` |
| `--header PATH` | optionally emit a header with `extern const gfx_image_t <name>;` |

Pixel values are native `uint16_t` `0xRRRRRGGGGGGBBBBB`, row-major from the top
row. The module API (`prepare_image`, `quantise`, `emit_image_c`,
`convert_file`) is imported by `gen_demo_art.py`.

## gen_demo_art.py

Procedurally paints six 120x120 abstract album covers -- **no third-party
imagery**. Every cover is deterministic (per-cover `random.Random(3620 + i*7919)`),
painted at 4x and downsampled with LANCZOS, then finished with a fixed grain +
vignette pass. Palettes/techniques: sunset gradient with a banded sun, teal
concentric rings + planet, flat Bauhaus geometry, synthwave grid, aurora sine
ribbons, radial prism wedges.

```bash
python -I scripts/gen_demo_art.py --dither fs
```

Steps: write `assets/demo_art/art_<i>.png`, convert each through
`gen_image.convert_file()` to `lib/ui_demo_assets/art_<i>.c`, then write
`demo_art.h` (six externs, `DEMO_ART_COUNT`, and
`extern const gfx_image_t *const demo_art[6];`) and `demo_art.c` (the pointer
table).

Each cover costs 120 * 120 * 2 = **28800 B** of pixels; six covers = 172800 B,
plus 6 x 8 B structs and a 24 B pointer table (32-bit target).

## Wiring into CMake

`lib/gfx/CMakeLists.txt` only needs:

```cmake
include("${CMAKE_CURRENT_LIST_DIR}/fonts/fonts.cmake")
```

`fonts.cmake` adds the five `.c` files to the `gfx` target and puts
`lib/gfx/fonts/` on the public include path, so firmware just does
`#include "fonts.h"`.

`lib/ui_demo_assets/CMakeLists.txt` defines the `STATIC` library
`ui_demo_assets` (all `art_*.c` + `demo_art.c`), exports its own include dir and
links `gfx` PUBLIC for `gfx_image.h`. Link it only into apps that need the demo
artwork (e.g. `apps/pipod`).

## Host syntax check (no Pico SDK needed)

The generated files only need `gfx_font.h` and `gfx_image.h`. With stub copies
of those headers in a scratch directory (outside the repo):

```bash
STUB="$TEMP/gfx-stub"   # stub gfx_font.h + gfx_image.h
for f in lib/gfx/fonts/*.c lib/ui_demo_assets/*.c; do
    gcc -std=c11 -Wall -Wextra -c "$f" -I"$STUB" -o /dev/null || echo "FAIL $f"
done
```

All generated files must compile warning-free under `-Wall -Wextra`.

## License

Font: Inter by Rasmus Andersson, SIL Open Font License 1.1
(`assets/fonts/OFL.txt`). The generated bitmaps are derived from the font and
carry the same license. Demo art is generated by `gen_demo_art.py`; no
copyrighted images are used.
