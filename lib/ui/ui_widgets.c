/**
 * @file ui_widgets.c
 * @brief Status bar, glyphs, text helpers, bars and scrollbar for lib/ui.
 *
 * All glyphs are composed from gfx primitives (fills, triangles, circles), so
 * no extra assets are needed. Every routine starts with a band test: the scene
 * callback is invoked once per 16-row strip and must reject off-band work
 * before touching the canvas.
 */

#include "ui_widgets.h"

#include <stdio.h>
#include <string.h>

#include "fonts.h"
#include "gfx_render.h"

/* ------------------------------------------------------------------------- */
/* Geometry and text                                                         */
/* ------------------------------------------------------------------------- */

gfx_rect_t ui_band(const gfx_canvas_t *c) {
    gfx_rect_t r = {0, 0, 0, 0};
    if (c == NULL) {
        return r;
    }
    return gfx_rect_intersect(c->area, c->clip);
}

bool ui_visible(const gfx_canvas_t *c, int x, int y, int w, int h) {
    if (c == NULL || w <= 0 || h <= 0) {
        return false;
    }
    const gfx_rect_t band = ui_band(c);
    if (gfx_rect_empty(band)) {
        return false;
    }
    if (x >= (int)band.x + (int)band.w || x + w <= (int)band.x) {
        return false;
    }
    if (y >= (int)band.y + (int)band.h || y + h <= (int)band.y) {
        return false;
    }
    return true;
}

const char *ui_ascii(const char *s, char *buf, size_t cap) {
    if (buf == NULL || cap == 0) {
        return "";
    }
    if (s == NULL) {
        buf[0] = '\0';
        return buf;
    }
    size_t i = 0;
    for (; s[i] != '\0' && i + 1u < cap; i++) {
        const unsigned char ch = (unsigned char)s[i];
        buf[i] = (ch >= 0x20u && ch <= 0x7Eu) ? (char)ch : '?';
    }
    buf[i] = '\0';
    return buf;
}

void ui_text(gfx_canvas_t *c, const gfx_font_t *f, int x, int baseline, int max_w,
             const char *s, gfx_color_t col) {
    char tmp[UI_TITLE_MAX];
    if (c == NULL || f == NULL || s == NULL) {
        return;
    }
    ui_ascii(s, tmp, sizeof tmp);
    if (tmp[0] == '\0') {
        return;
    }
    if (max_w > 0) {
        (void)gfx_draw_text_clipped(c, f, x, baseline, max_w, tmp, col);
    } else {
        (void)gfx_draw_text(c, f, x, baseline, tmp, col);
    }
}

void ui_text_center(gfx_canvas_t *c, const gfx_font_t *f, int cx, int baseline, int max_w,
                    const char *s, gfx_color_t col) {
    char tmp[UI_TITLE_MAX];
    if (c == NULL || f == NULL || s == NULL) {
        return;
    }
    ui_ascii(s, tmp, sizeof tmp);
    if (tmp[0] == '\0') {
        return;
    }
    const int w = gfx_text_width(f, tmp);
    if (max_w > 0 && w > max_w) {
        (void)gfx_draw_text_clipped(c, f, cx - max_w / 2, baseline, max_w, tmp, col);
    } else {
        (void)gfx_draw_text(c, f, cx - w / 2, baseline, tmp, col);
    }
}

void ui_text_right(gfx_canvas_t *c, const gfx_font_t *f, int right, int baseline, int max_w,
                   const char *s, gfx_color_t col) {
    char tmp[UI_TITLE_MAX];
    if (c == NULL || f == NULL || s == NULL) {
        return;
    }
    ui_ascii(s, tmp, sizeof tmp);
    if (tmp[0] == '\0') {
        return;
    }
    const int w = gfx_text_width(f, tmp);
    if (max_w > 0 && w > max_w) {
        (void)gfx_draw_text_clipped(c, f, right - max_w, baseline, max_w, tmp, col);
    } else {
        (void)gfx_draw_text(c, f, right - w, baseline, tmp, col);
    }
}

void ui_format_time(char *buf, size_t cap, uint32_t ms, bool remaining) {
    if (buf == NULL || cap == 0u) {
        return;
    }
    const uint32_t total_s = ms / 1000u;
    const uint32_t h = total_s / 3600u;
    const uint32_t m = (total_s / 60u) % 60u;
    const uint32_t s = total_s % 60u;
    const char *sign = remaining ? "-" : "";
    if (h > 0u) {
        (void)snprintf(buf, cap, "%s%lu:%02lu:%02lu", sign, (unsigned long)h, (unsigned long)m,
                       (unsigned long)s);
    } else {
        (void)snprintf(buf, cap, "%s%lu:%02lu", sign, (unsigned long)m, (unsigned long)s);
    }
}

/* ------------------------------------------------------------------------- */
/* Invariant rectangles                                                      */
/* ------------------------------------------------------------------------- */

gfx_rect_t ui_rect_status(void) {
    gfx_rect_t r = {0, 0, UI_SCREEN_W, UI_STATUS_H};
    return r;
}

gfx_rect_t ui_rect_title(void) {
    /* ui_text_center() draws the title clipped to 150 px centred on x = 160,
     * i.e. x 85..235. Invalidate that whole area, not a narrower box. */
    gfx_rect_t r = {85, 2, 150, 18};
    return r;
}

gfx_rect_t ui_rect_play_glyph(void) {
    gfx_rect_t r = {4, 3, 16, 16};
    return r;
}

gfx_rect_t ui_rect_mode_icons(void) {
    gfx_rect_t r = {254, 3, 34, 16};
    return r;
}

gfx_rect_t ui_rect_battery(void) {
    gfx_rect_t r = {286, 3, 30, 16};
    return r;
}

gfx_rect_t ui_rect_perf(void) {
    gfx_rect_t r = {20, 2, 78, 18};
    return r;
}

/* ------------------------------------------------------------------------- */
/* Glyphs                                                                    */
/* ------------------------------------------------------------------------- */

void ui_draw_glyph_play(gfx_canvas_t *c, int x, int y, bool playing, gfx_color_t col) {
    if (!ui_visible(c, x, y, 10, 12)) {
        return;
    }
    if (playing) {
        gfx_fill_triangle(c, x, y, x, y + 11, x + 9, y + 5, col);
    } else {
        gfx_fill_rect(c, x, y, 3, 11, col);
        gfx_fill_rect(c, x + 6, y, 3, 11, col);
    }
}

void ui_draw_glyph_battery(gfx_canvas_t *c, int x, int y, uint8_t percent, bool charging,
                           gfx_color_t col) {
    if (!ui_visible(c, x - 1, y - 1, 25, 13)) {
        return;
    }
    if (percent > 100u) {
        percent = 100u;
    }
    gfx_draw_round_rect(c, x, y, 20, 10, 2, col);
    const int inner = (int)((18u * (uint32_t)percent) / 100u);
    if (inner > 0) {
        gfx_fill_rect(c, x + 1, y + 1, inner, 8, col);
    }
    gfx_fill_rect(c, x + 20, y + 3, 2, 4, col);
    if (charging) {
        /* Bolt in the middle: light on a filled cell, dark on an empty one. */
        const gfx_color_t bolt = (percent >= 50u) ? UI_COL_STATUS_BOT : col;
        const int bx = x + 7;
        const int by = y + 1;
        gfx_fill_triangle(c, bx + 3, by, bx, by + 5, bx + 4, by + 4, bolt);
        gfx_fill_triangle(c, bx + 3, by + 9, bx, by + 5, bx + 4, by + 5, bolt);
    }
}

void ui_draw_glyph_shuffle(gfx_canvas_t *c, int x, int y, gfx_color_t col) {
    if (!ui_visible(c, x, y, 10, 12)) {
        return;
    }
    gfx_hline(c, x, y + 2, 6, col);
    gfx_fill_triangle(c, x + 6, y, x + 6, y + 5, x + 9, y + 2, col);
    gfx_hline(c, x, y + 8, 6, col);
    gfx_fill_triangle(c, x + 6, y + 6, x + 6, y + 11, x + 9, y + 8, col);
    for (int i = 0; i < 6; i++) {
        gfx_fill_rect(c, x + 1 + i, y + 8 - i, 1, 1, col);
    }
}

void ui_draw_glyph_repeat(gfx_canvas_t *c, int x, int y, gfx_color_t col) {
    if (!ui_visible(c, x, y, 10, 12)) {
        return;
    }
    gfx_draw_round_rect(c, x, y + 2, 9, 7, 2, col);
    gfx_fill_triangle(c, x + 3, y, x + 8, y + 1, x + 3, y + 3, col);
}

static void glyph_fill(gfx_canvas_t *c, int x, int y, int w, int h, gfx_color_t col, uint8_t a) {
    if (a == 0u || w <= 0 || h <= 0) {
        return;
    }
    if (a >= 255u) {
        gfx_fill_rect(c, x, y, w, h, col);
    } else {
        gfx_fill_rect_alpha(c, x, y, w, h, col, a);
    }
}

void ui_draw_glyph_speaker_alpha(gfx_canvas_t *c, int x, int y, bool high, gfx_color_t col,
                                 uint8_t alpha) {
    if (alpha == 0u || !ui_visible(c, x, y, high ? 18 : 12, 14)) {
        return;
    }
    /* Body plus a stair-stepped cone, all through glyph_fill so the fade works. */
    glyph_fill(c, x, y + 5, 3, 5, col, alpha);
    for (int i = 0; i < 6; i++) {
        const int hh = 2 + i * 2;
        glyph_fill(c, x + 2 + i, y + 7 - hh / 2, 1, hh, col, alpha);
    }

    static const int8_t arc1[7][2] = {{3, -3}, {4, -2}, {4, -1}, {4, 0}, {4, 1}, {4, 2}, {3, 3}};
    static const int8_t arc2[11][2] = {{6, -5}, {7, -4}, {7, -3}, {7, -2}, {7, -1}, {7, 0},
                                       {7, 1},  {7, 2},  {7, 3},  {7, 4},  {6, 5}};
    const int cx = x + 7;
    const int cy = y + 7;
    for (size_t i = 0; i < sizeof arc1 / sizeof arc1[0]; i++) {
        glyph_fill(c, cx + arc1[i][0], cy + arc1[i][1], 1, 1, col, alpha);
    }
    if (high) {
        for (size_t i = 0; i < sizeof arc2 / sizeof arc2[0]; i++) {
            glyph_fill(c, cx + arc2[i][0], cy + arc2[i][1], 1, 1, col, alpha);
        }
    }
}

void ui_draw_glyph_speaker(gfx_canvas_t *c, int x, int y, bool high, gfx_color_t col) {
    ui_draw_glyph_speaker_alpha(c, x, y, high, col, 255u);
}

void ui_draw_glyph_chevron(gfx_canvas_t *c, int x, int y, gfx_color_t col) {
    if (!ui_visible(c, x, y, 8, 12)) {
        return;
    }
    for (int r = 0; r < 12; r++) {
        const int dx = (r < 6) ? r : 11 - r;
        gfx_fill_rect(c, x + dx, y + r, 2, 1, col);
    }
}

/* ------------------------------------------------------------------------- */
/* Composite widgets                                                         */
/* ------------------------------------------------------------------------- */

void ui_draw_selection(gfx_canvas_t *c, int x, int y, int w, int h) {
    if (!ui_visible(c, x, y, w, h)) {
        return;
    }
    gfx_fill_gradient_v(c, x, y, w, h, UI_COL_SEL_TOP, UI_COL_SEL_BOT);
}

void ui_draw_progress(gfx_canvas_t *c, int x, int y, int w, int h, uint32_t pos_ms,
                      uint32_t dur_ms, bool knob) {
    if (w <= 0 || h <= 0 || !ui_visible(c, x - 6, y - 6, w + 12, h + 12)) {
        return;
    }
    gfx_fill_round_rect(c, x, y, w, h, h / 2, UI_COL_TRACK);

    uint32_t fw = 0;
    if (dur_ms > 0u) {
        if (pos_ms > dur_ms) {
            pos_ms = dur_ms;
        }
        fw = (uint32_t)(((uint64_t)w * (uint64_t)pos_ms) / (uint64_t)dur_ms);
    }
    if (fw > 0u) {
        const int fwi = (int)fw;
        if (fwi <= h) {
            gfx_fill_rect(c, x, y, fwi, h, UI_COL_SEL_BOT);
        } else {
            gfx_fill_round_rect(c, x, y, fwi, h, h / 2, UI_COL_SEL_BOT);
            /* Two-tone top row suggests the selection gradient at 6 px height. */
            if (h >= 5) {
                gfx_fill_rect(c, x + 2, y + 1, fwi - 4, 2, UI_COL_SEL_TOP);
            }
        }
    }
    if (knob) {
        const int kx = x + (int)fw;
        const int ky = y + h / 2;
        gfx_fill_triangle(c, kx, ky - 5, kx, ky + 5, kx + 6, ky, UI_COL_TEXT);
        gfx_fill_triangle(c, kx, ky - 5, kx - 6, ky, kx, ky + 5, UI_COL_TEXT);
    }
}

void ui_draw_scrollbar(gfx_canvas_t *c, int y, int h, uint32_t first_row, uint32_t rows,
                       uint32_t total_rows, int x_off) {
    if (total_rows <= rows || h <= 0 || rows == 0u) {
        return;
    }
    const int x = UI_SCROLLBAR_X + x_off;
    if (!ui_visible(c, x - 2, y, UI_SCROLLBAR_W + 2, h)) {
        return;
    }
    gfx_fill_round_rect(c, x, y, UI_SCROLLBAR_W, h, 2, UI_COL_SCROLL_BG);

    int thumb = (int)(((int64_t)h * (int64_t)rows) / (int64_t)total_rows);
    if (thumb < 16) {
        thumb = 16;
    }
    if (thumb > h) {
        thumb = h;
    }
    const int max_first = (int)(total_rows - rows);
    int ty = y;
    if (max_first > 0) {
        ty += (int)(((int64_t)(h - thumb) * (int64_t)first_row) / (int64_t)max_first);
    }
    gfx_fill_round_rect(c, x, ty, UI_SCROLLBAR_W, thumb, 2, UI_COL_SCROLL_TH);
}

/** Baseline that centres the font's ascent..descent box inside the 22 px bar. */
static int status_title_baseline(void) {
    const int asc = (int)font_inter_14.ascent;
    int desc = (int)font_inter_14.line_height - asc;
    if (desc < 0) {
        desc = 0;
    }
    const int box = asc + desc;
    int top = (UI_STATUS_H - box) / 2;
    if (top < 0) {
        top = 0;
    }
    return top + asc; /* 15 px for font_inter_14: box y 1..19, ink y 4..17 */
}

void ui_draw_status_bar(gfx_canvas_t *c, const ui_status_t *st) {
    if (c == NULL || st == NULL) {
        return;
    }
    const gfx_rect_t band = ui_band(c);
    if (gfx_rect_empty(band) || band.y >= UI_STATUS_H) {
        return;
    }

    gfx_fill_gradient_v(c, 0, 0, UI_SCREEN_W, UI_STATUS_H, UI_COL_STATUS_TOP, UI_COL_STATUS_BOT);
    gfx_hline(c, 0, UI_STATUS_H - 1, UI_SCREEN_W, UI_COL_STATUS_LINE);

    if (st->has_track) {
        ui_draw_glyph_play(c, 8, 6, st->play == UI_PLAY_PLAYING, UI_COL_TEXT);
    }
    if (st->perf) {
        char tmp[20];
        (void)snprintf(tmp, sizeof tmp, "%u fps %u%%", (unsigned)st->fps, (unsigned)st->cpu);
        ui_text(c, &font_inter_12, 22, 15, 72, tmp, UI_COL_TEXT_2);
    }

    ui_text_center(c, &font_inter_14, UI_SCREEN_W / 2, status_title_baseline(), 150, st->title,
                   UI_COL_TEXT);

    if (st->shuffle) {
        ui_draw_glyph_shuffle(c, 258, 6, UI_COL_TEXT);
    }
    if (st->repeat) {
        ui_draw_glyph_repeat(c, 272, 6, UI_COL_TEXT);
    }
    ui_draw_glyph_battery(c, 290, 6, st->battery, st->charging, UI_COL_TEXT);
}
