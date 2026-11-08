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
#include "ui_assets_sel.h"

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

gfx_rect_t ui_rect_status_left(void) {
    /* Everything left of the title box: play glyph and the FPS overlay. */
    gfx_rect_t r = {0, 0, UI_ASSET_TITLE_X, UI_STATUS_H};
    return r;
}

gfx_rect_t ui_rect_status_right(void) {
    /* Everything right of the title box: shuffle, repeat and battery. */
    gfx_rect_t r = {UI_ASSET_TITLE_X + UI_ASSET_TITLE_W, 0,
                    UI_SCREEN_W - (UI_ASSET_TITLE_X + UI_ASSET_TITLE_W), UI_STATUS_H};
    return r;
}

gfx_rect_t ui_rect_title(void) {
    /* The title box is a precomputed asset (lib/ui/assets) and the only status
     * bar rectangle the direct flash path can send; keep the geometry in sync
     * with the asset generator through the UI_ASSET_* constants. */
    gfx_rect_t r = {UI_ASSET_TITLE_X, UI_ASSET_TITLE_Y, UI_ASSET_TITLE_W, UI_ASSET_TITLE_H};
    return r;
}

void ui_invalidate_status(void) {
    /* Rows above and below the title box first (plain merge), then the three
     * side-by-side pieces exactly: merging them would make the union wider than
     * the title asset and silently disable the direct path. */
    gfx_rect_t top = {UI_ASSET_TITLE_X, 0, UI_ASSET_TITLE_W, UI_ASSET_TITLE_Y};
    gfx_rect_t bottom = {UI_ASSET_TITLE_X, UI_ASSET_TITLE_Y + UI_ASSET_TITLE_H, UI_ASSET_TITLE_W,
                         UI_STATUS_H - (UI_ASSET_TITLE_Y + UI_ASSET_TITLE_H)};
    ui_invalidate(top);
    ui_invalidate(bottom);
    gfx_invalidate_exact(ui_rect_status_left());
    gfx_invalidate_exact(ui_rect_status_right());
    gfx_invalidate_exact(ui_rect_title());
}

gfx_rect_t ui_rect_play_glyph(void) {
    gfx_rect_t r = {UI_RECT_PLAY_GLYPH_X, UI_RECT_PLAY_GLYPH_Y, UI_RECT_PLAY_GLYPH_W,
                    UI_RECT_PLAY_GLYPH_H};
    return r;
}

gfx_rect_t ui_rect_mode_icons(void) {
    gfx_rect_t r = {UI_RECT_MODE_ICONS_X, UI_RECT_MODE_ICONS_Y, UI_RECT_MODE_ICONS_W,
                    UI_RECT_MODE_ICONS_H};
    return r;
}

gfx_rect_t ui_rect_battery(void) {
    gfx_rect_t r = {UI_RECT_BATTERY_X, UI_RECT_BATTERY_Y, UI_RECT_BATTERY_W,
                    UI_RECT_BATTERY_H};
    return r;
}

gfx_rect_t ui_rect_perf(void) {
    gfx_rect_t r = {UI_RECT_PERF_X, UI_RECT_PERF_Y, UI_RECT_PERF_W, UI_RECT_PERF_H};
    return r;
}

/* ------------------------------------------------------------------------- */
/* Glyphs                                                                    */
/* ------------------------------------------------------------------------- */

void ui_draw_glyph_play(gfx_canvas_t *c, int x, int y, bool playing, gfx_color_t col) {
    if (!ui_visible(c, x, y, UI_STATUS_PLAY_W, UI_STATUS_PLAY_H)) {
        return;
    }
    if (playing) {
        gfx_fill_triangle(c, x, y, x, y + UI_STATUS_PLAY_H - 1, x + UI_STATUS_PLAY_W - 1,
                          y + (UI_STATUS_PLAY_H - 1) / 2, col);
    } else {
        const int bar_w = (UI_STATUS_PLAY_W * 3) / 10;
        const int gap_x = (UI_STATUS_PLAY_W * 6) / 10;
        gfx_fill_rect(c, x, y, bar_w, UI_STATUS_PLAY_H - 1, col);
        gfx_fill_rect(c, x + gap_x, y, bar_w, UI_STATUS_PLAY_H - 1, col);
    }
}

void ui_draw_glyph_battery(gfx_canvas_t *c, int x, int y, uint8_t percent, bool charging,
                           gfx_color_t col) {
    if (!ui_visible(c, x - 1, y - 1, UI_STATUS_BATT_W + 5, UI_STATUS_BATT_H + 3)) {
        return;
    }
    if (percent > 100u) {
        percent = 100u;
    }
    gfx_draw_round_rect(c, x, y, UI_STATUS_BATT_W, UI_STATUS_BATT_H, 2, col);
    const int inner = (int)(((uint32_t)(UI_STATUS_BATT_W - 2) * (uint32_t)percent) / 100u);
    if (inner > 0) {
        gfx_fill_rect(c, x + 1, y + 1, inner, UI_STATUS_BATT_H - 2, col);
    }
    gfx_fill_rect(c, x + UI_STATUS_BATT_W, y + (UI_STATUS_BATT_H * 3) / 10,
                  UI_STATUS_BATT_NUB_W, (UI_STATUS_BATT_H * 4) / 10, col);
    if (charging) {
        /* Bolt in the middle: light on a filled cell, dark on an empty one. */
        const gfx_color_t bolt = (percent >= 50u) ? UI_COL_STATUS_BOT : col;
        const int bw = UI_STATUS_BATT_W / 5;
        const int bx = x + (UI_STATUS_BATT_W * 7) / 20;
        const int by = y + 1;
        const int h5 = (UI_STATUS_BATT_H * 5) / 10;
        const int h4 = (UI_STATUS_BATT_H * 4) / 10;
        const int h9 = (UI_STATUS_BATT_H * 9) / 10;
        gfx_fill_triangle(c, bx + bw - 1, by, bx, by + h5, bx + bw, by + h4, bolt);
        gfx_fill_triangle(c, bx + bw - 1, by + h9, bx, by + h5, bx + bw, by + h5, bolt);
    }
}

void ui_draw_glyph_shuffle(gfx_canvas_t *c, int x, int y, gfx_color_t col) {
    if (!ui_visible(c, x, y, UI_STATUS_MODE_W, UI_STATUS_MODE_H)) {
        return;
    }
    const int len = (UI_STATUS_MODE_W * 6) / 10;
    const int y2 = y + (UI_STATUS_MODE_H * 2) / 12;
    const int y8 = y + (UI_STATUS_MODE_H * 8) / 12;
    gfx_hline(c, x, y2, len, col);
    gfx_fill_triangle(c, x + len, y, x + len, y + (UI_STATUS_MODE_H * 5) / 12,
                      x + (UI_STATUS_MODE_W * 9) / 10, y2, col);
    gfx_hline(c, x, y8, len, col);
    gfx_fill_triangle(c, x + len, y + (UI_STATUS_MODE_H * 6) / 12, x + len,
                      y + (UI_STATUS_MODE_H * 11) / 12, x + (UI_STATUS_MODE_W * 9) / 10, y8, col);
    for (int i = 0; i < len; i++) {
        gfx_fill_rect(c, x + 1 + i, y8 - i, 1, 1, col);
    }
}

void ui_draw_glyph_repeat(gfx_canvas_t *c, int x, int y, gfx_color_t col) {
    if (!ui_visible(c, x, y, UI_STATUS_MODE_W, UI_STATUS_MODE_H)) {
        return;
    }
    gfx_draw_round_rect(c, x, y + (UI_STATUS_MODE_H * 2) / 12, (UI_STATUS_MODE_W * 9) / 10,
                        (UI_STATUS_MODE_H * 7) / 12, 2, col);
    gfx_fill_triangle(c, x + (UI_STATUS_MODE_W * 3) / 10, y,
                      x + (UI_STATUS_MODE_W * 8) / 10, y + 1,
                      x + (UI_STATUS_MODE_W * 3) / 10, y + 3, col);
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

/** Scale a 320-layout speaker coordinate to the active layout. */
static int spk_px(int v) {
    return (v * UI_VOL_SPK_NUM) / UI_VOL_SPK_DEN;
}

void ui_draw_glyph_speaker_alpha(gfx_canvas_t *c, int x, int y, bool high, gfx_color_t col,
                                 uint8_t alpha) {
    const int box_w = (high ? 18 : 12);
    if (alpha == 0u || !ui_visible(c, x, y, spk_px(box_w), spk_px(14))) {
        return;
    }
    /* Body plus a stair-stepped cone, all through glyph_fill so the fade works.
     * Geometry is written in 320-layout units and scaled by UI_VOL_SPK_NUM/DEN
     * so the 480 layout gets a 1.5x glyph without a second code path. */
    glyph_fill(c, x + spk_px(0), y + spk_px(5), spk_px(3) < 1 ? 1 : spk_px(3),
               spk_px(5) < 1 ? 1 : spk_px(5), col, alpha);
    for (int i = 0; i < 6; i++) {
        const int hh = 2 + i * 2;
        glyph_fill(c, x + spk_px(2 + i), y + spk_px(7 - hh / 2), spk_px(1) < 1 ? 1 : spk_px(1),
                   spk_px(hh) < 1 ? 1 : spk_px(hh), col, alpha);
    }

    static const int8_t arc1[7][2] = {{3, -3}, {4, -2}, {4, -1}, {4, 0}, {4, 1}, {4, 2}, {3, 3}};
    static const int8_t arc2[11][2] = {{6, -5}, {7, -4}, {7, -3}, {7, -2}, {7, -1}, {7, 0},
                                       {7, 1},  {7, 2},  {7, 3},  {7, 4},  {6, 5}};
    const int cx = x + spk_px(7);
    const int cy = y + spk_px(7);
    for (size_t i = 0; i < sizeof arc1 / sizeof arc1[0]; i++) {
        glyph_fill(c, cx + spk_px(arc1[i][0]), cy + spk_px(arc1[i][1]), 1, 1, col, alpha);
    }
    if (high) {
        for (size_t i = 0; i < sizeof arc2 / sizeof arc2[0]; i++) {
            glyph_fill(c, cx + spk_px(arc2[i][0]), cy + spk_px(arc2[i][1]), 1, 1, col, alpha);
        }
    }
}

void ui_draw_glyph_speaker(gfx_canvas_t *c, int x, int y, bool high, gfx_color_t col) {
    ui_draw_glyph_speaker_alpha(c, x, y, high, col, 255u);
}

void ui_draw_glyph_chevron(gfx_canvas_t *c, int x, int y, gfx_color_t col) {
    if (!ui_visible(c, x, y, UI_CHEVRON_W, UI_CHEVRON_H)) {
        return;
    }
    for (int r = 0; r < UI_CHEVRON_H; r++) {
        const int dx = (r < UI_CHEVRON_H / 2) ? r : UI_CHEVRON_H - 1 - r;
        gfx_fill_rect(c, x + dx, y + r, UI_CHEVRON_STROKE, 1, col);
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
        const int kr = (h >= 8) ? 7 : 5;
        const int kw = (h >= 8) ? 8 : 6;
        const int kx = x + (int)fw;
        const int ky = y + h / 2;
        gfx_fill_triangle(c, kx, ky - kr, kx, ky + kr, kx + kw, ky, UI_COL_TEXT);
        gfx_fill_triangle(c, kx, ky - kr, kx - kw, ky, kx, ky + kr, UI_COL_TEXT);
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
    gfx_fill_round_rect(c, x, y, UI_SCROLLBAR_W, h, UI_SCROLLBAR_W / 2, UI_COL_SCROLL_BG);

    int thumb = (int)(((int64_t)h * (int64_t)rows) / (int64_t)total_rows);
    if (thumb < UI_SCROLL_THUMB_MIN) {
        thumb = UI_SCROLL_THUMB_MIN;
    }
    if (thumb > h) {
        thumb = h;
    }
    const int max_first = (int)(total_rows - rows);
    int ty = y;
    if (max_first > 0) {
        ty += (int)(((int64_t)(h - thumb) * (int64_t)first_row) / (int64_t)max_first);
    }
    gfx_fill_round_rect(c, x, ty, UI_SCROLLBAR_W, thumb, UI_SCROLLBAR_W / 2, UI_COL_SCROLL_TH);
}

/** Baseline that centres the font's ascent..descent box inside the status bar. */
static int status_title_baseline(void) {
    const int asc = (int)UI_FONT_STATUS->ascent;
    int desc = (int)UI_FONT_STATUS->line_height - asc;
    if (desc < 0) {
        desc = 0;
    }
    const int box = asc + desc;
    int top = (UI_STATUS_H - box) / 2;
    if (top < 0) {
        top = 0;
    }
    return top + asc; /* 15 px at 320x240, 21 px at 480x320 */
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
        ui_draw_glyph_play(c, UI_STATUS_PLAY_X, UI_STATUS_PLAY_Y, st->play == UI_PLAY_PLAYING,
                           UI_COL_TEXT);
    }
    if (st->perf) {
        char tmp[20];
        (void)snprintf(tmp, sizeof tmp, "%u fps %u%%", (unsigned)st->fps, (unsigned)st->cpu);
        ui_text(c, UI_FONT_SMALL, UI_STATUS_PERF_X, UI_STATUS_PERF_BASE, UI_STATUS_PERF_MAX_W, tmp,
                UI_COL_TEXT_2);
    }

    ui_text_center(c, UI_FONT_STATUS, UI_SCREEN_W / 2, status_title_baseline(),
                   UI_STATUS_TITLE_MAX_W, st->title, UI_COL_TEXT);

    if (st->shuffle) {
        ui_draw_glyph_shuffle(c, UI_STATUS_SHUFFLE_X, UI_STATUS_MODE_Y, UI_COL_TEXT);
    }
    if (st->repeat) {
        ui_draw_glyph_repeat(c, UI_STATUS_REPEAT_X, UI_STATUS_MODE_Y, UI_COL_TEXT);
    }
    ui_draw_glyph_battery(c, UI_STATUS_BATT_X, UI_STATUS_BATT_Y, st->battery, st->charging,
                          UI_COL_TEXT);
}
