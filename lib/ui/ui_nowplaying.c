/**
 * @file ui_nowplaying.c
 * @brief Now Playing screen: artwork, marquee title, progress, volume, scrub.
 *
 * The screen owns no dynamic memory: the album art comes from the library
 * provider (scaled to 120x120 with nearest-neighbour sampling and given a soft
 * mirrored reflection), the title marquees when it is wider than its column,
 * and the progress bar is redrawn only when the displayed pixel or second
 * changes. The volume overlay replaces the bar for 1.5 s after a wheel tick and
 * fades back over 150 ms; scrub mode seeks with a diamond knob and a 10 Hz
 * command debounce.
 */

#include "ui_internal.h"

#include <stdio.h>
#include <string.h>

#include "fonts.h"
#include "ui_anim.h"
#include "ui_widgets.h"

/* Layout (docs/ui_spec.md). */
#define ART_X 14
#define ART_Y 40
#define ART_SIZE 120
#define REFL_Y 162
#define REFL_H 24
#define BAR_X 14
#define BAR_Y 196
#define BAR_W 292
#define BAR_H 6
#define LABEL_BASE 220

/* One rect for the whole progress block: bar, elapsed and remaining labels.
 * The volume overlay replaces exactly this block, so both must invalidate and
 * repaint the same pixels; a narrower overlay rect leaves the labels half
 * erased (top half cleared, bottom half stale). y 186..228 per the spec. */
#define PROGRESS_X 8
#define PROGRESS_Y 186
#define PROGRESS_W 304
#define PROGRESS_H 42

#define VOL_TIMEOUT_US 1500000u
#define VOL_FADE_S 0.15f
#define SCRUB_IDLE_US 2000000u
#define SCRUB_DEBOUNCE_US 100000u
#define SCRUB_FAST_MS 10000
#define SCRUB_SLOW_MS 2000

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

static struct {
    bool synced;            /**< Track bookkeeping holds a valid track_seq. */
    uint32_t track_seq;     /**< Last track sequence seen. */

    /* Album art cache (the provider's pointer is stable per album). */
    bool art_valid;
    uint32_t art_album;
    const gfx_image_t *art;

    /* Marquee. */
    int mq_width;           /**< Sanitised title width in px, 0 = not moving. */
    float mq_offset;
    float mq_t;
    uint8_t mq_phase;       /**< 0 pause, 1 scroll, 2 pause at the end. */
    int mq_drawn;           /**< Offset last drawn. */

    /* Progress invalidation bookkeeping. */
    int32_t drawn_px;
    int32_t drawn_second;
    uint32_t drawn_dur;

    /* Volume overlay. */
    bool vol_active;
    bool vol_fading;
    float vol_fade;
    uint32_t vol_last_us;
    uint8_t vol_value;
    uint8_t vol_seen;

    /* Scrub. */
    bool scrubbing;
    uint32_t scrub_pos;
    uint32_t scrub_last_us;
    uint32_t scrub_tick_us;
    uint32_t scrub_sent_us;
    uint32_t scrub_sent;
    bool scrub_sent_valid;
} s_np;

/* ------------------------------------------------------------------------- */
/* Rectangles and small helpers                                              */
/* ------------------------------------------------------------------------- */

static gfx_rect_t title_rect(void) {
    gfx_rect_t r = {UI_TITLE_COL_X, 62, UI_TITLE_COL_W, 30};
    return r;
}

static gfx_rect_t progress_rect(void) {
    gfx_rect_t r = {PROGRESS_X, PROGRESS_Y, PROGRESS_W, PROGRESS_H};
    return r;
}

static gfx_rect_t art_rect(void) {
    gfx_rect_t r = {ART_X - 2, ART_Y - 2, ART_SIZE + 4, ART_SIZE + 4};
    return r;
}

static gfx_rect_t reflection_rect(void) {
    gfx_rect_t r = {ART_X, REFL_Y, ART_SIZE, REFL_H};
    return r;
}

static int progress_px(uint32_t pos_ms, uint32_t dur_ms) {
    if (dur_ms == 0u) {
        return 0;
    }
    if (pos_ms > dur_ms) {
        pos_ms = dur_ms;
    }
    return (int)(((uint64_t)BAR_W * (uint64_t)pos_ms) / (uint64_t)dur_ms);
}

static void scrub_exit(void) {
    if (!s_np.scrubbing) {
        return;
    }
    s_np.scrubbing = false;
    if (!s_np.scrub_sent_valid || s_np.scrub_sent != s_np.scrub_pos) {
        ui_shared_post_simple(UI_CMD_SEEK, (int32_t)s_np.scrub_pos);
        s_np.scrub_sent = s_np.scrub_pos;
        s_np.scrub_sent_valid = true;
    }
}

/* ------------------------------------------------------------------------- */
/* Artwork                                                                   */
/* ------------------------------------------------------------------------- */

static const gfx_image_t *album_art(const ui_np_t *np) {
    if (!np->has_track) {
        return NULL;
    }
    if (s_np.art_valid && s_np.art_album == np->track.album_id) {
        return s_np.art;
    }
    const ui_library_t *lib = ui_library();
    s_np.art = NULL;
    if (lib != NULL && lib->get_album_art != NULL) {
        s_np.art = lib->get_album_art(np->track.album_id, lib->ctx);
    }
    s_np.art_album = np->track.album_id;
    s_np.art_valid = true;
    return s_np.art;
}

static void draw_reflection(gfx_canvas_t *c, const gfx_image_t *img, int ax) {
    if (!ui_visible(c, ax, REFL_Y, ART_SIZE, REFL_H)) {
        return;
    }
    const gfx_rect_t band = ui_band(c);
    const int ys = (REFL_Y > (int)band.y) ? REFL_Y : (int)band.y;
    const int y_end = REFL_Y + REFL_H;
    const int ye = (y_end < (int)band.y + (int)band.h) ? y_end : (int)band.y + (int)band.h;
    const int xs = (ax > (int)band.x) ? ax : (int)band.x;
    const int x_end = ax + ART_SIZE;
    const int xe = (x_end < (int)band.x + (int)band.w) ? x_end : (int)band.x + (int)band.w;
    if (ys >= ye || xs >= xe) {
        return;
    }

    for (int y = ys; y < ye; y++) {
        const int row = y - REFL_Y;
        const int src_row = ART_SIZE - 1 - row;
        const uint32_t sy = ((uint32_t)src_row * img->height) / (uint32_t)ART_SIZE;
        const uint8_t alpha = (uint8_t)(77u - (77u * (uint32_t)row) / (uint32_t)REFL_H);
        const uint16_t *srow = img->pixels + (size_t)sy * img->width;
        uint16_t *dst =
            c->buf + (size_t)(y - c->area.y) * (size_t)c->area.w + (size_t)(xs - c->area.x);
        for (int x = xs; x < xe; x++) {
            const uint32_t sx = ((uint32_t)(x - ax) * img->width) / (uint32_t)ART_SIZE;
            dst[x - xs] = gfx_blend(srow[sx], UI_COL_BG, alpha);
        }
    }
}

static void draw_art(gfx_canvas_t *c, const ui_np_t *np, int x_off) {
    const int ax = ART_X + x_off;
    if (ui_visible(c, ax - 2, ART_Y - 2, ART_SIZE + 4, ART_SIZE + 4)) {
        gfx_draw_round_rect(c, ax - 1, ART_Y - 1, ART_SIZE + 2, ART_SIZE + 2, 3, UI_COL_FRAME);
    }
    const gfx_image_t *img = album_art(np);
    if (img != NULL && img->pixels != NULL && img->width != 0u && img->height != 0u) {
        if (ui_visible(c, ax, ART_Y, ART_SIZE, ART_SIZE)) {
            if (img->width == ART_SIZE && img->height == ART_SIZE) {
                gfx_draw_image(c, ax, ART_Y, img);
            } else {
                gfx_draw_image_scaled(c, ax, ART_Y, ART_SIZE, ART_SIZE, img);
            }
        }
        draw_reflection(c, img, ax);
        return;
    }

    /* Placeholder: gradient square with a music note from circles and rects. */
    if (ui_visible(c, ax, ART_Y, ART_SIZE, ART_SIZE)) {
        gfx_fill_gradient_v(c, ax, ART_Y, ART_SIZE, ART_SIZE, GFX_RGB(96, 132, 208),
                            GFX_RGB(38, 54, 110));
        gfx_fill_circle(c, ax + 52, 136, 9, UI_COL_SEL_TEXT);
        gfx_fill_rect(c, ax + 59, 68, 3, 68, UI_COL_SEL_TEXT);
        gfx_fill_triangle(c, ax + 62, 68, ax + 62, 86, ax + 78, 78, UI_COL_SEL_TEXT);
    }
}

/* ------------------------------------------------------------------------- */
/* Volume overlay                                                            */
/* ------------------------------------------------------------------------- */

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

static void draw_volume(gfx_canvas_t *c, int x_off, uint8_t alpha) {
    const gfx_rect_t r = progress_rect();
    if (!ui_visible(c, r.x + x_off, r.y, r.w, r.h)) {
        return;
    }
    gfx_fill_rect_alpha(c, r.x + x_off, r.y, r.w, r.h, UI_COL_BG, alpha);

    ui_draw_glyph_speaker_alpha(c, 20 + x_off, 194, false, UI_COL_TEXT, alpha);
    ui_draw_glyph_speaker_alpha(c, 292 + x_off, 194, true, UI_COL_TEXT, alpha);

    const int x = 48 + x_off;
    const int y = BAR_Y;
    const int w = 242;
    int fill = (int)(((uint32_t)w * (uint32_t)s_np.vol_value) / 100u);
    if (fill < 0) {
        fill = 0;
    }
    if (fill > w) {
        fill = w;
    }
    if (alpha >= 255u) {
        gfx_fill_round_rect(c, x, y, w, BAR_H, BAR_H / 2, UI_COL_TRACK);
        if (fill > 0) {
            gfx_fill_round_rect(c, x, y, fill, BAR_H, BAR_H / 2, UI_COL_SEL_BOT);
        }
    } else {
        glyph_fill(c, x, y, w, BAR_H, UI_COL_TRACK, alpha);
        if (fill > 0) {
            glyph_fill(c, x, y, fill, BAR_H, UI_COL_SEL_BOT, alpha);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Public (internal) API                                                     */
/* ------------------------------------------------------------------------- */

void ui_nowplaying_reset(void) {
    s_np.synced = false;
    s_np.track_seq = 0;
    s_np.art_valid = false;
    s_np.art_album = 0;
    s_np.art = NULL;
    s_np.mq_width = 0;
    s_np.mq_offset = 0.0f;
    s_np.mq_t = 0.0f;
    s_np.mq_phase = 0;
    s_np.mq_drawn = -1;
    s_np.drawn_px = -1;
    s_np.drawn_second = -1;
    s_np.drawn_dur = 0;
    s_np.vol_active = false;
    s_np.vol_fading = false;
    s_np.vol_fade = 0.0f;
    s_np.vol_value = 0;
    s_np.vol_seen = 0;
    s_np.scrubbing = false;
}

void ui_nowplaying_wheel(const ui_np_t *np, int ticks, uint32_t now_us) {
    if (np == NULL || ticks == 0 || !np->has_track) {
        return;
    }

    if (s_np.scrubbing) {
        const uint32_t since_tick = now_us - s_np.scrub_tick_us;
        const int step = (since_tick < 60000u) ? SCRUB_FAST_MS : SCRUB_SLOW_MS;
        s_np.scrub_tick_us = now_us;
        s_np.scrub_last_us = now_us;

        int32_t pos = (int32_t)s_np.scrub_pos + ticks * step;
        if (pos < 0) {
            pos = 0;
        }
        if ((uint32_t)pos > np->track.duration_ms) {
            pos = (int32_t)np->track.duration_ms;
        }
        s_np.scrub_pos = (uint32_t)pos;

        if ((uint32_t)(now_us - s_np.scrub_sent_us) >= SCRUB_DEBOUNCE_US) {
            ui_shared_post_simple(UI_CMD_SEEK, pos);
            s_np.scrub_sent = (uint32_t)pos;
            s_np.scrub_sent_valid = true;
            s_np.scrub_sent_us = now_us;
        }
        return;
    }

    int vol = (int)s_np.vol_value + ticks * 2;
    if (vol < 0) {
        vol = 0;
    }
    if (vol > 100) {
        vol = 100;
    }
    s_np.vol_value = (uint8_t)vol;
    s_np.vol_active = true;
    s_np.vol_fading = false;
    s_np.vol_fade = 1.0f;
    s_np.vol_last_us = now_us;
    ui_invalidate(progress_rect());
    ui_shared_post_simple(UI_CMD_SET_VOLUME, vol);
}

void ui_nowplaying_click(const ui_np_t *np, uint32_t now_us) {
    if (np == NULL || !np->has_track) {
        return;
    }
    if (s_np.scrubbing) {
        scrub_exit();
        ui_invalidate(progress_rect());
        return;
    }
    s_np.scrubbing = true;
    s_np.scrub_pos = np->position_ms;
    if (s_np.scrub_pos > np->track.duration_ms) {
        s_np.scrub_pos = np->track.duration_ms;
    }
    s_np.scrub_last_us = now_us;
    s_np.scrub_tick_us = now_us - 1000000u;
    s_np.scrub_sent_valid = false;
    s_np.scrub_sent_us = 0;
    ui_invalidate(progress_rect());
}

void ui_nowplaying_step(const ui_np_t *np, ui_screen_t *s, float dt_s, uint32_t now_us) {
    (void)s;
    if (np == NULL || np->seq == 0u) {
        return;
    }

    /* New track: reset the transient display state. */
    if (!s_np.synced || s_np.track_seq != np->track_seq) {
        const bool had = s_np.synced;
        s_np.synced = true;
        s_np.track_seq = np->track_seq;
        s_np.art_valid = false;
        s_np.mq_width = 0;
        s_np.mq_offset = 0.0f;
        s_np.mq_t = 0.0f;
        s_np.mq_phase = 0;
        s_np.mq_drawn = -1;
        s_np.drawn_px = -1;
        s_np.drawn_second = -1;
        s_np.drawn_dur = 0;
        s_np.vol_value = np->volume;
        s_np.vol_seen = np->volume;
        s_np.scrubbing = false;
        ui_invalidate(title_rect());
        ui_invalidate(art_rect());
        ui_invalidate(reflection_rect());
        ui_invalidate(progress_rect());
        if (had) {
            ui_invalidate(progress_rect());
        }
    }

    if (!np->has_track) {
        return;
    }

    /* The overlay value follows the audio side whenever it is not on screen. */
    if (np->volume != s_np.vol_seen) {
        s_np.vol_seen = np->volume;
    }
    if (!s_np.vol_active) {
        s_np.vol_value = np->volume;
    }

    /* Marquee: 30 px/s with a 1.5 s pause at each end. */
    char title[UI_TITLE_MAX];
    ui_ascii(np->track.title, title, sizeof title);
    const int width = gfx_text_width(&font_inter_20b, title);
    if (width <= UI_TITLE_COL_W) {
        if (s_np.mq_offset != 0.0f || s_np.mq_width != 0) {
            s_np.mq_offset = 0.0f;
            s_np.mq_width = 0;
            s_np.mq_t = 0.0f;
            s_np.mq_phase = 0;
            s_np.mq_drawn = -1;
            ui_invalidate(title_rect());
        }
    } else {
        if (s_np.mq_width != width) {
            s_np.mq_width = width;
            s_np.mq_offset = 0.0f;
            s_np.mq_t = 0.0f;
            s_np.mq_phase = 0;
            s_np.mq_drawn = -1;
        }
        const float max_off = (float)(width - UI_TITLE_COL_W);
        switch (s_np.mq_phase) {
        case 0:
            s_np.mq_t += dt_s;
            if (s_np.mq_t >= 1.5f) {
                s_np.mq_phase = 1;
                s_np.mq_t = 0.0f;
            }
            break;
        case 1:
            s_np.mq_offset += 30.0f * dt_s;
            if (s_np.mq_offset >= max_off) {
                s_np.mq_offset = max_off;
                s_np.mq_phase = 2;
                s_np.mq_t = 0.0f;
            }
            break;
        default:
            s_np.mq_t += dt_s;
            if (s_np.mq_t >= 1.5f) {
                s_np.mq_phase = 0;
                s_np.mq_t = 0.0f;
                s_np.mq_offset = 0.0f;
            }
            break;
        }
        const int off = (int)(s_np.mq_offset + 0.5f);
        if (off != s_np.mq_drawn) {
            s_np.mq_drawn = off;
            ui_invalidate(title_rect());
        }
    }

    /* Progress bar: only when the pixel or the displayed second changes. */
    if (s_np.scrubbing && (uint32_t)(now_us - s_np.scrub_last_us) >= SCRUB_IDLE_US) {
        scrub_exit();
        ui_invalidate(progress_rect());
    }
    const uint32_t pos = s_np.scrubbing ? s_np.scrub_pos : np->position_ms;
    const uint32_t dur = np->track.duration_ms;
    const int px = progress_px(pos, dur);
    const int32_t sec = (int32_t)(pos / 1000u);
    if (px != s_np.drawn_px || sec != s_np.drawn_second || dur != s_np.drawn_dur) {
        s_np.drawn_px = px;
        s_np.drawn_second = sec;
        s_np.drawn_dur = dur;
        ui_invalidate(progress_rect());
    }

    /* Volume overlay timeout and 150 ms fade. */
    if (s_np.vol_active) {
        if (!s_np.vol_fading) {
            if ((uint32_t)(now_us - s_np.vol_last_us) >= VOL_TIMEOUT_US) {
                s_np.vol_fading = true;
                s_np.vol_fade = 1.0f;
            }
        } else {
            s_np.vol_fade -= dt_s / VOL_FADE_S;
            if (s_np.vol_fade <= 0.0f) {
                s_np.vol_fade = 0.0f;
                s_np.vol_active = false;
                s_np.vol_fading = false;
            }
            ui_invalidate(progress_rect());
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Drawing                                                                   */
/* ------------------------------------------------------------------------- */

void ui_nowplaying_draw(gfx_canvas_t *c, const ui_np_t *np, const ui_screen_t *s, int x_off) {
    (void)s;
    if (c == NULL || np == NULL) {
        return;
    }
    if (!ui_visible(c, x_off, UI_CONTENT_Y, UI_SCREEN_W, UI_CONTENT_H)) {
        return;
    }
    gfx_fill_rect(c, x_off, UI_CONTENT_Y, UI_SCREEN_W, UI_CONTENT_H, UI_COL_BG);

    if (!np->has_track) {
        ui_text_center(c, &font_inter_20b, 160 + x_off, 132, 0, "Nothing playing",
                       UI_COL_TEXT_2);
        return;
    }

    draw_art(c, np, x_off);

    if (np->track.queue_length > 0u) {
        char idx[24];
        (void)snprintf(idx, sizeof idx, "%u of %u", (unsigned)np->track.queue_index,
                       (unsigned)np->track.queue_length);
        ui_text(c, &font_inter_12, UI_TITLE_COL_X + x_off, 52, UI_TITLE_COL_W, idx,
                UI_COL_TEXT_2);
    }

    char title[UI_TITLE_MAX];
    ui_ascii(np->track.title, title, sizeof title);
    if (s_np.mq_width > UI_TITLE_COL_W) {
        const gfx_rect_t saved = c->clip;
        gfx_rect_t tr = title_rect();
        tr.x = (int16_t)(tr.x + x_off);
        gfx_set_clip(c, gfx_rect_intersect(saved, tr));
        (void)gfx_draw_text(c, &font_inter_20b, UI_TITLE_COL_X + x_off - s_np.mq_drawn, 84, title,
                            UI_COL_TEXT);
        gfx_set_clip(c, saved);
    } else {
        ui_text(c, &font_inter_20b, UI_TITLE_COL_X + x_off, 84, UI_TITLE_COL_W, title,
                UI_COL_TEXT);
    }

    ui_text(c, &font_inter_14, UI_TITLE_COL_X + x_off, 108, UI_TITLE_COL_W, np->track.artist,
            UI_COL_TEXT);
    ui_text(c, &font_inter_14, UI_TITLE_COL_X + x_off, 128, UI_TITLE_COL_W, np->track.album,
            UI_COL_TEXT_2);

    const uint32_t pos = s_np.scrubbing ? s_np.scrub_pos : np->position_ms;
    const uint32_t dur = np->track.duration_ms;
    const bool overlay = s_np.vol_active && s_np.vol_fade > 0.0f;
    const gfx_rect_t block = progress_rect();
    if (ui_visible(c, block.x + x_off, block.y, block.w, block.h)) {
        if (!overlay) {
            /* Overlay hidden: the whole progress block, labels included. */
            ui_draw_progress(c, BAR_X + x_off, BAR_Y, BAR_W, BAR_H, pos, dur, s_np.scrubbing);

            char buf[16];
            ui_format_time(buf, sizeof buf, pos, false);
            ui_text(c, &font_inter_12, BAR_X + x_off, LABEL_BASE, 90, buf, UI_COL_TEXT_2);
            ui_format_time(buf, sizeof buf, (dur > pos) ? (dur - pos) : 0u, true);
            ui_text_right(c, &font_inter_12, BAR_X + BAR_W + x_off, LABEL_BASE, 90, buf,
                          UI_COL_TEXT_2);
        } else {
            /* Overlay visible: it replaces the block (bar and both time
             * labels) inside the one shared rect; labels are not drawn at
             * all, so no half-erased glyphs can survive at any alpha. */
            uint8_t alpha = 255u;
            if (s_np.vol_fading) {
                alpha = (uint8_t)(s_np.vol_fade * 255.0f + 0.5f);
            }
            if (alpha > 0u) {
                draw_volume(c, x_off, alpha);
            }
        }
    }
}
