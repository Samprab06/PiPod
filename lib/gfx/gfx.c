/*
 * Framebuffer-less RGB565 drawing primitives.
 *
 * Every primitive is cheap to reject: the target rectangle is intersected with
 * the canvas clip and area first, and only then are pixels touched. Inner
 * loops work on row pointers with the canvas stride (area.w), never on
 * per-pixel multiplications.
 */

#include "gfx.h"

#include <math.h>
#include <stddef.h>

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static inline uint32_t expand_r(gfx_color_t c) {
    uint32_t v = (c >> 11) & 0x1Fu;
    return (v << 3) | (v >> 2);
}

static inline uint32_t expand_g(gfx_color_t c) {
    uint32_t v = (c >> 5) & 0x3Fu;
    return (v << 2) | (v >> 4);
}

static inline uint32_t expand_b(gfx_color_t c) {
    uint32_t v = c & 0x1Fu;
    return (v << 3) | (v >> 2);
}

static inline gfx_color_t pack_rgb(uint32_t r, uint32_t g, uint32_t b) {
    return (gfx_color_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

static inline int imin(int a, int b) {
    return a < b ? a : b;
}

static inline int imax(int a, int b) {
    return a > b ? a : b;
}

/* Clip [x, x+w) x [y, y+h) to (clip intersect area). Uses 64-bit intermediate
 * arithmetic so wild caller coordinates cannot overflow. */
static gfx_rect_t clip_to_canvas(const gfx_canvas_t *c, int x, int y, int w, int h) {
    gfx_rect_t out = {0, 0, 0, 0};
    if (w <= 0 || h <= 0) {
        return out;
    }
    gfx_rect_t cl = gfx_rect_intersect(c->clip, c->area);
    int64_t x0 = x, y0 = y, x1 = (int64_t)x + w, y1 = (int64_t)y + h;
    int64_t cx0 = cl.x, cy0 = cl.y, cx1 = (int64_t)cl.x + cl.w, cy1 = (int64_t)cl.y + cl.h;
    if (x0 < cx0) {
        x0 = cx0;
    }
    if (y0 < cy0) {
        y0 = cy0;
    }
    if (x1 > cx1) {
        x1 = cx1;
    }
    if (y1 > cy1) {
        y1 = cy1;
    }
    if (x1 <= x0 || y1 <= y0) {
        return out;
    }
    out.x = (int16_t)x0;
    out.y = (int16_t)y0;
    out.w = (int16_t)(x1 - x0);
    out.h = (int16_t)(y1 - y0);
    return out;
}

/* Pointer to screen pixel (x, y). The caller guarantees the pixel is inside
 * the canvas area. */
static inline uint16_t *px_at(const gfx_canvas_t *c, int x, int y) {
    return c->buf + (size_t)(y - c->area.y) * (size_t)c->area.w + (size_t)(x - c->area.x);
}

static void fill_span(uint16_t *p, int n, gfx_color_t col) {
    for (int i = 0; i < n; i++) {
        p[i] = col;
    }
}

static void blend_span(uint16_t *p, int n, gfx_color_t col, uint8_t alpha) {
    if (alpha == 0u || n <= 0) {
        return;
    }
    if (alpha == 255u) {
        fill_span(p, n, col);
        return;
    }

    const uint32_t a = alpha;
    const uint32_t ia = 255u - a;
    const uint32_t fr = expand_r(col);
    const uint32_t fgn = expand_g(col);
    const uint32_t fb = expand_b(col);

    for (int i = 0; i < n; i++) {
        gfx_color_t bg = p[i];
        uint32_t r = (fr * a + expand_r(bg) * ia + 127u) / 255u;
        uint32_t g = (fgn * a + expand_g(bg) * ia + 127u) / 255u;
        uint32_t b = (fb * a + expand_b(bg) * ia + 127u) / 255u;
        p[i] = pack_rgb(r, g, b);
    }
}

/* Alpha (0..255) of a pixel covered by a disc of radius r centred on the
 * integer point (cx, cy). The pixel centre is sampled at +0.5. */
static uint8_t disc_coverage(int px, int py, int cx, int cy, int r) {
    float dx = (float)px + 0.5f - (float)cx;
    float dy = (float)py + 0.5f - (float)cy;
    float cov = (float)r + 0.5f - sqrtf(dx * dx + dy * dy);
    if (cov <= 0.0f) {
        return 0;
    }
    if (cov >= 1.0f) {
        return 255;
    }
    return (uint8_t)(cov * 255.0f + 0.5f);
}

/* Alpha (0..255) of a pixel covered by a 1 px circular outline of radius r
 * (centre-line at r - 0.5). */
static uint8_t ring_coverage(int px, int py, int cx, int cy, int r) {
    float dx = (float)px + 0.5f - (float)cx;
    float dy = (float)py + 0.5f - (float)cy;
    float d = fabsf(sqrtf(dx * dx + dy * dy) - ((float)r - 0.5f));
    float cov = 1.0f - d;
    if (cov <= 0.0f) {
        return 0;
    }
    if (cov >= 1.0f) {
        return 255;
    }
    return (uint8_t)(cov * 255.0f + 0.5f);
}

/* ------------------------------------------------------------------------- */
/* Rectangles                                                                */
/* ------------------------------------------------------------------------- */

gfx_rect_t gfx_rect_intersect(gfx_rect_t a, gfx_rect_t b) {
    gfx_rect_t r = {0, 0, 0, 0};
    if (gfx_rect_empty(a) || gfx_rect_empty(b)) {
        return r;
    }
    int32_t x0 = imax(a.x, b.x);
    int32_t y0 = imax(a.y, b.y);
    int32_t x1 = imin((int32_t)a.x + a.w, (int32_t)b.x + b.w);
    int32_t y1 = imin((int32_t)a.y + a.h, (int32_t)b.y + b.h);
    if (x1 <= x0 || y1 <= y0) {
        return r;
    }
    r.x = (int16_t)x0;
    r.y = (int16_t)y0;
    r.w = (int16_t)(x1 - x0);
    r.h = (int16_t)(y1 - y0);
    return r;
}

bool gfx_rect_empty(gfx_rect_t r) {
    return r.w <= 0 || r.h <= 0;
}

gfx_rect_t gfx_rect_union(gfx_rect_t a, gfx_rect_t b) {
    if (gfx_rect_empty(a)) {
        return b;
    }
    if (gfx_rect_empty(b)) {
        return a;
    }
    int32_t x0 = imin(a.x, b.x);
    int32_t y0 = imin(a.y, b.y);
    int32_t x1 = imax((int32_t)a.x + a.w, (int32_t)b.x + b.w);
    int32_t y1 = imax((int32_t)a.y + a.h, (int32_t)b.y + b.h);
    gfx_rect_t r;
    r.x = (int16_t)x0;
    r.y = (int16_t)y0;
    r.w = (int16_t)(x1 - x0);
    r.h = (int16_t)(y1 - y0);
    return r;
}

void gfx_set_clip(gfx_canvas_t *c, gfx_rect_t clip) {
    if (c == NULL) {
        return;
    }
    c->clip = clip;
}

/* ------------------------------------------------------------------------- */
/* Fills                                                                     */
/* ------------------------------------------------------------------------- */

void gfx_fill(gfx_canvas_t *c, gfx_color_t col) {
    if (c == NULL || c->buf == NULL) {
        return;
    }
    gfx_rect_t r = gfx_rect_intersect(c->clip, c->area);
    if (gfx_rect_empty(r)) {
        return;
    }
    uint16_t *p = px_at(c, r.x, r.y);
    size_t stride = (size_t)c->area.w;
    for (int row = 0; row < r.h; row++) {
        fill_span(p, r.w, col);
        p += stride;
    }
}

void gfx_fill_rect(gfx_canvas_t *c, int x, int y, int w, int h, gfx_color_t col) {
    if (c == NULL || c->buf == NULL) {
        return;
    }
    gfx_rect_t r = clip_to_canvas(c, x, y, w, h);
    if (gfx_rect_empty(r)) {
        return;
    }
    uint16_t *p = px_at(c, r.x, r.y);
    size_t stride = (size_t)c->area.w;
    for (int row = 0; row < r.h; row++) {
        fill_span(p, r.w, col);
        p += stride;
    }
}

void gfx_fill_rect_alpha(gfx_canvas_t *c, int x, int y, int w, int h, gfx_color_t col, uint8_t alpha) {
    if (c == NULL || c->buf == NULL || alpha == 0u) {
        return;
    }
    gfx_rect_t r = clip_to_canvas(c, x, y, w, h);
    if (gfx_rect_empty(r)) {
        return;
    }
    uint16_t *p = px_at(c, r.x, r.y);
    size_t stride = (size_t)c->area.w;
    for (int row = 0; row < r.h; row++) {
        blend_span(p, r.w, col, alpha);
        p += stride;
    }
}

void gfx_hline(gfx_canvas_t *c, int x, int y, int w, gfx_color_t col) {
    gfx_fill_rect(c, x, y, w, 1, col);
}

void gfx_vline(gfx_canvas_t *c, int x, int y, int h, gfx_color_t col) {
    gfx_fill_rect(c, x, y, 1, h, col);
}

void gfx_fill_gradient_v(gfx_canvas_t *c, int x, int y, int w, int h, gfx_color_t top, gfx_color_t bottom) {
    if (c == NULL || c->buf == NULL) {
        return;
    }
    gfx_rect_t r = clip_to_canvas(c, x, y, w, h);
    if (gfx_rect_empty(r)) {
        return;
    }
    if (h <= 1) {
        gfx_fill_rect(c, x, y, w, h, top);
        return;
    }

    const uint32_t tr = expand_r(top), tg = expand_g(top), tb = expand_b(top);
    const uint32_t br = expand_r(bottom), bg = expand_g(bottom), bb = expand_b(bottom);
    const uint32_t den = (uint32_t)(h - 1);
    size_t stride = (size_t)c->area.w;

    uint16_t *p = px_at(c, r.x, r.y);
    for (int sy = r.y; sy < r.y + r.h; sy++, p += stride) {
        uint32_t t = ((uint32_t)(sy - y) * 65535u) / den;
        uint32_t it = 65535u - t;
        uint32_t rr = (tr * it + br * t + 32767u) / 65535u;
        uint32_t gg = (tg * it + bg * t + 32767u) / 65535u;
        uint32_t bbv = (tb * it + bb * t + 32767u) / 65535u;
        fill_span(p, r.w, pack_rgb(rr, gg, bbv));
    }
}

/* ------------------------------------------------------------------------- */
/* Rounded rectangles                                                        */
/* ------------------------------------------------------------------------- */

static int clamp_radius(int w, int h, int r) {
    int maxr = imin(w, h) / 2;
    if (r > maxr) {
        r = maxr;
    }
    if (r < 0) {
        r = 0;
    }
    return r;
}

/* Draw one quarter of a 1 px rounded-rect outline: the box [bx, bx+bw) x
 * [by, by+bh) clipped to the canvas, sampled against the circle centred on
 * (cx, cy). */
static void draw_arc_box(gfx_canvas_t *c, int bx, int by, int bw, int bh, int cx, int cy, int r,
                         gfx_color_t col) {
    gfx_rect_t cl = gfx_rect_intersect(c->clip, c->area);
    int x0 = imax(bx, cl.x);
    int y0 = imax(by, cl.y);
    int x1 = imin(bx + bw, cl.x + cl.w);
    int y1 = imin(by + bh, cl.y + cl.h);
    for (int sy = y0; sy < y1; sy++) {
        for (int sx = x0; sx < x1; sx++) {
            blend_span(px_at(c, sx, sy), 1, col, ring_coverage(sx, sy, cx, cy, r));
        }
    }
}

void gfx_fill_round_rect(gfx_canvas_t *c, int x, int y, int w, int h, int r, gfx_color_t col) {
    if (c == NULL || c->buf == NULL || w <= 0 || h <= 0) {
        return;
    }
    r = clamp_radius(w, h, r);
    gfx_rect_t cr = clip_to_canvas(c, x, y, w, h);
    if (gfx_rect_empty(cr)) {
        return;
    }
    if (r == 0) {
        gfx_fill_rect(c, x, y, w, h, col);
        return;
    }

    const int x1 = x + w;
    const int y1 = y + h;
    const int rx0 = cr.x;
    const int rx1 = cr.x + cr.w;

    for (int sy = cr.y; sy < cr.y + cr.h; sy++) {
        int dy_top = sy - y;
        int dy_bot = (y1 - 1) - sy;
        bool in_top = dy_top < r;
        bool in_bot = dy_bot < r;

        if (!in_top && !in_bot) {
            fill_span(px_at(c, cr.x, sy), cr.w, col);
            continue;
        }

        int cyc = in_top ? (y + r) : (y1 - r);
        int solid0 = imax(rx0, x + r);
        int solid1 = imin(rx1, x1 - r);
        if (solid1 > solid0) {
            fill_span(px_at(c, solid0, sy), solid1 - solid0, col);
        }

        /* Left corner arc. */
        int l0 = imax(rx0, x);
        int l1 = imin(rx1, x + r);
        for (int sx = l0; sx < l1; sx++) {
            blend_span(px_at(c, sx, sy), 1, col, disc_coverage(sx, sy, x + r, cyc, r));
        }
        /* Right corner arc. */
        int q0 = imax(rx0, x1 - r);
        int q1 = imin(rx1, x1);
        for (int sx = q0; sx < q1; sx++) {
            blend_span(px_at(c, sx, sy), 1, col, disc_coverage(sx, sy, x1 - r, cyc, r));
        }
    }
}

void gfx_draw_round_rect(gfx_canvas_t *c, int x, int y, int w, int h, int r, gfx_color_t col) {
    if (c == NULL || c->buf == NULL || w <= 0 || h <= 0) {
        return;
    }
    r = clamp_radius(w, h, r);

    /* Straight edges (always 1 px, no anti-aliasing needed). */
    if (w - 2 * r > 0) {
        gfx_hline(c, x + r, y, w - 2 * r, col);
        gfx_hline(c, x + r, y + h - 1, w - 2 * r, col);
    }
    if (h - 2 * r > 0) {
        gfx_vline(c, x, y + r, h - 2 * r, col);
        gfx_vline(c, x + w - 1, y + r, h - 2 * r, col);
    }
    if (r == 0) {
        return;
    }

    /* Four quarter-arcs, one clipped corner box each. */
    draw_arc_box(c, x, y, r, r, x + r, y + r, r, col);
    draw_arc_box(c, x + w - r, y, r, r, x + w - r, y + r, r, col);
    draw_arc_box(c, x, y + h - r, r, r, x + r, y + h - r, r, col);
    draw_arc_box(c, x + w - r, y + h - r, r, r, x + w - r, y + h - r, r, col);
}

/* ------------------------------------------------------------------------- */
/* Circle and triangle                                                       */
/* ------------------------------------------------------------------------- */

void gfx_fill_circle(gfx_canvas_t *c, int cx, int cy, int r, gfx_color_t col) {
    if (c == NULL || c->buf == NULL || r < 0) {
        return;
    }
    if (r == 0) {
        gfx_fill_rect(c, cx, cy, 1, 1, col);
        return;
    }
    gfx_rect_t cr = clip_to_canvas(c, cx - r - 1, cy - r - 1, 2 * r + 3, 2 * r + 3);
    if (gfx_rect_empty(cr)) {
        return;
    }

    const float outer = (float)r + 0.5f;
    const float inner = (float)r - 0.75f;
    const float o2 = outer * outer;
    const float i2 = inner > 0.0f ? inner * inner : 0.0f;
    const int xlim0 = cr.x;
    const int xlim1 = cr.x + cr.w - 1;
    size_t stride = (size_t)c->area.w;
    uint16_t *rowp = px_at(c, cr.x, cr.y);

    for (int sy = cr.y; sy < cr.y + cr.h; sy++, rowp += stride) {
        float dy = (float)sy + 0.5f - (float)cy;
        float oy = o2 - dy * dy;
        if (oy <= 0.0f) {
            continue;
        }
        float ox = sqrtf(oy);
        int xs = (int)ceilf((float)cx - ox);
        int xe = (int)floorf((float)cx + ox);
        if (xs < xlim0) {
            xs = xlim0;
        }
        if (xe > xlim1) {
            xe = xlim1;
        }
        if (xe < xs) {
            continue;
        }

        int is = 1, ie = 0;
        float iy = i2 - dy * dy;
        if (iy > 0.0f) {
            float ix = sqrtf(iy);
            is = (int)ceilf((float)cx - ix);
            ie = (int)floorf((float)cx + ix);
        }
        if (is < xs) {
            is = xs;
        }
        if (ie > xe) {
            ie = xe;
        }
        if (ie >= is) {
            fill_span(&rowp[is - cr.x], ie - is + 1, col);
        }
        for (int sx = xs; sx < is; sx++) {
            blend_span(&rowp[sx - cr.x], 1, col, disc_coverage(sx, sy, cx, cy, r));
        }
        for (int sx = ie + 1; sx <= xe; sx++) {
            blend_span(&rowp[sx - cr.x], 1, col, disc_coverage(sx, sy, cx, cy, r));
        }
    }
}

/* Edge function in doubled pixel coordinates (pixel centres are odd). */
static int64_t tri_edge(int ax, int ay, int bx, int by, int px, int py) {
    return (int64_t)(2 * (bx - ax)) * (py - 2 * ay) - (int64_t)(2 * (by - ay)) * (px - 2 * ax);
}

void gfx_fill_triangle(gfx_canvas_t *c, int x0, int y0, int x1, int y1, int x2, int y2, gfx_color_t col) {
    if (c == NULL || c->buf == NULL) {
        return;
    }
    int64_t area = (int64_t)(x1 - x0) * (y2 - y0) - (int64_t)(y1 - y0) * (x2 - x0);
    if (area == 0) {
        return;
    }
    if (area < 0) {
        int t;
        t = x1;
        x1 = x2;
        x2 = t;
        t = y1;
        y1 = y2;
        y2 = t;
    }

    int minx = imin(imin(x0, x1), x2);
    int maxx = imax(imax(x0, x1), x2);
    int miny = imin(imin(y0, y1), y2);
    int maxy = imax(imax(y0, y1), y2);
    gfx_rect_t cr = clip_to_canvas(c, minx, miny, maxx - minx + 1, maxy - miny + 1);
    if (gfx_rect_empty(cr)) {
        return;
    }

    size_t stride = (size_t)c->area.w;
    uint16_t *rowp = px_at(c, cr.x, cr.y);
    for (int sy = cr.y; sy < cr.y + cr.h; sy++, rowp += stride) {
        int py = 2 * sy + 1;
        for (int sx = cr.x; sx < cr.x + cr.w; sx++) {
            int px = 2 * sx + 1;
            if (tri_edge(x0, y0, x1, y1, px, py) >= 0 && tri_edge(x1, y1, x2, y2, px, py) >= 0 &&
                tri_edge(x2, y2, x0, y0, px, py) >= 0) {
                rowp[sx - cr.x] = col;
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Images                                                                    */
/* ------------------------------------------------------------------------- */

void gfx_draw_image(gfx_canvas_t *c, int x, int y, const gfx_image_t *img) {
    if (c == NULL || c->buf == NULL || img == NULL || img->pixels == NULL || img->width == 0 ||
        img->height == 0) {
        return;
    }
    gfx_rect_t r = clip_to_canvas(c, x, y, img->width, img->height);
    if (gfx_rect_empty(r)) {
        return;
    }

    const uint16_t *src = img->pixels + (size_t)(r.y - y) * img->width + (size_t)(r.x - x);
    uint16_t *dst = px_at(c, r.x, r.y);
    size_t sstride = img->width;
    size_t dstride = (size_t)c->area.w;
    for (int row = 0; row < r.h; row++) {
        for (int col = 0; col < r.w; col++) {
            dst[col] = src[col];
        }
        src += sstride;
        dst += dstride;
    }
}

void gfx_draw_image_scaled(gfx_canvas_t *c, int x, int y, int w, int h, const gfx_image_t *img) {
    if (c == NULL || c->buf == NULL || img == NULL || img->pixels == NULL || img->width == 0 ||
        img->height == 0 || w <= 0 || h <= 0) {
        return;
    }
    gfx_rect_t r = clip_to_canvas(c, x, y, w, h);
    if (gfx_rect_empty(r)) {
        return;
    }

    size_t stride = (size_t)c->area.w;
    uint16_t *dst = px_at(c, r.x, r.y);
    for (int sy = r.y; sy < r.y + r.h; sy++, dst += stride) {
        uint32_t src_y = ((uint32_t)(sy - y) * img->height) / (uint32_t)h;
        const uint16_t *srow = img->pixels + (size_t)src_y * img->width;
        for (int sx = r.x; sx < r.x + r.w; sx++) {
            uint32_t src_x = ((uint32_t)(sx - x) * img->width) / (uint32_t)w;
            dst[sx - r.x] = srow[src_x];
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Text                                                                      */
/* ------------------------------------------------------------------------- */

static const gfx_glyph_t *find_glyph(const gfx_font_t *f, unsigned ch) {
    if (f == NULL || f->glyphs == NULL) {
        return NULL;
    }
    if (ch < f->first || ch > f->last) {
        return NULL;
    }
    return &f->glyphs[ch - f->first];
}

static void draw_glyph(gfx_canvas_t *c, const gfx_font_t *f, const gfx_glyph_t *g, int bx, int by,
                       gfx_color_t col) {
    if (f->bitmap == NULL || g->width == 0 || g->height == 0) {
        return;
    }
    gfx_rect_t r = clip_to_canvas(c, bx, by, g->width, g->height);
    if (gfx_rect_empty(r)) {
        return;
    }

    const int row_bytes = (g->width + 1) / 2;
    const uint8_t *base = f->bitmap + g->bitmap_offset;
    size_t stride = (size_t)c->area.w;
    uint16_t *dst = px_at(c, r.x, r.y);

    for (int sy = r.y; sy < r.y + r.h; sy++, dst += stride) {
        const uint8_t *src = base + (size_t)(sy - by) * (size_t)row_bytes;
        for (int sx = r.x; sx < r.x + r.w; sx++) {
            int gx = sx - bx;
            uint8_t byte = src[gx >> 1];
            uint8_t nib = (gx & 1) ? (uint8_t)(byte & 0x0Fu) : (uint8_t)(byte >> 4);
            if (nib == 0u) {
                continue;
            }
            uint16_t *p = &dst[sx - r.x];
            if (nib == 15u) {
                *p = col; /* fast path: fully opaque */
            } else {
                *p = gfx_blend(col, *p, (uint8_t)(nib * 17u));
            }
        }
    }
}

int gfx_text_width(const gfx_font_t *f, const char *s) {
    if (f == NULL || s == NULL) {
        return 0;
    }
    int width = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p != 0; p++) {
        const gfx_glyph_t *g = find_glyph(f, *p);
        if (g == NULL) {
            continue; /* out of range: skipped, no advance */
        }
        width += g->x_advance;
    }
    return width;
}

int gfx_draw_text(gfx_canvas_t *c, const gfx_font_t *f, int x, int baseline_y, const char *s,
                  gfx_color_t col) {
    if (c == NULL || c->buf == NULL || f == NULL || s == NULL) {
        return x;
    }
    int pen = x;
    for (const unsigned char *p = (const unsigned char *)s; *p != 0; p++) {
        const gfx_glyph_t *g = find_glyph(f, *p);
        if (g == NULL) {
            continue;
        }
        draw_glyph(c, f, g, pen + g->x_offset, baseline_y + g->y_offset, col);
        pen += g->x_advance;
    }
    return pen;
}

int gfx_draw_text_clipped(gfx_canvas_t *c, const gfx_font_t *f, int x, int baseline_y, int max_w,
                          const char *s, gfx_color_t col) {
    if (c == NULL || c->buf == NULL || f == NULL || s == NULL || max_w <= 0) {
        return x;
    }

    /* Restrict drawing to [x, x + max_w) so a glyph at the edge cannot spill. */
    gfx_rect_t saved = c->clip;
    gfx_rect_t lim = saved;
    lim.x = (int16_t)imax(-32768, imin(32767, x));
    lim.w = (int16_t)imax(0, imin(32767, max_w));
    c->clip = gfx_rect_intersect(saved, lim);

    int end = x;
    if (gfx_text_width(f, s) <= max_w) {
        end = gfx_draw_text(c, f, x, baseline_y, s, col);
    } else {
        int ell_w = gfx_text_width(f, "...");
        if (max_w > ell_w) {
            int pen = x;
            const int avail = max_w - ell_w;
            for (const unsigned char *p = (const unsigned char *)s; *p != 0; p++) {
                const gfx_glyph_t *g = find_glyph(f, *p);
                if (g == NULL) {
                    continue;
                }
                if (pen - x + g->x_advance > avail) {
                    break;
                }
                draw_glyph(c, f, g, pen + g->x_offset, baseline_y + g->y_offset, col);
                pen += g->x_advance;
            }
            end = gfx_draw_text(c, f, pen, baseline_y, "...", col);
        }
        /* max_w <= ell_w: nothing fits, draw nothing. */
    }

    c->clip = saved;
    return end;
}
