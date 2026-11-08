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

/* Layout comes from ui_layout.h: UI_ART_*, UI_BAR_*, UI_PROGRESS_*, UI_NP_*.
 * The volume overlay replaces exactly the progress block; both that overlay and
 * the normal bar/labels invalidate and repaint the same rectangle, otherwise a
 * narrower overlay rect leaves the labels half erased. */

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
    gfx_rect_t r = {UI_NP_TITLE_COL_X, UI_NP_TITLE_RECT_Y, UI_NP_TITLE_COL_W,
                    UI_NP_TITLE_RECT_H};
    return r;
}

/** Queue label + title + artist + album: everything that changes with a track.
 *  The art, reflection and progress bar have their own rects. */
static gfx_rect_t track_text_rect(void) {
    gfx_rect_t r = {UI_NP_TITLE_COL_X, UI_NP_TEXT_RECT_Y, UI_NP_TITLE_COL_W, UI_NP_TEXT_RECT_H};
    return r;
}

static gfx_rect_t progress_rect(void) {
    gfx_rect_t r = {UI_PROGRESS_X, UI_PROGRESS_Y, UI_PROGRESS_W, UI_PROGRESS_H};
    return r;
}

/* The album art itself (inside the frame). This is the rectangle a precomputed
 * flash image can be sent from: the 1 px frame ring is static and lies just
 * outside it, so it never needs repainting when the image changes. */
static gfx_rect_t art_image_rect(void) {
    gfx_rect_t r = {UI_ART_X, UI_ART_Y, UI_ART_SIZE, UI_ART_SIZE};
    return r;
}

static gfx_rect_t reflection_rect(void) {
    gfx_rect_t r = {UI_ART_X, UI_ART_REFL_Y, UI_ART_SIZE, UI_ART_REFL_H};
    return r;
}

static int progress_px(uint32_t pos_ms, uint32_t dur_ms) {
    if (dur_ms == 0u) {
        return 0;
    }
    if (pos_ms > dur_ms) {
        pos_ms = dur_ms;
    }
    return (int)(((uint64_t)UI_BAR_W * (uint64_t)pos_ms) / (uint64_t)dur_ms);
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

/** Scale a 120x120 art-relative coordinate to the active art size. */
static int art_px(int v) {
    return (v * UI_ART_SCALE_NUM) / UI_ART_SCALE_DEN;
}

static void draw_reflection(gfx_canvas_t *c, const gfx_image_t *img, int ax) {
    if (!ui_visible(c, ax, UI_ART_REFL_Y, UI_ART_SIZE, UI_ART_REFL_H)) {
        return;
    }
    const gfx_rect_t band = ui_band(c);
    const int ys = (UI_ART_REFL_Y > (int)band.y) ? UI_ART_REFL_Y : (int)band.y;
    const int y_end = UI_ART_REFL_Y + UI_ART_REFL_H;
    const int ye = (y_end < (int)band.y + (int)band.h) ? y_end : (int)band.y + (int)band.h;
    const int xs = (ax > (int)band.x) ? ax : (int)band.x;
    const int x_end = ax + UI_ART_SIZE;
    const int xe = (x_end < (int)band.x + (int)band.w) ? x_end : (int)band.x + (int)band.w;
    if (ys >= ye || xs >= xe) {
        return;
    }

    for (int y = ys; y < ye; y++) {
        const int row = y - UI_ART_REFL_Y;
        const int src_row = UI_ART_SIZE - 1 - row;
        const uint32_t sy = ((uint32_t)src_row * img->height) / (uint32_t)UI_ART_SIZE;
        const uint8_t alpha = (uint8_t)(77u - (77u * (uint32_t)row) / (uint32_t)UI_ART_REFL_H);
        const uint16_t *srow = img->pixels + (size_t)sy * img->width;
        uint16_t *dst =
            c->buf + (size_t)(y - c->area.y) * (size_t)c->area.w + (size_t)(xs - c->area.x);
        for (int x = xs; x < xe; x++) {
            const uint32_t sx = ((uint32_t)(x - ax) * img->width) / (uint32_t)UI_ART_SIZE;
            dst[x - xs] = gfx_blend(srow[sx], UI_COL_BG, alpha);
        }
    }
}

static void draw_art(gfx_canvas_t *c, const ui_np_t *np, int x_off) {
    const int ax = UI_ART_X + x_off;
    if (ui_visible(c, ax - 2, UI_ART_Y - 2, UI_ART_SIZE + 4, UI_ART_SIZE + 4)) {
        gfx_draw_round_rect(c, ax - 1, UI_ART_Y - 1, UI_ART_SIZE + 2, UI_ART_SIZE + 2,
                            UI_ART_FRAME_R, UI_COL_FRAME);
    }
    const gfx_image_t *img = album_art(np);
    if (img != NULL && img->pixels != NULL && img->width != 0u && img->height != 0u) {
        if (ui_visible(c, ax, UI_ART_Y, UI_ART_SIZE, UI_ART_SIZE)) {
            if (img->width == UI_ART_SIZE && img->height == UI_ART_SIZE) {
                gfx_draw_image(c, ax, UI_ART_Y, img);
            } else {
                gfx_draw_image_scaled(c, ax, UI_ART_Y, UI_ART_SIZE, UI_ART_SIZE, img);
            }
        }
        draw_reflection(c, img, ax);
        return;
    }

    /* Placeholder: gradient square with a music note from circles and rects.
     * The note is authored in 120x120 art-relative coordinates and scaled to
     * the active art size (1x at 320x240, 1.5x at 480x320). */
    if (ui_visible(c, ax, UI_ART_Y, UI_ART_SIZE, UI_ART_SIZE)) {
        gfx_fill_gradient_v(c, ax, UI_ART_Y, UI_ART_SIZE, UI_ART_SIZE, GFX_RGB(96, 132, 208),
                            GFX_RGB(38, 54, 110));
        gfx_fill_circle(c, ax + art_px(UI_ART_NOTE_CX), UI_ART_Y + art_px(UI_ART_NOTE_CY),
                        art_px(UI_ART_NOTE_R), UI_COL_SEL_TEXT);
        gfx_fill_rect(c, ax + art_px(UI_ART_NOTE_STEM_X), UI_ART_Y + art_px(UI_ART_NOTE_STEM_Y),
                      art_px(UI_ART_NOTE_STEM_W), art_px(UI_ART_NOTE_STEM_H), UI_COL_SEL_TEXT);
        gfx_fill_triangle(c, ax + art_px(62), UI_ART_Y + art_px(28), ax + art_px(62),
                          UI_ART_Y + art_px(46), ax + art_px(78), UI_ART_Y + art_px(38),
                          UI_COL_SEL_TEXT);
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

    ui_draw_glyph_speaker_alpha(c, UI_VOL_SPK_LOW_X + x_off, UI_VOL_SPK_Y, false, UI_COL_TEXT,
                                alpha);
    ui_draw_glyph_speaker_alpha(c, UI_VOL_SPK_HIGH_X + x_off, UI_VOL_SPK_Y, true, UI_COL_TEXT,
                                alpha);

    const int x = UI_VOL_BAR_X + x_off;
    const int y = UI_BAR_Y;
    const int w = UI_VOL_BAR_W;
    int fill = (int)(((uint32_t)w * (uint32_t)s_np.vol_value) / 100u);
    if (fill < 0) {
        fill = 0;
    }
    if (fill > w) {
        fill = w;
    }
    if (alpha >= 255u) {
        gfx_fill_round_rect(c, x, y, w, UI_BAR_H, UI_BAR_H / 2, UI_COL_TRACK);
        if (fill > 0) {
            gfx_fill_round_rect(c, x, y, fill, UI_BAR_H, UI_BAR_H / 2, UI_COL_SEL_BOT);
        }
    } else {
        glyph_fill(c, x, y, w, UI_BAR_H, UI_COL_TRACK, alpha);
        if (fill > 0) {
            glyph_fill(c, x, y, fill, UI_BAR_H, UI_COL_SEL_BOT, alpha);
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

void ui_nowplaying_direct_layers(const ui_np_t *np, const ui_screen_t *s) {
    (void)s;
    const gfx_image_t *img = album_art(np);
    /* Only a native 120x120 image maps 1:1 onto the art rectangle; anything
     * else is scaled by gfx_draw_image_scaled() and stays on the strip path. */
    if (img == NULL || img->pixels == NULL || img->width != UI_ART_SIZE || img->height != UI_ART_SIZE) {
        return;
    }
    gfx_rect_t r = art_image_rect();
    (void)gfx_direct_add(r, img->pixels, img->width);
}

void ui_nowplaying_invalidate_view(const ui_np_t *np, const ui_screen_t *s) {
    (void)s;
    gfx_rect_t content = {0, UI_CONTENT_Y, UI_SCREEN_W, UI_CONTENT_H};
    ui_invalidate(content);

    /* The art rectangle is added exactly and last: the content rect above
     * repaints the same pixels through the strip path, so the image can then be
     * sent straight from flash without a stale stripe. */
    const gfx_image_t *img = album_art(np);
    if (img != NULL && img->pixels != NULL && img->width == UI_ART_SIZE && img->height == UI_ART_SIZE) {
        ui_invalidate_exact(art_image_rect());
    }
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
        ui_invalidate(track_text_rect());
        ui_invalidate(reflection_rect());
        ui_invalidate(progress_rect());
        if (had) {
            ui_invalidate(progress_rect());
        }
        /* Only the image changes; invalidate it exactly and last so it can go
         * straight from flash instead of through the strip buffers. The frame
         * ring around it is static. */
        ui_invalidate_exact(art_image_rect());
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
    const int width = gfx_text_width(UI_FONT_TITLE, title);
    if (width <= UI_NP_TITLE_COL_W) {
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
        const float max_off = (float)(width - UI_NP_TITLE_COL_W);
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
        ui_text_center(c, UI_FONT_TITLE, UI_SCREEN_W / 2 + x_off, UI_NP_EMPTY_BASE, 0,
                       "Nothing playing", UI_COL_TEXT_2);
        return;
    }

    draw_art(c, np, x_off);

    if (np->track.queue_length > 0u) {
        char idx[24];
        (void)snprintf(idx, sizeof idx, "%u of %u", (unsigned)np->track.queue_index,
                       (unsigned)np->track.queue_length);
        ui_text(c, UI_FONT_SMALL, UI_NP_TITLE_COL_X + x_off, UI_NP_QUEUE_BASE, UI_NP_TITLE_COL_W,
                idx, UI_COL_TEXT_2);
    }

    char title[UI_TITLE_MAX];
    ui_ascii(np->track.title, title, sizeof title);
    if (s_np.mq_width > UI_NP_TITLE_COL_W) {
        const gfx_rect_t saved = c->clip;
        gfx_rect_t tr = title_rect();
        tr.x = (int16_t)(tr.x + x_off);
        gfx_set_clip(c, gfx_rect_intersect(saved, tr));
        (void)gfx_draw_text(c, UI_FONT_TITLE, UI_NP_TITLE_COL_X + x_off - s_np.mq_drawn,
                            UI_NP_TITLE_BASE, title, UI_COL_TEXT);
        gfx_set_clip(c, saved);
    } else {
        ui_text(c, UI_FONT_TITLE, UI_NP_TITLE_COL_X + x_off, UI_NP_TITLE_BASE, UI_NP_TITLE_COL_W,
                title, UI_COL_TEXT);
    }

    ui_text(c, UI_FONT_BODY, UI_NP_TITLE_COL_X + x_off, UI_NP_ARTIST_BASE, UI_NP_TITLE_COL_W,
            np->track.artist, UI_COL_TEXT);
    ui_text(c, UI_FONT_BODY, UI_NP_TITLE_COL_X + x_off, UI_NP_ALBUM_BASE, UI_NP_TITLE_COL_W,
            np->track.album, UI_COL_TEXT_2);

    const uint32_t pos = s_np.scrubbing ? s_np.scrub_pos : np->position_ms;
    const uint32_t dur = np->track.duration_ms;
    const bool overlay = s_np.vol_active && s_np.vol_fade > 0.0f;
    const gfx_rect_t block = progress_rect();
    if (ui_visible(c, block.x + x_off, block.y, block.w, block.h)) {
        if (!overlay) {
            /* Overlay hidden: the whole progress block, labels included. */
            ui_draw_progress(c, UI_BAR_X + x_off, UI_BAR_Y, UI_BAR_W, UI_BAR_H, pos, dur,
                             s_np.scrubbing);

            char buf[16];
            ui_format_time(buf, sizeof buf, pos, false);
            ui_text(c, UI_FONT_SMALL, UI_BAR_X + x_off, UI_BAR_LABEL_BASE, 90, buf, UI_COL_TEXT_2);
            ui_format_time(buf, sizeof buf, (dur > pos) ? (dur - pos) : 0u, true);
            ui_text_right(c, UI_FONT_SMALL, UI_BAR_X + UI_BAR_W + x_off, UI_BAR_LABEL_BASE, 90, buf,
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
