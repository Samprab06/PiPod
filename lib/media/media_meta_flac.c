/**
 * @file media_meta_flac.c
 * @brief FLAC: STREAMINFO, VORBIS_COMMENT and PICTURE metadata blocks.
 */
#include <string.h>

#include "media_meta_internal.h"

static uint8_t mime_type(const uint8_t *mime, uint32_t len) {
    if (len >= 10 && memcmp(mime, "image/jpeg", 10) == 0) return MEDIA_ART_JPEG;
    if (len >= 9 && memcmp(mime, "image/png", 9) == 0) return MEDIA_ART_PNG;
    return MEDIA_ART_OTHER;
}

static void flac_streaminfo(meta_ctx_t *c, uint32_t off, uint32_t len, media_meta_t *out) {
    uint8_t si[18];
    uint64_t x;
    uint32_t rate;
    uint64_t total;

    if (len < 18 || !meta_read(c, off, si, sizeof si)) return;
    x = meta_u64be(si + 10);
    rate = (uint32_t)(x >> 44);
    out->sample_rate = rate;
    out->channels = (uint8_t)(((x >> 41) & 0x07) + 1);
    out->bits_per_sample = (uint8_t)(((x >> 36) & 0x1F) + 1);
    total = x & 0xFFFFFFFFFULL;
    if (rate > 0 && total > 0)
        out->duration_ms = (uint32_t)(total * 1000u / rate);
}

static void flac_comment(meta_ctx_t *c, uint32_t off, uint32_t len, media_meta_t *out) {
    meta_scratch_t sc;
    uint32_t end = off + len;
    uint32_t p = off;
    uint32_t count;
    uint8_t b[4];

    meta_scratch_split(c, &sc);
    if (len < 8) return;
    if (!meta_read(c, p, b, 4)) return;
    p += 4;
    {
        uint32_t vlen = meta_u32le(b);
        if (vlen > end - p) return;
        p += vlen;
    }
    if (p + 4 > end || !meta_read(c, p, b, 4)) return;
    count = meta_u32le(b);
    p += 4;
    if (count > 4096) count = 4096;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t clen, n, eq;
        uint8_t kbuf[48];
        uint8_t *text = sc.text;

        if (p + 4 > end || !meta_read(c, p, b, 4)) return;
        clen = meta_u32le(b);
        p += 4;
        if (clen > end - p) return;

        n = clen < sc.text_len ? clen : sc.text_len;
        if (n > 0 && !meta_read(c, p, text, n)) return;
        text[n < sc.text_len ? n : sc.text_len - 1] = '\0';

        eq = 0;
        while (eq < n && text[eq] != '=') eq++;
        if (eq > 0 && eq < n) {
            char value[256];
            uint32_t vn = n - eq - 1;
            uint32_t kc = eq < sizeof kbuf ? eq : (uint32_t)sizeof kbuf;
            memcpy(kbuf, text, kc);
            if (vn >= sizeof value) vn = sizeof value - 1;
            if (vn > 0) memcpy(value, text + eq + 1, vn);
            value[vn] = '\0';
            meta_comment_apply(out, (const char *)kbuf, kc, value);
        }
        p += clen;
    }
}

static void flac_picture(meta_ctx_t *c, uint32_t off, uint32_t len, media_meta_t *out,
                         uint8_t *best_ptype) {
    uint32_t end = off + len;
    uint32_t p = off;
    uint8_t b[8];
    uint8_t mime[32];
    uint32_t mime_len, desc_len, width, height, data_len, ptype;
    uint32_t data_off;
    uint8_t type;

    if (len < 32) return;

    if (!meta_read(c, p, b, 8)) return;
    ptype = meta_u32be(b);
    mime_len = meta_u32be(b + 4);
    p += 8;
    if (mime_len > sizeof mime || mime_len > end - p) return;
    if (mime_len && !meta_read(c, p, mime, mime_len)) return;
    p += mime_len;

    if (p + 4 > end || !meta_read(c, p, b, 4)) return;
    desc_len = meta_u32be(b);
    p += 4;
    if (desc_len > end - p) return;
    p += desc_len;

    if (p + 20 > end || !meta_read(c, p, b, 8)) return;
    width = meta_u32be(b);
    height = meta_u32be(b + 4);
    p += 16; /* width, height, depth, colors */
    if (!meta_read(c, p, b, 4)) return;
    data_len = meta_u32be(b);
    p += 4;
    if (data_len > end - p) return;
    data_off = p;
    if (data_len == 0) return;

    {
        uint8_t magic[8];
        uint32_t n = data_len < sizeof magic ? data_len : (uint32_t)sizeof magic;
        type = MEDIA_ART_OTHER;
        if (meta_read(c, data_off, magic, n)) type = meta_art_type_magic(magic, n);
        if (type == MEDIA_ART_OTHER) type = mime_type(mime, mime_len);
    }

    if (out->art.type != MEDIA_ART_NONE) {
        if (*best_ptype == 3 || ptype != 3) return;
    }
    *best_ptype = (uint8_t)ptype;

    out->art.offset = data_off;
    out->art.size = data_len;
    out->art.type = type;
    out->art.encoding = MEDIA_ART_ENC_RAW;
    out->art.width = (uint16_t)(width > 0xFFFF ? 0 : width);
    out->art.height = (uint16_t)(height > 0xFFFF ? 0 : height);
    out->art.inner_offset = 0;
    out->art.inner_size = 0;
}

media_status_t meta_parse_flac(meta_ctx_t *c, const char *filename, media_meta_t *out) {
    uint8_t hdr[4];
    uint32_t pos = 4;
    uint32_t file = c->src->size;
    media_status_t st = MEDIA_OK;
    bool have_streaminfo = false;
    uint8_t best_ptype = 0xFF;

    (void)filename;
    if (file < 4 || !meta_read(c, 0, hdr, 4)) return MEDIA_ERR_IO;
    if (memcmp(hdr, "fLaC", 4) != 0) return MEDIA_ERR_UNSUPPORTED;
    out->format = MEDIA_FMT_FLAC;

    while (pos + 4 <= file) {
        uint8_t bh[4];
        uint32_t blen, body;
        bool last;
        unsigned type;

        if (!meta_read(c, pos, bh, 4)) { st = MEDIA_ERR_CORRUPT; break; }
        last = (bh[0] & 0x80) != 0;
        type = bh[0] & 0x7F;
        blen = meta_u24be(bh + 1);
        body = pos + 4;
        if (blen > file - body) { st = MEDIA_ERR_CORRUPT; break; }

        switch (type) {
        case 0:
            flac_streaminfo(c, body, blen, out);
            have_streaminfo = true;
            break;
        case 4:
            flac_comment(c, body, blen, out);
            break;
        case 6:
            flac_picture(c, body, blen, out, &best_ptype);
            break;
        default:
            break; /* padding, application, seektable, cuesheet, ... */
        }

        pos = body + blen;
        if (last) break;
    }

    out->audio_offset = pos;
    out->audio_size = pos < file ? file - pos : 0;
    if (!have_streaminfo) st = MEDIA_ERR_CORRUPT;

    if (out->duration_ms > 0 && out->audio_size > 0) {
        out->bitrate_kbps =
            (uint32_t)(((uint64_t)out->audio_size * 8u + out->duration_ms / 2) /
                       out->duration_ms);
    }
    return st;
}
