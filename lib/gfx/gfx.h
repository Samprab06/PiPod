/**
 * \file gfx.h
 * \brief Framebuffer-less RGB565 drawing primitives.
 *
 * There is no framebuffer. A gfx_canvas_t is a window onto a small RGB565
 * buffer (a "strip") that covers an arbitrary rectangle of the screen. Every
 * primitive writes into that buffer, clipped to both the canvas area and an
 * explicit clip rectangle, so the same scene-drawing function can be replayed
 * for each strip the renderer needs.
 *
 * Coordinates are screen coordinates: x grows right, y grows down, the origin
 * is the top-left of the panel. The renderer (gfx_render.h) asks for 320 x 16
 * strips, but anything from a single pixel up to the whole panel works.
 *
 * Threading: all functions are unsynchronised. A single owner (the UI core)
 * calls them; nothing allocates, and there is no hidden state beyond the
 * caller's canvas.
 *
 * Colours are RGB565, see GFX_RGB(). Alpha blending uses 8-bit alpha.
 */

#ifndef GFX_H
#define GFX_H

#include <stdbool.h>
#include <stdint.h>

#include "gfx_font.h"
#include "gfx_image.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A 16-bit RGB565 colour. */
typedef uint16_t gfx_color_t;

/** Pack 8-bit RGB components into RGB565 (least significant bits truncated). */
#define GFX_RGB(r, g, b) ((gfx_color_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

/** Axis-aligned rectangle in screen coordinates. */
typedef struct {
    int16_t x, y; /**< Top-left corner. */
    int16_t w, h; /**< Size in pixels; w <= 0 or h <= 0 means empty. */
} gfx_rect_t;

/**
 * \brief A drawable RGB565 region.
 *
 * `buf` holds `area.w * area.h` pixels, row-major, with a row stride of
 * `area.w`. `area` is where the buffer lives on screen. Drawing is clipped to
 * `clip` intersected with `area`; nothing outside `area` is ever touched.
 */
typedef struct {
    uint16_t *buf;   /**< Pixel storage, stride = area.w. */
    gfx_rect_t area; /**< Screen rectangle covered by buf. */
    gfx_rect_t clip; /**< Additional clip rectangle (screen coordinates). */
} gfx_canvas_t;

/**
 * \brief Blend two RGB565 colours with 8-bit alpha.
 *
 * \param fg Foreground colour.
 * \param bg Background colour.
 * \param alpha 0 = fully transparent (bg), 255 = fully opaque (fg).
 * \return Blended RGB565 colour.
 */
static inline gfx_color_t gfx_blend(gfx_color_t fg, gfx_color_t bg, uint8_t alpha) {
    if (alpha == 0u) {
        return bg;
    }
    if (alpha == 255u) {
        return fg;
    }
    uint32_t a = alpha;
    uint32_t ia = 255u - a;

    uint32_t fr = (fg >> 11) & 0x1Fu;
    uint32_t fgn = (fg >> 5) & 0x3Fu;
    uint32_t fb = fg & 0x1Fu;
    uint32_t br = (bg >> 11) & 0x1Fu;
    uint32_t bgn = (bg >> 5) & 0x3Fu;
    uint32_t bb = bg & 0x1Fu;

    /* Expand 5/6/5 to 8 bits (replicate high bits), blend, re-quantise. */
    uint32_t r = (((fr << 3) | (fr >> 2)) * a + ((br << 3) | (br >> 2)) * ia + 127u) / 255u;
    uint32_t g = (((fgn << 2) | (fgn >> 4)) * a + ((bgn << 2) | (bgn >> 4)) * ia + 127u) / 255u;
    uint32_t b = (((fb << 3) | (fb >> 2)) * a + ((bb << 3) | (bb >> 2)) * ia + 127u) / 255u;
    return (gfx_color_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

/**
 * \brief Intersection of two rectangles.
 *
 * \return The overlap, or an empty rect (w or h = 0) if they do not overlap.
 */
gfx_rect_t gfx_rect_intersect(gfx_rect_t a, gfx_rect_t b);

/** \brief True when the rectangle has no area. */
bool gfx_rect_empty(gfx_rect_t r);

/**
 * \brief Smallest rectangle containing both inputs.
 *
 * Empty inputs are ignored, so the union of an empty rect and r is r.
 */
gfx_rect_t gfx_rect_union(gfx_rect_t a, gfx_rect_t b);

/**
 * \brief Replace the canvas clip rectangle.
 *
 * The stored clip is not intersected with the canvas area here; drawing always
 * uses clip intersect area. Pass an empty rect to suppress all drawing.
 *
 * \param c Canvas; must not be NULL.
 * \param clip New clip rectangle in screen coordinates.
 */
void gfx_set_clip(gfx_canvas_t *c, gfx_rect_t clip);

/**
 * \brief Fill the whole visible area (area intersect clip) with one colour.
 */
void gfx_fill(gfx_canvas_t *c, gfx_color_t col);

/**
 * \brief Fill an axis-aligned rectangle.
 *
 * \param c Canvas.
 * \param x Left edge (screen).
 * \param y Top edge (screen).
 * \param w Width in pixels; <= 0 draws nothing.
 * \param h Height in pixels; <= 0 draws nothing.
 * \param col Fill colour.
 */
void gfx_fill_rect(gfx_canvas_t *c, int x, int y, int w, int h, gfx_color_t col);

/**
 * \brief Fill a rectangle, blending with whatever is already in the canvas.
 *
 * \param alpha 0 = no-op, 255 = same as gfx_fill_rect().
 */
void gfx_fill_rect_alpha(gfx_canvas_t *c, int x, int y, int w, int h, gfx_color_t col, uint8_t alpha);

/**
 * \brief Fill a rounded rectangle with anti-aliased corners.
 *
 * The straight edges are exact; the four corner arcs are coverage blended with
 * the pixels already in the canvas, so the caller must draw a background
 * first.
 *
 * \param r Corner radius in pixels; clamped to min(w, h) / 2, negative = 0.
 */
void gfx_fill_round_rect(gfx_canvas_t *c, int x, int y, int w, int h, int r, gfx_color_t col);

/**
 * \brief Draw a 1 px anti-aliased rounded-rectangle outline.
 *
 * The outline lies just inside the [x, x+w) x [y, y+h) box. The interior is
 * left untouched.
 */
void gfx_draw_round_rect(gfx_canvas_t *c, int x, int y, int w, int h, int r, gfx_color_t col);

/**
 * \brief Fill a rectangle with a vertical linear gradient (top to bottom).
 *
 * The first row is \p top, the last row is \p bottom, intermediate rows are
 * linearly interpolated in 8-bit RGB space.
 */
void gfx_fill_gradient_v(gfx_canvas_t *c, int x, int y, int w, int h, gfx_color_t top, gfx_color_t bottom);

/** \brief Draw a 1 px horizontal line (w pixels starting at x). */
void gfx_hline(gfx_canvas_t *c, int x, int y, int w, gfx_color_t col);

/** \brief Draw a 1 px vertical line (h pixels starting at y). */
void gfx_vline(gfx_canvas_t *c, int x, int y, int h, gfx_color_t col);

/**
 * \brief Fill a circle with an anti-aliased edge.
 *
 * \param cx Centre x in screen coordinates.
 * \param cy Centre y in screen coordinates.
 * \param r Radius in pixels; r = 0 draws a single pixel, r < 0 does nothing.
 */
void gfx_fill_circle(gfx_canvas_t *c, int cx, int cy, int r, gfx_color_t col);

/**
 * \brief Fill a triangle (flat colour, no anti-aliasing).
 *
 * Winding order does not matter; degenerate (zero-area) triangles draw nothing.
 */
void gfx_fill_triangle(gfx_canvas_t *c, int x0, int y0, int x1, int y1, int x2, int y2, gfx_color_t col);

/**
 * \brief Draw an image at its native size, clipped.
 *
 * \param img Image; NULL or zero-sized images are ignored.
 */
void gfx_draw_image(gfx_canvas_t *c, int x, int y, const gfx_image_t *img);

/**
 * \brief Draw an image scaled into a w x h box using nearest-neighbour sampling.
 *
 * \param w Destination width in pixels; <= 0 draws nothing.
 * \param h Destination height in pixels; <= 0 draws nothing.
 */
void gfx_draw_image_scaled(gfx_canvas_t *c, int x, int y, int w, int h, const gfx_image_t *img);

/**
 * \brief Draw a single-line, anti-aliased string.
 *
 * Glyph bitmaps are 4bpp alpha; alpha 15 writes the colour directly, alpha 0
 * is skipped and everything else is blended with the canvas. Characters
 * outside [font->first, font->last] are skipped (they do not advance the pen),
 * so '\n' and UTF-8 continuation bytes are ignored.
 *
 * \param f Font.
 * \param x Pen x in screen coordinates.
 * \param baseline_y Baseline y in screen coordinates.
 * \param s NUL-terminated string.
 * \param col Text colour (blended with the background).
 * \return The pen x after the last glyph, i.e. where the next string starts.
 */
int gfx_draw_text(gfx_canvas_t *c, const gfx_font_t *f, int x, int baseline_y, const char *s, gfx_color_t col);

/**
 * \brief Draw a string truncated to max_w pixels, ending with "..." if needed.
 *
 * If the full string fits, it is drawn unchanged. Otherwise glyphs are dropped
 * from the end until the remainder plus "..." fits. If even "..." does not
 * fit, nothing is drawn. Drawing is additionally clipped to the
 * [x, x + max_w) window so a partially fitting glyph cannot spill.
 *
 * \param max_w Available width in pixels; <= 0 draws nothing.
 * \return The pen x after the last drawn glyph (including the ellipsis).
 */
int gfx_draw_text_clipped(gfx_canvas_t *c, const gfx_font_t *f, int x, int baseline_y, int max_w, const char *s, gfx_color_t col);

/**
 * \brief Width in pixels of a string as drawn by gfx_draw_text().
 *
 * Out-of-range characters are skipped, matching the drawing routine.
 *
 * \return Total x advance, 0 for NULL inputs.
 */
int gfx_text_width(const gfx_font_t *f, const char *s);

#ifdef __cplusplus
}
#endif

#endif /* GFX_H */
