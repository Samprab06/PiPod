/*
 * Dirty-rectangle manager and strip renderer.
 *
 * The renderer owns two strip buffers. Each band is rendered into the current
 * buffer and then handed to the display driver with ili9341_blit_async(), which
 * waits for the previous transfer and starts DMA. The renderer then flips to
 * the other buffer, so the next band is drawn while the previous one is still
 * on the wire.
 *
 * Because ili9341_blit_async() waits for the transfer that preceded it, a
 * buffer is only reused after the transfer that used it has completed: by the
 * time the renderer comes back to buffer N (two blits later) the blit of N was
 * completed by the blit of N + 1. The parity is kept across gfx_render() calls
 * so that the first band of a new frame never lands in the buffer that the
 * previous frame left in flight.
 */

#include "gfx_render.h"

#include <string.h>

#include "ili9341.h"

#ifdef PICO_ON_DEVICE
#include "pico/time.h"
#else
#include <time.h>
#endif

/* The strip buffers are sized for the widest landscape window. */
#define GFX_STRIP_MAX_W 320

#define GFX_STRIP_PIXELS ((size_t)GFX_STRIP_MAX_W * (size_t)GFX_STRIP_H)

static uint16_t s_buf[2][GFX_STRIP_PIXELS];
static uint8_t s_buf_index;
static gfx_rect_t s_dirty[GFX_MAX_DIRTY];
static uint8_t s_dirty_count;
static gfx_render_stats_t s_stats;

/* ------------------------------------------------------------------------- */
/* Timing                                                                    */
/* ------------------------------------------------------------------------- */

static uint32_t now_us(void) {
#ifdef PICO_ON_DEVICE
    return time_us_32();
#else
    /* Host builds only need a repeatable monotonic-ish timer for statistics. */
    return (uint32_t)(((uint64_t)clock() * 1000000ull) / (uint64_t)CLOCKS_PER_SEC);
#endif
}

/* ------------------------------------------------------------------------- */
/* Dirty rectangle list                                                      */
/* ------------------------------------------------------------------------- */

static gfx_rect_t screen_rect(void) {
    int w = (int)ili9341_width();
    int h = (int)ili9341_height();
    gfx_rect_t r;
    if (w > GFX_STRIP_MAX_W) {
        w = GFX_STRIP_MAX_W;
    }
    if (w < 0) {
        w = 0;
    }
    if (h < 0) {
        h = 0;
    }
    r.x = 0;
    r.y = 0;
    r.w = (int16_t)w;
    r.h = (int16_t)h;
    return r;
}

static uint32_t rect_area(gfx_rect_t r) {
    return (uint32_t)r.w * (uint32_t)r.h;
}

void gfx_render_init(void) {
    s_buf_index = 0;
    s_dirty_count = 0;
    memset(s_dirty, 0, sizeof(s_dirty));
    memset(&s_stats, 0, sizeof(s_stats));
}

void gfx_invalidate(gfx_rect_t r) {
    r = gfx_rect_intersect(r, screen_rect());
    if (gfx_rect_empty(r)) {
        return;
    }

    const uint32_t ra = rect_area(r);

    /* Merge into the existing rect whose union adds the least area, provided
     * the union stays within 1.3x of the two separate areas. */
    int best = -1;
    int64_t best_added = INT64_MAX;
    gfx_rect_t best_union = {0, 0, 0, 0};
    for (int i = 0; i < s_dirty_count; i++) {
        gfx_rect_t u = gfx_rect_union(s_dirty[i], r);
        uint32_t ua = rect_area(u);
        uint32_t sum = rect_area(s_dirty[i]) + ra;
        if ((uint64_t)ua * 10u <= (uint64_t)sum * 13u) {
            int64_t added = (int64_t)ua - (int64_t)rect_area(s_dirty[i]);
            if (added < best_added) {
                best = i;
                best_added = added;
                best_union = u;
            }
        }
    }
    if (best >= 0) {
        s_dirty[best] = best_union;
        return;
    }

    if (s_dirty_count < GFX_MAX_DIRTY) {
        s_dirty[s_dirty_count++] = r;
        return;
    }

    /* List full: merge the pair whose union adds the least area, then appending
     * the new rect keeps the count at GFX_MAX_DIRTY. */
    int mi = 0;
    int mj = 1;
    int64_t best_pair = INT64_MAX;
    gfx_rect_t pair_union = gfx_rect_union(s_dirty[0], s_dirty[1]);
    for (int i = 0; i < GFX_MAX_DIRTY; i++) {
        for (int j = i + 1; j < GFX_MAX_DIRTY; j++) {
            gfx_rect_t u = gfx_rect_union(s_dirty[i], s_dirty[j]);
            int64_t added = (int64_t)rect_area(u) - (int64_t)rect_area(s_dirty[i]) -
                            (int64_t)rect_area(s_dirty[j]);
            if (added < best_pair) {
                best_pair = added;
                mi = i;
                mj = j;
                pair_union = u;
            }
        }
    }
    s_dirty[mi] = pair_union;
    for (int k = mj; k + 1 < s_dirty_count; k++) {
        s_dirty[k] = s_dirty[k + 1];
    }
    s_dirty_count--;
    s_dirty[s_dirty_count++] = r;
}

void gfx_invalidate_all(void) {
    gfx_rect_t r = screen_rect();
    if (gfx_rect_empty(r)) {
        return;
    }
    s_dirty[0] = r;
    s_dirty_count = 1;
}

bool gfx_has_dirty(void) {
    return s_dirty_count > 0;
}

/* ------------------------------------------------------------------------- */
/* Rendering                                                                 */
/* ------------------------------------------------------------------------- */

void gfx_render(gfx_draw_fn draw, void *user) {
    if (draw == NULL || s_dirty_count == 0) {
        return;
    }

    const uint32_t t0 = now_us();
    const uint32_t rect_count = s_dirty_count;
    uint32_t bands = 0;
    uint32_t pixels = 0;

    for (uint32_t i = 0; i < rect_count; i++) {
        gfx_rect_t r = s_dirty[i];
        if (r.w > GFX_STRIP_MAX_W) {
            continue;
        }
        const int y_end = (int)r.y + (int)r.h;
        for (int band_y = r.y; band_y < y_end; band_y += GFX_STRIP_H) {
            int band_h = y_end - band_y;
            if (band_h > GFX_STRIP_H) {
                band_h = GFX_STRIP_H;
            }

            gfx_canvas_t c;
            c.buf = s_buf[s_buf_index];
            c.area.x = r.x;
            c.area.y = (int16_t)band_y;
            c.area.w = r.w;
            c.area.h = (int16_t)band_h;
            c.clip = c.area;

            draw(&c, user);

            /* blit_async() waits for the previous transfer (the other buffer)
             * before starting this one, so s_buf[s_buf_index ^ 1] is free. */
            ili9341_blit_async((uint16_t)r.x, (uint16_t)band_y, (uint16_t)r.w,
                               (uint16_t)band_h, c.buf);
            s_buf_index ^= 1u;

            bands++;
            pixels += (uint32_t)r.w * (uint32_t)band_h;
        }
    }

    s_dirty_count = 0;

    const uint32_t dt = now_us() - t0;
    s_stats.frames++;
    s_stats.rects += rect_count;
    s_stats.bands += bands;
    s_stats.pixels += pixels;
    s_stats.last_render_us = dt;
    if (dt > s_stats.max_render_us) {
        s_stats.max_render_us = dt;
    }
}

void gfx_render_get_stats(gfx_render_stats_t *s) {
    if (s == NULL) {
        return;
    }
    *s = s_stats;
}

void gfx_render_reset_stats(void) {
    memset(&s_stats, 0, sizeof(s_stats));
}
