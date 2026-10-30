/**
 * @file ui_widgets.h
 * @brief Drawing primitives shared by the UI screens.
 *
 * Everything here draws through lib/gfx with screen coordinates (plus an
 * optional x offset used by the push/pop slide). Each entry point rejects work
 * that falls outside the current band before touching the canvas, because the
 * scene callback runs once per 16-row strip.
 */

#ifndef UI_WIDGETS_H
#define UI_WIDGETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfx.h"
#include "gfx_font.h"
#include "ui_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Palette (docs/ui_spec.md). */
#define UI_COL_BG          GFX_RGB(255, 255, 255)
#define UI_COL_TEXT        GFX_RGB(20, 20, 24)
#define UI_COL_TEXT_2      GFX_RGB(110, 110, 118)
#define UI_COL_STATUS_TOP  GFX_RGB(250, 250, 252)
#define UI_COL_STATUS_BOT  GFX_RGB(214, 216, 222)
#define UI_COL_STATUS_LINE GFX_RGB(160, 162, 170)
#define UI_COL_SEL_TOP     GFX_RGB(74, 144, 226)
#define UI_COL_SEL_BOT     GFX_RGB(32, 98, 196)
#define UI_COL_SEL_TEXT    GFX_RGB(255, 255, 255)
#define UI_COL_SEL_TEXT_2  GFX_RGB(224, 234, 248)
#define UI_COL_TRACK       GFX_RGB(210, 212, 218)
#define UI_COL_SCROLL_BG   GFX_RGB(230, 230, 234)
#define UI_COL_SCROLL_TH   GFX_RGB(150, 150, 158)
#define UI_COL_FRAME       GFX_RGB(200, 200, 206)
#define UI_COL_CHEVRON     GFX_RGB(150, 150, 158)

/** @brief The part of the canvas that the current band actually covers. */
gfx_rect_t ui_band(const gfx_canvas_t *c);

/** @brief True when the rectangle overlaps the band (cheap rejection). */
bool ui_visible(const gfx_canvas_t *c, int x, int y, int w, int h);

/**
 * @brief Copy @p s into @p buf, replacing non-ASCII bytes with '?'.
 * @return @p buf (never NULL; an empty string when @p s is NULL).
 */
const char *ui_ascii(const char *s, char *buf, size_t cap);

/** @brief Draw sanitised text, truncated with "..." when max_w > 0 and needed. */
void ui_text(gfx_canvas_t *c, const gfx_font_t *f, int x, int baseline, int max_w,
             const char *s, gfx_color_t col);

/** @brief Draw sanitised text centred on @p cx, clipped to @p max_w when > 0. */
void ui_text_center(gfx_canvas_t *c, const gfx_font_t *f, int cx, int baseline, int max_w,
                    const char *s, gfx_color_t col);

/** @brief Draw sanitised text right-aligned at @p right. */
void ui_text_right(gfx_canvas_t *c, const gfx_font_t *f, int right, int baseline, int max_w,
                   const char *s, gfx_color_t col);

/** @brief Format milliseconds as "m:ss" or "h:mm:ss"; prefix '-' when remaining. */
void ui_format_time(char *buf, size_t cap, uint32_t ms, bool remaining);

/** @brief Draw the complete status bar (gradient, title, glyphs, overlay). */
void ui_draw_status_bar(gfx_canvas_t *c, const ui_status_t *st);

/* Status bar invalidation helpers. */
gfx_rect_t ui_rect_status(void);
gfx_rect_t ui_rect_title(void);
gfx_rect_t ui_rect_play_glyph(void);
gfx_rect_t ui_rect_mode_icons(void);
gfx_rect_t ui_rect_battery(void);
gfx_rect_t ui_rect_perf(void);

/** @brief Play triangle (playing) or pause bars (not playing). */
void ui_draw_glyph_play(gfx_canvas_t *c, int x, int y, bool playing, gfx_color_t col);

/** @brief Draw the selection gradient (used by rows and the progress fill). */
void ui_draw_selection(gfx_canvas_t *c, int x, int y, int w, int h);

/** @brief Draw a rounded progress track with a proportional fill. */
void ui_draw_progress(gfx_canvas_t *c, int x, int y, int w, int h, uint32_t pos_ms,
                      uint32_t dur_ms, bool knob);

/** @brief Draw the list scrollbar. */
void ui_draw_scrollbar(gfx_canvas_t *c, int y, int h, uint32_t first_row, uint32_t rows,
                       uint32_t total_rows, int x_off);

/** @brief Battery outline + fill + optional charge bolt. */
void ui_draw_glyph_battery(gfx_canvas_t *c, int x, int y, uint8_t percent, bool charging,
                           gfx_color_t col);

/** @brief Small shuffle icon. */
void ui_draw_glyph_shuffle(gfx_canvas_t *c, int x, int y, gfx_color_t col);

/** @brief Small repeat icon. */
void ui_draw_glyph_repeat(gfx_canvas_t *c, int x, int y, gfx_color_t col);

/** @brief Speaker glyph with one (low) or two (high) sound arcs. */
void ui_draw_glyph_speaker(gfx_canvas_t *c, int x, int y, bool high, gfx_color_t col);

/**
 * @brief Speaker glyph composited with 8-bit alpha (volume overlay fade).
 * @param alpha 255 draws opaque, 0 draws nothing.
 */
void ui_draw_glyph_speaker_alpha(gfx_canvas_t *c, int x, int y, bool high, gfx_color_t col,
                                 uint8_t alpha);

/** @brief Menu chevron ">" drawn with 2 px strokes. */
void ui_draw_glyph_chevron(gfx_canvas_t *c, int x, int y, gfx_color_t col);

#ifdef __cplusplus
}
#endif

#endif /* UI_WIDGETS_H */
