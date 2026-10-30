/*
 * Anti-aliased bitmap font format for lib/gfx.
 *
 * A font is a set of glyphs stored as 4-bit per pixel alpha masks. Each glyph
 * bitmap is addressed by a byte offset into a single shared bitmap blob, so
 * fonts are one flat `const` object and live in flash.
 *
 * Bitmap layout: 4 bpp alpha, 2 pixels per byte, HIGH nibble first, each row
 * padded to a whole byte (row_bytes = (width + 1) / 2). Alpha 0 is fully
 * transparent, 15 is fully opaque.
 *
 * Generated fonts (see lib/gfx/fonts/) follow this layout; see
 * docs/design.md for the authoritative definition.
 */

#ifndef GFX_FONT_H
#define GFX_FONT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One glyph in a font. */
typedef struct {
    uint32_t bitmap_offset; /**< Byte offset into font->bitmap. */
    uint8_t width, height;  /**< Glyph bitmap size in px. */
    uint8_t x_advance;      /**< Pen advance in px. */
    int8_t x_offset;        /**< From pen x to bitmap left. */
    int8_t y_offset;        /**< From baseline to bitmap top (negative = above). */
} gfx_glyph_t;

/** A complete font: glyph table plus shared 4bpp alpha bitmap. */
typedef struct {
    const uint8_t *bitmap;    /**< 4bpp alpha, 2 px/byte, high nibble first, rows byte padded. */
    const gfx_glyph_t *glyphs;/**< glyphs[c - first]. */
    uint16_t first, last;     /**< Inclusive codepoint range (0x20..0x7E). */
    uint8_t line_height;      /**< Recommended baseline-to-baseline distance. */
    uint8_t ascent;           /**< Baseline distance from top of line. */
} gfx_font_t;

#ifdef __cplusplus
}
#endif

#endif /* GFX_FONT_H */
