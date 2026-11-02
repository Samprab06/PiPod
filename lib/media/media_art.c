/**
 * @file media_art.c
 * @brief Album art: streaming JPEG decode + centre-crop/box-resize to 120x120.
 *
 * See media_art.h for the contract. This file contains three pieces:
 *  1. a filtering reader that turns the encoded byte range of a media file
 *     (raw / ID3 unsynchronised / base64) into a plain JPEG byte stream,
 *  2. the TJpgDec glue: input callback, MCU output callback and scale choice,
 *  3. the scaler that maps decoded MCU blocks onto the 120x120 canvas with an
 *     exact area-average ("box") filter, plus the placeholder painter.
 */

#include <string.h>

#include "media_art.h"

#if defined(_WIN32) && defined(__MINGW32__)
/* TJpgDec R0.03 selects a pre-C99 integer typedef block when _WIN32 is defined
 * ("VC++ or some compiler without stdint.h"). MinGW defines _WIN32 but ships a
 * complete <stdint.h>, and the two definitions of uint32_t/int32_t disagree.
 * Hide the macro while the header is parsed; device toolchains never take that
 * branch. <string.h> is already included above so no system header is parsed
 * without _WIN32. */
#pragma push_macro("_WIN32")
#undef _WIN32
#include "tjpgd.h"
#pragma pop_macro("_WIN32")
#else
#include "tjpgd.h"
#endif

/* -------------------------------------------------------------------------- */
/* Tunables                                                                   */
/* -------------------------------------------------------------------------- */

/** File read chunk for the filtering paths (base64 / unsync). */
#define ART_IO_BUF 64u

/**
 * Largest accepted shorter side after prescaling. The box filter accumulates
 * channel * weight in uint32_t, where the total weight is side * side, so
 * 255 * side^2 must fit: side <= 4104. Anything larger (> 32768 px artwork)
 * is rejected instead of silently overflowing.
 */
#define ART_MAX_SIDE 4096u

/* The pool must cover the derivation documented in media_art.h (3988 bytes for
 * four quantisation tables); this catches an accidental shrink. */
_Static_assert(MEDIA_ART_WORK_SIZE >= 3988u, "MEDIA_ART_WORK_SIZE below the TJpgDec requirement");

/* -------------------------------------------------------------------------- */
/* Filtering reader                                                           */
/* -------------------------------------------------------------------------- */

typedef struct {
    const media_source_t *src;
    uint32_t base_off;   /**< file offset of the first encoded byte */
    uint32_t file_off;   /**< next file offset to fetch */
    uint32_t file_end;   /**< end of the encoded range in the file */
    uint32_t limit;      /**< number of decoded bytes the JPEG stream may use */
    uint32_t pos;        /**< decoded bytes delivered so far */
    uint32_t b64_acc;    /**< base64 quantum accumulator (24 bits) */
    uint8_t b64_n;       /**< base64 symbols collected in b64_acc (0..3) */
    uint8_t b64_out[3];  /**< bytes of the current base64 quantum */
    uint8_t b64_len;     /**< valid bytes in b64_out */
    uint8_t b64_idx;     /**< next byte of b64_out */
    uint8_t b64_end;     /**< padding seen / stream finished */
    uint8_t pb[2];       /**< pushback for the SOI sniff */
    uint8_t pb_len;
    uint8_t pb_idx;
    uint8_t buf[ART_IO_BUF];
    uint8_t buf_len;
    uint8_t buf_idx;
    uint8_t enc;         /**< media_art_enc_t */
    uint8_t io_error;    /**< source read() returned -1 */
    uint8_t unsync_hold; /**< byte held back behind a pending 0xFF (ID3 unsync) */
    uint8_t unsync_hold_valid;
} art_reader_t;

/** Value of one base64 symbol, or -1. */
static int art_b64_val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/** Refill the file chunk buffer. False at a clean end of the range. */
static int art_rd_fill(art_reader_t *r)
{
    uint32_t want;
    int32_t got;

    if (r->buf_idx < r->buf_len) return 1;
    if (r->file_off >= r->file_end) return 0;

    want = r->file_end - r->file_off;
    if (want > ART_IO_BUF) want = ART_IO_BUF;
    got = r->src->read(r->src->ctx, r->file_off, r->buf, want);
    if (got <= 0) {
        if (got < 0) r->io_error = 1;
        return 0;
    }
    r->file_off += (uint32_t)got;
    r->buf_idx = 0;
    r->buf_len = (uint8_t)got;
    return 1;
}

/** Next raw byte of the encoded range, or -1. */
static int art_rd_raw_byte(art_reader_t *r)
{
    if (!art_rd_fill(r)) return -1;
    return r->buf[r->buf_idx++];
}

/** Next byte of the filtered (JPEG) stream, or -1 at the end of the range. */
static int art_rd_byte(art_reader_t *r)
{
    if (r->pos >= r->limit) return -1;

    if (r->enc == MEDIA_ART_ENC_RAW) {
        int c = art_rd_raw_byte(r);
        if (c < 0) return -1;
        r->pos++;
        return c;
    }

    if (r->enc == MEDIA_ART_ENC_ID3_UNSYNC) {
        int c;

        /* ID3 unsynchronisation inserts 0x00 after a 0xFF, so 0xFF has to be
         * held back until the following byte is known (0xFF 00 is one 0xFF,
         * 0xFF xx is 0xFF followed by xx). */
        if (r->unsync_hold_valid) {
            r->unsync_hold_valid = 0;
            r->pos++;
            return r->unsync_hold;
        }
        c = art_rd_raw_byte(r);
        if (c < 0) return -1;
        if (c == 0xFF) {
            int n = art_rd_raw_byte(r);

            if (n > 0 && n != 0x00) {
                r->unsync_hold = (uint8_t)n;
                r->unsync_hold_valid = 1;
            }
            r->pos++;
            return 0xFF;
        }
        r->pos++;
        return c;
    }

    /* MEDIA_ART_ENC_BASE64: decode 4 symbols into 3 bytes, on demand. */
    for (;;) {
        int c;
        int v;

        if (r->b64_idx < r->b64_len) {
            r->pos++;
            return r->b64_out[r->b64_idx++];
        }
        if (r->b64_end) return -1;

        c = art_rd_raw_byte(r);
        if (c < 0) return -1;
        if (c == '=') {
            r->b64_end = 1;
            return -1;
        }
        v = art_b64_val(c);
        if (v < 0) {
            if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
            r->b64_end = 1; /* not base64 text */
            return -1;
        }
        r->b64_acc = (r->b64_acc << 6) | (uint32_t)v;
        if (++r->b64_n < 4) continue;

        r->b64_out[0] = (uint8_t)(r->b64_acc >> 16);
        r->b64_out[1] = (uint8_t)(r->b64_acc >> 8);
        r->b64_out[2] = (uint8_t)r->b64_acc;
        r->b64_len = 3;
        r->b64_idx = 0;
        r->b64_acc = 0;
        r->b64_n = 0;
    }
}

/** Read up to @p len filtered bytes; @p dst may be NULL to skip them. */
static size_t art_rd_read(art_reader_t *r, uint8_t *dst, size_t len)
{
    size_t done = 0;

    /* Bytes captured by the SOI sniff are replayed first. */
    while (r->pb_idx < r->pb_len && done < len) {
        if (dst != NULL) dst[done] = r->pb[r->pb_idx];
        r->pb_idx++;
        done++;
    }
    if (done == len) return done;

    if (r->enc == MEDIA_ART_ENC_RAW) {
        /* Unfiltered: the file range is the JPEG stream, so read (or skip)
         * straight from the source instead of going byte by byte. */
        size_t rem = len - done;

        if (rem > r->limit - r->pos) rem = r->limit - r->pos;
        if (dst == NULL) {
            r->pos += (uint32_t)rem;
            return done + rem;
        }
        while (rem != 0) {
            uint32_t chunk = (uint32_t)rem;
            int32_t got;

            if (chunk > r->limit - r->pos) chunk = r->limit - r->pos;
            got = r->src->read(r->src->ctx, r->base_off + r->pos, dst + done, chunk);
            if (got <= 0) {
                if (got < 0) r->io_error = 1;
                break;
            }
            r->pos += (uint32_t)got;
            done += (size_t)got;
            rem -= (size_t)got;
            if ((uint32_t)got < chunk) break; /* short read: end of file */
        }
        return done;
    }

    while (done < len) {
        int c = art_rd_byte(r);
        if (c < 0) break;
        if (dst != NULL) dst[done] = (uint8_t)c;
        done++;
    }
    return done;
}

/**
 * Prepare the reader and point it at the JPEG stream described by @p ref.
 * @return 0 when the reference is inconsistent (caller reports FORMAT).
 */
static int art_rd_init(art_reader_t *r, const media_source_t *src, const media_art_ref_t *ref)
{
    uint32_t avail;
    uint32_t n;

    memset(r, 0, sizeof *r);
    r->src = src;
    r->enc = (ref->encoding <= (uint8_t)MEDIA_ART_ENC_BASE64) ? ref->encoding
                                                             : (uint8_t)MEDIA_ART_ENC_RAW;
    r->base_off = ref->offset;

    if (ref->offset > src->size) return 0;
    avail = src->size - ref->offset;
    n = (ref->size < avail) ? ref->size : avail;
    r->file_off = ref->offset;
    r->file_end = ref->offset + n;
    r->limit = n;

    if (r->enc == MEDIA_ART_ENC_BASE64) {
        if (ref->inner_offset > 0xFFFFFFFFu - ref->inner_size) return 0;
        if (ref->inner_size == 0) return 0;
        r->limit = ref->inner_offset + ref->inner_size;
    }
    return 1;
}

/** Consume @p n decoded bytes (used to step over the PICTURE header). */
static int art_rd_skip(art_reader_t *r, uint32_t n)
{
    while (n != 0) {
        uint32_t chunk = (n > 4096u) ? 4096u : n;
        size_t got = art_rd_read(r, NULL, chunk);

        if (got != chunk) return 0;
        n -= chunk;
    }
    return 1;
}

/** Read the first two filtered bytes and require JPEG SOI (0xFF 0xD8). */
static int art_rd_sniff_soi(art_reader_t *r)
{
    uint8_t soi[2];

    if (art_rd_read(r, soi, sizeof soi) != sizeof soi) return 0;
    if (soi[0] != 0xFFu || soi[1] != 0xD8u) return 0;
    r->pb[0] = soi[0];
    r->pb[1] = soi[1];
    r->pb_len = 2;
    r->pb_idx = 0;
    return 1;
}

/* -------------------------------------------------------------------------- */
/* Decode state                                                               */
/* -------------------------------------------------------------------------- */

/**
 * Largest number of cropped source pixels held while one MCU row is decoded.
 *
 * TJpgDec delivers one MCU at a time (row-major inside the MCU, MCU columns
 * innermost), so the rows of an MCU band arrive column by column. Buffering the
 * cropped part of the band turns that back into row order for the scaler.
 * Bound: the shorter side after prescaling is < 240 for every scale (the next
 * scale up would still have been >= 120), and an MCU is at most 16 rows tall,
 * so 240 x 16 = 3840 pixels. The upscale path keeps one extra row (<= 119 x 17).
 */
#define ART_BAND_PIXELS 3840u

/** Cropped source band, filled by the MCU callback, consumed row by row. */
static uint16_t art_band[ART_BAND_PIXELS];

/** One output row of area-weighted channel sums. */
typedef struct {
    uint32_t sum[3][MEDIA_ART_SIZE];
    int16_t oy; /**< output row being accumulated, -1 when free */
} art_rowacc_t;

typedef struct {
    art_reader_t rd;
    uint16_t *out;
    uint32_t img_w;   /**< decoded image width after prescaling */
    uint32_t img_h;   /**< decoded image height after prescaling */
    uint32_t crop_x;  /**< left edge of the square crop in img_* coordinates */
    uint32_t crop_y;  /**< top edge of the square crop */
    uint32_t side;    /**< shorter side of the decoded image */
    uint32_t weight;  /**< side * side: total area weight of one output pixel */
    uint32_t band_y0; /**< first cropped source row stored in art_band */
    uint32_t band_rows;
    uint32_t band_max; /**< rows art_band can hold for this image */
    art_rowacc_t acc[2];
    uint16_t rows_done;
    uint16_t next_oy; /**< next output row for the upscale path */
    uint8_t band_active;
    uint8_t upscale;
} art_ctx_t;

/** Two output-row accumulators (3 x 120 uint32 each) + reader + misc state. */
#define ART_CTX_MAX 4096u
_Static_assert(sizeof(art_ctx_t) <= ART_CTX_MAX, "album art scaler state too large");

/** Convert 5-6-5 channels to 8 bits each. */
static void art_expand(uint16_t p, uint32_t *r, uint32_t *g, uint32_t *b)
{
    uint32_t r5 = ((uint32_t)p >> 11) & 0x1Fu;
    uint32_t g6 = ((uint32_t)p >> 5) & 0x3Fu;
    uint32_t b5 = (uint32_t)p & 0x1Fu;

    *r = (r5 << 3) | (r5 >> 2);
    *g = (g6 << 2) | (g6 >> 4);
    *b = (b5 << 3) | (b5 >> 2);
}

/** Convert 8-bit channels to a native RGB565 pixel. */
static uint16_t art_rgb565(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

/** Write one accumulated output row and mark the accumulator free. */
static void art_flush_row(art_ctx_t *c, art_rowacc_t *a)
{
    uint16_t *dst;
    uint32_t half = c->weight / 2;
    unsigned ox;

    if (a->oy < 0) return;
    dst = c->out + (size_t)a->oy * MEDIA_ART_SIZE;
    for (ox = 0; ox < MEDIA_ART_SIZE; ox++) {
        uint32_t r = (a->sum[0][ox] + half) / c->weight;
        uint32_t g = (a->sum[1][ox] + half) / c->weight;
        uint32_t b = (a->sum[2][ox] + half) / c->weight;

        dst[ox] = art_rgb565(r, g, b);
    }
    c->rows_done++;
    a->oy = -1;
}

/** Accumulator for output row @p oy, flushing a completed row if needed. */
static art_rowacc_t *art_acc_row(art_ctx_t *c, uint32_t oy)
{
    art_rowacc_t *a = &c->acc[oy & 1u];

    if ((uint32_t)a->oy != oy) {
        art_flush_row(c, a);
        memset(a, 0, sizeof *a);
        a->oy = (int16_t)oy;
    }
    return a;
}

/**
 * Add one cropped source row to the output accumulators.
 *
 * @p y_rel counts from the top of the crop, @p x0 counts from the left edge of
 * the decoded image (art_band_flush passes crop_x). Source and output pixels
 * are unit squares; scaling both coordinates by MEDIA_ART_SIZE makes every
 * overlap an integer, so the weights are exact box-filter areas (no floating
 * point and no division per pixel).
 */
static void art_box_row(art_ctx_t *c, const uint16_t *row, uint32_t x0, uint32_t n,
                        uint32_t y_rel)
{
    art_rowacc_t *a0;
    art_rowacc_t *a1;
    uint32_t v = y_rel * MEDIA_ART_SIZE;
    uint32_t oy0 = v / c->side;
    uint32_t oy1 = (v + MEDIA_ART_SIZE - 1u) / c->side;
    uint32_t v0 = (oy0 + 1u) * c->side - v;
    uint32_t v1 = v + MEDIA_ART_SIZE - oy1 * c->side;
    uint32_t i;

    if (v0 > MEDIA_ART_SIZE) v0 = MEDIA_ART_SIZE;
    if (v1 > MEDIA_ART_SIZE) v1 = MEDIA_ART_SIZE;
    a0 = art_acc_row(c, oy0);
    a1 = (oy1 == oy0) ? a0 : art_acc_row(c, oy1);

    for (i = 0; i < n; i++) {
        uint32_t x = x0 + i;
        uint32_t pix;
        uint32_t r;
        uint32_t g;
        uint32_t b;
        uint32_t u;
        uint32_t ox0;
        uint32_t ox1;
        uint32_t w0;
        uint32_t w1;

        if (x < c->crop_x || x >= c->crop_x + c->side) continue;
        pix = (uint32_t)row[i];
        r = (pix >> 11) & 0x1Fu;
        g = (pix >> 5) & 0x3Fu;
        b = pix & 0x1Fu;
        /* Expand to 8 bits; averaging happens in 8-bit space. */
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);

        u = (x - c->crop_x) * MEDIA_ART_SIZE;
        ox0 = u / c->side;
        ox1 = (u + MEDIA_ART_SIZE - 1u) / c->side;
        w0 = (ox0 + 1u) * c->side - u;
        w1 = u + MEDIA_ART_SIZE - ox1 * c->side;
        if (w0 > MEDIA_ART_SIZE) w0 = MEDIA_ART_SIZE;
        if (w1 > MEDIA_ART_SIZE) w1 = MEDIA_ART_SIZE;

        /* Up to 2 output columns x 2 output rows overlap this source pixel. */
        {
            uint32_t wt = w0 * v0;

            a0->sum[0][ox0] += r * wt;
            a0->sum[1][ox0] += g * wt;
            a0->sum[2][ox0] += b * wt;
        }
        if (ox1 != ox0) {
            uint32_t wt = w1 * v0;

            a0->sum[0][ox1] += r * wt;
            a0->sum[1][ox1] += g * wt;
            a0->sum[2][ox1] += b * wt;
        }
        if (a1 != a0) {
            uint32_t wt = w0 * v1;

            a1->sum[0][ox0] += r * wt;
            a1->sum[1][ox0] += g * wt;
            a1->sum[2][ox0] += b * wt;
            if (ox1 != ox0) {
                wt = w1 * v1;
                a1->sum[0][ox1] += r * wt;
                a1->sum[1][ox1] += g * wt;
                a1->sum[2][ox1] += b * wt;
            }
        }
    }
}

/**
 * Overlap of the output span [out_lo, out_lo + span) with the source pixel
 * span [s * 120, s * 120 + 120), in 1/120 pixel units. Integer by design.
 */
static uint32_t art_overlap(uint32_t out_lo, uint32_t span, uint32_t s)
{
    uint32_t src_lo = s * MEDIA_ART_SIZE;
    uint32_t src_hi = src_lo + MEDIA_ART_SIZE;
    uint32_t hi = out_lo + span;
    uint32_t lo = (out_lo > src_lo) ? out_lo : src_lo;

    if (src_hi < hi) hi = src_hi;
    return (hi > lo) ? hi - lo : 0;
}

/** Pointer to cropped source row @p y_rel of the current band, or NULL. */
static const uint16_t *art_band_row(const art_ctx_t *c, uint32_t y_rel)
{
    if (y_rel < c->band_y0 || y_rel >= c->band_y0 + c->band_rows) return NULL;
    return art_band + (size_t)(y_rel - c->band_y0) * c->side;
}

/**
 * Emit every output row whose source rows are all buffered, area-weighted.
 * Used when the cropped image is smaller than 120x120 (artwork upscaled).
 *
 * The band row that the next (not yet complete) output row still needs is
 * carried over as row 0 of the next band by art_band_flush().
 */
static void art_up_rows(art_ctx_t *c)
{
    uint32_t last = c->band_y0 + c->band_rows - 1u;
    uint32_t total = c->weight;
    uint32_t half = total / 2u;

    while (c->next_oy < MEDIA_ART_SIZE) {
        uint32_t oy = c->next_oy;
        uint32_t v = oy * c->side;
        uint32_t y_lo = v / MEDIA_ART_SIZE;
        uint32_t y_hi = (v + c->side - 1u) / MEDIA_ART_SIZE;
        uint16_t *dst;
        uint32_t ox;

        if (y_hi > last || y_lo < c->band_y0) break;
        dst = c->out + (size_t)oy * MEDIA_ART_SIZE;

        for (ox = 0; ox < MEDIA_ART_SIZE; ox++) {
            uint32_t u = ox * c->side;
            uint32_t x_lo = u / MEDIA_ART_SIZE;
            uint32_t x_hi = (u + c->side - 1u) / MEDIA_ART_SIZE;
            uint32_t sr = 0;
            uint32_t sg = 0;
            uint32_t sb = 0;
            uint32_t y;

            for (y = y_lo; y <= y_hi; y++) {
                const uint16_t *row = art_band_row(c, y);
                uint32_t vw = art_overlap(v, c->side, y);
                uint32_t x;

                if (row == NULL) continue;
                for (x = x_lo; x <= x_hi; x++) {
                    uint32_t hw;
                    uint32_t r;
                    uint32_t g;
                    uint32_t b;
                    uint32_t wt;

                    hw = art_overlap(u, c->side, x);
                    if (hw == 0) continue;
                    art_expand(row[x], &r, &g, &b);
                    wt = hw * vw;
                    sr += r * wt;
                    sg += g * wt;
                    sb += b * wt;
                }
            }
            dst[ox] = art_rgb565((sr + half) / total, (sg + half) / total, (sb + half) / total);
        }
        c->rows_done++;
        c->next_oy++;
    }
}

/** Process the buffered MCU band; called when its last MCU column arrived. */
static void art_band_flush(art_ctx_t *c)
{
    uint32_t i;

    if (!c->band_active) return;

    if (c->upscale) {
        art_up_rows(c);
        if (c->next_oy >= MEDIA_ART_SIZE) {
            c->band_active = 0;
            return;
        }
        /* Keep the newest row: the next output row may still need it. */
        if (c->band_rows > 1u) {
            memmove(art_band, art_band + (size_t)(c->band_rows - 1u) * c->side,
                    (size_t)c->side * sizeof(uint16_t));
            c->band_y0 += c->band_rows - 1u;
            c->band_rows = 1;
        }
        return;
    }

    for (i = 0; i < c->band_rows; i++) {
        /* x0 = crop_x: the band row is already cropped, but art_box_row()
         * works in decoded-image coordinates. */
        art_box_row(c, art_band + (size_t)i * c->side, c->crop_x, c->side, c->band_y0 + i);
    }
    c->band_active = 0;
}

/** TJpgDec MCU output callback: buffer the cropped part of one MCU. */
static int art_outfunc(JDEC *jd, void *bitmap, JRECT *rect)
{
    art_ctx_t *c = (art_ctx_t *)jd->device;
    const uint16_t *src = (const uint16_t *)bitmap;
    uint32_t left = rect->left;
    uint32_t top = rect->top;
    uint32_t w = (uint32_t)rect->right - left + 1u;
    uint32_t h = (uint32_t)rect->bottom - top + 1u;
    uint32_t x_lo = (left > c->crop_x) ? left : c->crop_x;
    uint32_t x_hi = left + w;
    uint32_t iy;

    /* Refuse geometry TJpgDec should never produce: stops pointer overruns. */
    if (left + w > c->img_w || top + h > c->img_h) return 0;
    if (x_hi > c->crop_x + c->side) x_hi = c->crop_x + c->side;

    for (iy = 0; iy < h && x_lo < x_hi; iy++) {
        uint32_t y = top + iy;
        uint32_t rel;
        const uint16_t *row = src + (size_t)iy * w;
        uint16_t *dst;
        uint32_t i;

        if (y < c->crop_y || y >= c->crop_y + c->side) continue;
        if (!c->band_active) {
            c->band_y0 = y - c->crop_y;
            c->band_rows = 0;
            c->band_active = 1;
        }
        rel = y - c->crop_y - c->band_y0;
        if (rel >= c->band_max) return 0; /* would overrun art_band */
        dst = art_band + (size_t)rel * c->side;
        for (i = 0; i < x_hi - x_lo; i++) {
            dst[x_lo - c->crop_x + i] = row[(x_lo - left) + i];
        }
        if (rel + 1u > c->band_rows) c->band_rows = rel + 1u;
    }

    if (rect->right + 1u == c->img_w) art_band_flush(c);
    return 1;
}

/** TJpgDec input callback. */
static size_t art_infunc(JDEC *jd, uint8_t *buf, size_t len)
{
    art_ctx_t *c = (art_ctx_t *)jd->device;

    return art_rd_read(&c->rd, buf, len);
}

/** Smallest TJpgDec prescale (1/2, 1/4, 1/8) whose shorter side stays >= 120. */
static uint8_t art_choose_scale(uint32_t short_side)
{
    uint8_t s;

    for (s = 3; s > 0; s--) {
        if ((short_side >> s) >= MEDIA_ART_SIZE) return s;
    }
    return 0;
}

/** Map a TJpgDec result to the public status code. */
static media_art_status_t art_status(const art_reader_t *r, JRESULT res)
{
    if (r->io_error) return MEDIA_ART_ERR_IO;

    switch (res) {
    case JDR_OK:
        return MEDIA_ART_OK;
    case JDR_INP:  /* wrong stream termination (truncated / corrupt) */
    case JDR_FMT1: /* broken data */
    case JDR_FMT2: /* right format, not supported (segment larger than JD_SZBUF) */
    case JDR_FMT3: /* not supported (progressive, arithmetic, 12-bit, ...) */
        return MEDIA_ART_ERR_FORMAT;
    case JDR_MEM1: /* pool too small / unsupported table sizes */
    case JDR_MEM2:
    case JDR_PAR:
    case JDR_INTR:
    default:
        return MEDIA_ART_ERR_DECODE;
    }
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

media_art_status_t media_art_decode_120(const media_source_t *src, const media_art_ref_t *ref,
                                        uint16_t *out_rgb565, void *work, size_t work_len)
{
    /* The scaler state (two output-row accumulators + the reader) lives on the
     * stack: ~3.2 KB, see ART_CTX_MAX. */
    art_ctx_t ctx;
    art_ctx_t *c = &ctx;
    JDEC jd;
    JRESULT res;
    uint8_t scale;
    uint32_t short_side;

    if (src == NULL || out_rgb565 == NULL) return MEDIA_ART_ERR_DECODE;
    if (ref == NULL || ref->type == (uint8_t)MEDIA_ART_NONE || ref->size == 0) {
        return MEDIA_ART_ERR_NONE;
    }
    if (ref->type != (uint8_t)MEDIA_ART_JPEG) return MEDIA_ART_ERR_FORMAT;
    if (work == NULL || work_len < MEDIA_ART_WORK_SIZE) return MEDIA_ART_ERR_DECODE;
    if (((uintptr_t)work & 3u) != 0) return MEDIA_ART_ERR_DECODE;

    memset(c, 0, sizeof *c);
    c->out = out_rgb565;
    c->acc[0].oy = -1;
    c->acc[1].oy = -1;

    if (!art_rd_init(&c->rd, src, ref)) return MEDIA_ART_ERR_FORMAT;
    if (c->rd.enc == MEDIA_ART_ENC_BASE64) {
        /* Step over the FLAC PICTURE header; this also catches a truncated
         * base64 block before TJpgDec starts parsing. */
        if (!art_rd_skip(&c->rd, ref->inner_offset)) {
            return art_status(&c->rd, JDR_INP);
        }
    }
    if (!art_rd_sniff_soi(&c->rd)) return art_status(&c->rd, JDR_FMT3);

    res = jd_prepare(&jd, art_infunc, work, work_len, c);
    if (res != JDR_OK) return art_status(&c->rd, res);

    short_side = (jd.width < jd.height) ? jd.width : jd.height;
    scale = art_choose_scale(short_side);
    c->img_w = (uint32_t)jd.width >> scale;
    c->img_h = (uint32_t)jd.height >> scale;
    c->side = (c->img_w < c->img_h) ? c->img_w : c->img_h;
    if (c->side == 0) return MEDIA_ART_ERR_FORMAT;
    if (c->side > ART_MAX_SIDE) return MEDIA_ART_ERR_FORMAT;

    c->crop_x = (c->img_w - c->side) / 2u;
    c->crop_y = (c->img_h - c->side) / 2u;
    c->weight = c->side * c->side;
    c->upscale = (uint8_t)(c->side < MEDIA_ART_SIZE);

    /* One MCU band plus, when upscaling, the carried-over row. */
    c->band_max = ((uint32_t)jd.msy * 8u) >> scale;
    if (c->upscale) c->band_max++;
    if (c->band_max == 0 || c->side * c->band_max > ART_BAND_PIXELS) {
        return MEDIA_ART_ERR_FORMAT; /* cannot happen: see ART_BAND_PIXELS */
    }

    res = jd_decomp(&jd, art_outfunc, scale);
    if (res != JDR_OK) return art_status(&c->rd, res);

    /* The last MCU row is flushed by the callback; this is belt and braces. */
    art_band_flush(c);
    art_flush_row(c, &c->acc[0]);
    art_flush_row(c, &c->acc[1]);
    if (c->rows_done != MEDIA_ART_SIZE) return MEDIA_ART_ERR_DECODE;

    return art_status(&c->rd, JDR_OK);
}

/* -------------------------------------------------------------------------- */
/* Placeholder art                                                            */
/* -------------------------------------------------------------------------- */

/** Placeholder palettes: {dark, bright} per seed. */
static const uint8_t art_palette[6][2][3] = {
    { { 26, 34, 62 }, { 92, 110, 168 } },   /* indigo */
    { { 22, 48, 44 }, { 78, 158, 138 } },   /* teal */
    { { 56, 28, 34 }, { 176, 96, 104 } },   /* rose */
    { { 54, 40, 20 }, { 190, 140, 70 } },   /* amber */
    { { 24, 44, 26 }, { 96, 160, 92 } },    /* moss */
    { { 40, 30, 56 }, { 140, 108, 180 } },  /* violet */
};

/**
 * Inside test for the note glyph, in half-pixel coordinates (0..239).
 * Head: ellipse centred at (96, 172). Stem: the right edge of the head rising
 * to y = 52. Flag: a quadratic wedge down to y = 108.
 */
static int art_note_pixel(unsigned x, unsigned y)
{
    int dx = (int)x - 96;
    int dy = (int)y - 172;

    if ((int32_t)dx * dx * 676 + (int32_t)dy * dy * 1156 <= 1156 * 676) return 1;
    if (x >= 126 && x <= 140 && y >= 52 && y <= 172) return 1;
    if (y >= 52 && y <= 108 && x > 140) {
        unsigned k = 108u - y;
        unsigned off = (k * k) / 60u + k / 2u;

        if (x <= 140u + off) return 1;
    }
    return 0;
}

/** 2x2 coverage of the note glyph at pixel (x, y), 0..4. */
static unsigned art_note_cov(unsigned x, unsigned y, int shift_x, int shift_y)
{
    unsigned cov = 0;
    int i;
    int j;

    for (j = 0; j < 2; j++) {
        for (i = 0; i < 2; i++) {
            int sx = (int)(2u * x + (unsigned)i) + shift_x;
            int sy = (int)(2u * y + (unsigned)j) + shift_y;

            if (sx < 0 || sy < 0) continue;
            cov += (unsigned)art_note_pixel((unsigned)sx, (unsigned)sy);
        }
    }
    return cov;
}

void media_art_placeholder_120(uint16_t *out_rgb565, uint32_t seed)
{
    const uint8_t *lo = art_palette[seed % 6u][0];
    const uint8_t *hi = art_palette[seed % 6u][1];
    static const uint8_t glyph[3] = { 240, 242, 246 };
    static const uint8_t shadow[3] = { 12, 14, 22 };
    unsigned x;
    unsigned y;

    if (out_rgb565 == NULL) return;

    for (y = 0; y < MEDIA_ART_SIZE; y++) {
        uint32_t t = (uint32_t)y * 255u / (MEDIA_ART_SIZE - 1u);
        uint16_t *dst = out_rgb565 + (size_t)y * MEDIA_ART_SIZE;

        for (x = 0; x < MEDIA_ART_SIZE; x++) {
            int32_t dx = (int32_t)x * 2 - (MEDIA_ART_SIZE - 1);
            int32_t dy = (int32_t)y * 2 - (MEDIA_ART_SIZE - 1);
            uint32_t d2 = (uint32_t)(dx * dx + dy * dy);
            uint32_t vig = 255u - (d2 * 100u) / 28322u; /* 155 (corner) .. 255 */
            unsigned cov;
            unsigned cov_sh;
            unsigned ch;
            uint8_t rgb[3];

            for (ch = 0; ch < 3; ch++) {
                uint32_t v = (uint32_t)lo[ch] + ((uint32_t)(hi[ch] - lo[ch]) * t) / 255u;

                rgb[ch] = (uint8_t)((v * vig) / 255u);
            }

            cov_sh = art_note_cov(x, y, 4, 5);
            if (cov_sh != 0) {
                for (ch = 0; ch < 3; ch++) {
                    rgb[ch] = (uint8_t)(((uint32_t)rgb[ch] * (4u - cov_sh) +
                                         (uint32_t)shadow[ch] * cov_sh * 3u / 4u) / 4u);
                }
            }
            cov = art_note_cov(x, y, 0, 0);
            if (cov != 0) {
                for (ch = 0; ch < 3; ch++) {
                    uint32_t a = (uint32_t)rgb[ch] * (4u - cov);
                    uint32_t b = (uint32_t)glyph[ch] * cov;

                    rgb[ch] = (uint8_t)((a + b) / 4u);
                }
            }
            dst[x] = art_rgb565(rgb[0], rgb[1], rgb[2]);
        }
    }
}
