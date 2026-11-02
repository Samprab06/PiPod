/**
 * @file media_meta_mp3.c
 * @brief MP3: ID3v2.2/2.3/2.4 (+ footer, extended header, unsynchronisation),
 *        ID3v1 fallback and MPEG stream info (Xing/Info/VBRI or CBR estimate).
 */
#include <string.h>

#include "media_meta_internal.h"

/* ----------------------------------------------------------------------- */
/* ID3v1                                                                   */
/* ----------------------------------------------------------------------- */
static void id3v1_fill(meta_ctx_t *c, uint32_t off, media_meta_t *out) {
    uint8_t tag[128];
    char tmp[16];

    if (c->src->size < 128 || off > c->src->size - 128) return;
    if (!meta_read(c, off, tag, sizeof tag)) return;
    if (memcmp(tag, "TAG", 3) != 0) return;

    if (out->title[0] == '\0')
        meta_text(out->title, sizeof out->title, tag + 3, 30, 0);
    if (out->artist[0] == '\0')
        meta_text(out->artist, sizeof out->artist, tag + 33, 30, 0);
    if (out->album[0] == '\0')
        meta_text(out->album, sizeof out->album, tag + 63, 30, 0);
    if (out->year == 0) {
        meta_text(tmp, sizeof tmp, tag + 93, 4, 0);
        out->year = meta_parse_year(tmp);
    }
    if (out->track == 0 && tag[97 + 28] == 0x00 && tag[97 + 29] != 0x00)
        out->track = tag[97 + 29];
    if (out->genre[0] == '\0') {
        const char *name = meta_genre_name(tag[127]);
        if (name) meta_text_cstr(out->genre, sizeof out->genre, name);
    }
}

/* ----------------------------------------------------------------------- */
/* ID3v2                                                                   */
/* ----------------------------------------------------------------------- */
static bool id3_id_valid(const uint8_t *id, unsigned len) {
    for (unsigned i = 0; i < len; i++) {
        uint8_t ch = id[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) return false;
    }
    return true;
}

#define ID_IS(lit) (idlen == (unsigned)(sizeof(lit) - 1) && memcmp(id, lit, sizeof(lit) - 1) == 0)

static void set_text_field(char *dst, size_t cap, const uint8_t *data, uint32_t len) {
    if (dst[0] != '\0' || len < 2) return;
    meta_text(dst, cap, data + 1, len - 1, data[0]);
}

static void set_genre(media_meta_t *out, const uint8_t *data, uint32_t len) {
    char tmp[MEDIA_GENRE_MAX];

    if (out->genre[0] != '\0' || len < 2) return;
    meta_text(tmp, sizeof tmp, data + 1, len - 1, data[0]);
    meta_genre_apply(tmp, sizeof tmp, tmp);
    if (tmp[0] != '\0') memcpy(out->genre, tmp, sizeof tmp);
}

static void set_index(media_meta_t *out, const uint8_t *data, uint32_t len, bool disc) {
    char tmp[24];
    uint16_t total = 0;
    uint16_t value;

    if (len < 2) return;
    meta_text(tmp, sizeof tmp, data + 1, len - 1, data[0]);
    value = meta_parse_index(tmp, &total);
    if (disc) {
        if (out->disc == 0) out->disc = value;
        if (out->disc_total == 0) out->disc_total = total;
    } else {
        if (out->track == 0) out->track = value;
        if (out->track_total == 0) out->track_total = total;
    }
}

static void set_year(media_meta_t *out, const uint8_t *data, uint32_t len) {
    char tmp[24];

    if (out->year != 0 || len < 2) return;
    meta_text(tmp, sizeof tmp, data + 1, len - 1, data[0]);
    out->year = meta_parse_year(tmp);
}

static void apply_text_frame(media_meta_t *out, const uint8_t *id, unsigned idlen,
                             const uint8_t *data, uint32_t len) {
    if (ID_IS("TIT2") || ID_IS("TT2"))
        set_text_field(out->title, sizeof out->title, data, len);
    else if (ID_IS("TPE1") || ID_IS("TP1"))
        set_text_field(out->artist, sizeof out->artist, data, len);
    else if (ID_IS("TALB") || ID_IS("TAL"))
        set_text_field(out->album, sizeof out->album, data, len);
    else if (ID_IS("TPE2") || ID_IS("TP2"))
        set_text_field(out->album_artist, sizeof out->album_artist, data, len);
    else if (ID_IS("TCON") || ID_IS("TCO"))
        set_genre(out, data, len);
    else if (ID_IS("TRCK") || ID_IS("TRK"))
        set_index(out, data, len, false);
    else if (ID_IS("TPOS") || ID_IS("TPA"))
        set_index(out, data, len, true);
    else if (ID_IS("TYER") || ID_IS("TYE") || ID_IS("TDRC"))
        set_year(out, data, len);
}

/**
 * Parse a picture frame (APIC in v2.3/2.4, PIC in v2.2) and consume the whole
 * frame.  Returns false when the frame is malformed (caller stops the tag).
 */
static bool parse_picture(meta_seq_t *seq, bool v22, uint32_t fsize, uint32_t prefix,
                          bool tag_unsync, bool frame_unsync_only,
                          meta_art_info_t *art, uint8_t *best_type) {
    uint32_t data_raw_start = meta_seq_raw(seq);
    uint32_t logical = 0;
    uint32_t raw_total;
    uint8_t mime[16];
    uint32_t mime_len = 0;
    int enc;
    int pic_type;
    bool deunsync = tag_unsync || frame_unsync_only;

    if (fsize < prefix + 2) return false;
    raw_total = fsize - prefix;

    enc = meta_seq_u8(seq);
    if (enc < 0) return false;
    logical++;

    if (v22) {
        for (unsigned i = 0; i < 3; i++) {
            int ch = meta_seq_u8(seq);
            if (ch < 0) return false;
            if (mime_len < sizeof mime) mime[mime_len++] = (uint8_t)ch;
            logical++;
        }
    } else {
        for (;;) {
            int ch = meta_seq_u8(seq);
            if (ch < 0) return false;
            logical++;
            if (ch == 0) break;
            if (mime_len < sizeof mime) mime[mime_len++] = (uint8_t)ch;
            if (logical >= raw_total) return false;
        }
    }

    pic_type = meta_seq_u8(seq);
    if (pic_type < 0) return false;
    logical++;

    for (;;) {
        int ch = meta_seq_u8(seq);
        if (ch < 0) return false;
        logical++;
        if (ch == 0) break;
        if (logical >= raw_total) return false;
    }
    if (logical > raw_total) return false;

    {
        uint32_t img_raw = meta_seq_raw(seq);
        uint32_t hdr_raw = img_raw - data_raw_start;
        uint32_t img_size;

        if (frame_unsync_only) {
            if (hdr_raw > raw_total) return false;
            img_size = raw_total - hdr_raw;
            if (!meta_seq_skip_raw(seq, img_size)) return false;
        } else if (tag_unsync) {
            if (logical > raw_total) return false;
            if (!meta_seq_skip(seq, raw_total - logical)) return false;
            img_size = meta_seq_raw(seq) - img_raw;
            if (seq->pending_ff) img_size += 1; /* trailing inserted 0x00 */
        } else {
            img_size = raw_total - logical;
            if (!meta_seq_skip(seq, img_size)) return false;
        }

        if (img_size == 0 || img_raw + img_size > seq->end) return true;

        if (art) {
            uint8_t magic[8];
            uint32_t n = img_size < sizeof magic ? img_size : (uint32_t)sizeof magic;
            uint8_t type = MEDIA_ART_OTHER;
            if (seq->src->read(seq->src->ctx, img_raw, magic, n) == (int32_t)n)
                type = meta_art_type_magic(magic, n);
            if (type == MEDIA_ART_OTHER) {
                if (v22 && mime_len >= 3 && memcmp(mime, "JPG", 3) == 0)
                    type = MEDIA_ART_JPEG;
                else if (v22 && mime_len >= 3 && memcmp(mime, "PNG", 3) == 0)
                    type = MEDIA_ART_PNG;
                else if (!v22 && mime_len >= 10 && memcmp(mime, "image/jpeg", 10) == 0)
                    type = MEDIA_ART_JPEG;
                else if (!v22 && mime_len >= 9 && memcmp(mime, "image/png", 9) == 0)
                    type = MEDIA_ART_PNG;
            }
            if (type != MEDIA_ART_NONE && (*best_type != 3 || pic_type == 3)) {
                art->offset = img_raw;
                art->size = img_size;
                art->type = type;
                art->enc = deunsync ? MEDIA_ART_ENC_ID3_UNSYNC : MEDIA_ART_ENC_RAW;
                *best_type = (uint8_t)pic_type;
            }
        }
    }
    return true;
}

media_status_t meta_id3v2_parse(meta_ctx_t *c, uint32_t tag_off, uint32_t avail,
                                media_meta_t *out, meta_art_info_t *art,
                                uint32_t *tag_end) {
    meta_scratch_t sc;
    uint8_t hdr[10];
    uint8_t fh[10];
    uint32_t body_off, body_size, body_end, end;
    unsigned ver, flags;
    bool tag_unsync;
    uint32_t hdr_len;
    meta_seq_t seq;
    unsigned guard = 0;
    uint8_t best_type = 0xFF;
    media_status_t st = MEDIA_OK;

    if (tag_end) *tag_end = tag_off;
    if (avail < 10) return MEDIA_ERR_CORRUPT;
    if (!meta_read(c, tag_off, hdr, sizeof hdr)) return MEDIA_ERR_IO;
    if (memcmp(hdr, "ID3", 3) != 0) return MEDIA_ERR_CORRUPT;

    ver = hdr[3];
    flags = hdr[5];
    if (ver < 2 || ver > 4) return MEDIA_ERR_UNSUPPORTED;

    body_size = meta_syncsafe(hdr + 6);
    if (body_size > avail - 10) body_size = avail - 10;
    body_off = tag_off + 10;
    body_end = body_off + body_size;
    end = body_end;
    if (ver == 4 && (flags & 0x10)) {
        if (body_end + 10 <= tag_off + avail) end = body_end + 10;
        else st = MEDIA_ERR_CORRUPT;
    }
    if (tag_end) *tag_end = end;

    if (ver == 2 && (flags & 0x40)) return MEDIA_OK; /* whole-tag compression */

    if (ver >= 3 && (flags & 0x40)) {
        uint8_t ext[4];
        uint32_t skip = 0;
        if (!meta_read(c, body_off, ext, 4)) return MEDIA_ERR_CORRUPT;
        if (ver == 4) {
            skip = meta_syncsafe(ext);
            if (skip < 6 || skip > body_size) skip = 0;
        } else {
            uint32_t sz = meta_u32be(ext);
            if (sz >= 6 && sz + 4 <= body_size) skip = sz + 4;
        }
        body_off += skip;
        body_size -= skip;
        body_end = body_off + body_size;
    }

    tag_unsync = (flags & 0x80) != 0;
    hdr_len = (ver == 2) ? 6 : 10;
    meta_scratch_split(c, &sc);
    if (!meta_seq_init(&seq, c->src, sc.seq, sc.seq_len, body_off, body_end, tag_unsync))
        return MEDIA_ERR_SCRATCH;

    for (;;) {
        uint32_t fsize, prefix = 0;
        bool skip_frame = false, frame_unsync = false;
        unsigned idlen;
        uint32_t raw_before;
        bool clamped = false;
        bool saved_unsync;

        if (++guard > 4096) { st = MEDIA_ERR_CORRUPT; break; }
        if (meta_seq_raw(&seq) + hdr_len > body_end) break;

        for (unsigned i = 0; i < hdr_len; i++) {
            int ch = meta_seq_u8(&seq);
            if (ch < 0) return MEDIA_ERR_CORRUPT;
            fh[i] = (uint8_t)ch;
        }
        idlen = (ver == 2) ? 3 : 4;
        if (!id3_id_valid(fh, idlen)) break; /* padding or garbage */

        if (ver == 2) {
            fsize = meta_u24be(fh + 3);
        } else if (ver == 3) {
            fsize = meta_u32be(fh + 4);
        } else {
            fsize = meta_syncsafe(fh + 4);
        }
        if (ver == 3) {
            uint8_t fmt = fh[9];
            if (fmt & 0x80) skip_frame = true;      /* compressed */
            else if (fmt & 0x40) skip_frame = true; /* encrypted */
            else if (fmt & 0x20) prefix = 1;        /* grouping */
        } else if (ver == 4) {
            uint8_t fmt = fh[9];
            if (fmt & 0x08) skip_frame = true;      /* compressed */
            else if (fmt & 0x04) skip_frame = true; /* encrypted */
            if (fmt & 0x40) prefix += 1;            /* grouping */
            if (fmt & 0x01) prefix += 4;            /* data length indicator */
            frame_unsync = (fmt & 0x02) != 0;
        }

        if (fsize > body_end - meta_seq_raw(&seq)) {
            fsize = body_end - meta_seq_raw(&seq);
            clamped = true;
            st = MEDIA_ERR_CORRUPT;
        }

        if (skip_frame || fsize < prefix) {
            if (!meta_seq_skip(&seq, fsize)) { st = MEDIA_ERR_CORRUPT; break; }
            if (clamped) break;
            continue;
        }

        for (uint32_t i = 0; i < prefix; i++) {
            if (meta_seq_u8(&seq) < 0) return MEDIA_ERR_CORRUPT;
        }

        saved_unsync = seq.unsync;
        if (!tag_unsync && frame_unsync) seq.unsync = true;
        raw_before = meta_seq_raw(&seq);

        if (idlen == 4 ? memcmp(fh, "APIC", 4) == 0 : memcmp(fh, "PIC", 3) == 0) {
            if (!parse_picture(&seq, ver == 2, fsize, prefix, tag_unsync,
                               !tag_unsync && frame_unsync, art, &best_type)) {
                st = MEDIA_ERR_CORRUPT;
                break;
            }
        } else {
            uint32_t want = fsize - prefix;
            uint32_t take = want < sc.text_len ? want : sc.text_len;
            uint32_t got = 0;
            for (; got < take; got++) {
                int ch = meta_seq_u8(&seq);
                if (ch < 0) return MEDIA_ERR_CORRUPT;
                sc.text[got] = (uint8_t)ch;
            }
            apply_text_frame(out, fh, idlen, sc.text, got);
            if (!frame_unsync || tag_unsync) {
                if (!meta_seq_skip(&seq, want - got)) { st = MEDIA_ERR_CORRUPT; break; }
            } else {
                uint32_t raw_used = meta_seq_raw(&seq) - raw_before;
                if (raw_used > want || !meta_seq_skip_raw(&seq, want - raw_used)) {
                    st = MEDIA_ERR_CORRUPT;
                    break;
                }
            }
        }

        seq.unsync = saved_unsync;
        if (!seq.unsync) seq.pending_ff = false; /* frame boundary */
        if (clamped) break;
    }

    return st;
}

/* ----------------------------------------------------------------------- */
/* MPEG audio frames                                                       */
/* ----------------------------------------------------------------------- */
typedef struct {
    uint32_t bitrate;  /* bit/s */
    uint32_t rate;     /* Hz */
    uint32_t frame_len;
    uint32_t samples;  /* samples per frame */
    uint8_t channels;
    uint8_t version;   /* 1, 2, 25 */
    uint8_t layer;     /* 1..3 */
    bool crc;
} mp3_frame_t;

static const uint16_t br_mpeg1_l1[16] = {0, 32, 64, 96, 128, 160, 192, 224,
                                         256, 288, 320, 352, 384, 416, 448, 0};
static const uint16_t br_mpeg1_l2[16] = {0, 32, 48, 56, 64, 80, 96, 112,
                                         128, 160, 192, 224, 256, 320, 384, 0};
static const uint16_t br_mpeg1_l3[16] = {0, 32, 40, 48, 56, 64, 80, 96,
                                         112, 128, 160, 192, 224, 256, 320, 0};
static const uint16_t br_mpeg2_l1[16] = {0, 32, 48, 56, 64, 80, 96, 112,
                                         144, 160, 176, 192, 224, 256, 0, 0};
static const uint16_t br_mpeg2_l23[16] = {0, 8, 16, 24, 32, 40, 48, 56,
                                          64, 80, 96, 112, 128, 144, 160, 0};

static bool mp3_header(const uint8_t *h, mp3_frame_t *f) {
    unsigned ver_bits, layer_bits, br_idx, sr_idx;
    uint32_t rate, samples, bitrate_bps;
    const uint16_t *table;
    bool mpeg1;

    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return false;
    ver_bits = (h[1] >> 3) & 3;
    layer_bits = (h[1] >> 1) & 3;
    if (ver_bits == 1 || layer_bits == 0) return false;
    br_idx = (h[2] >> 4) & 0xF;
    sr_idx = (h[2] >> 2) & 3;
    if (br_idx == 0 || br_idx == 15 || sr_idx == 3) return false;

    mpeg1 = (ver_bits == 3);
    f->version = mpeg1 ? 1 : (ver_bits == 2 ? 2 : 25);
    f->layer = (uint8_t)(4 - layer_bits); /* 1..3 */
    if (f->layer == 1) table = mpeg1 ? br_mpeg1_l1 : br_mpeg2_l1;
    else if (f->layer == 2) table = mpeg1 ? br_mpeg1_l2 : br_mpeg2_l23;
    else table = mpeg1 ? br_mpeg1_l3 : br_mpeg2_l23;
    bitrate_bps = (uint32_t)table[br_idx] * 1000u;

    {
        static const uint16_t rates[3][3] = {
            {44100, 48000, 32000}, /* MPEG1 */
            {22050, 24000, 16000}, /* MPEG2 */
            {11025, 12000, 8000},  /* MPEG2.5 */
        };
        unsigned vi = mpeg1 ? 0 : (f->version == 2 ? 1 : 2);
        rate = rates[vi][sr_idx];
    }

    if (f->layer == 1) samples = 384;
    else if (f->layer == 2) samples = 1152;
    else samples = mpeg1 ? 1152 : 576;

    f->bitrate = bitrate_bps;
    f->rate = rate;
    f->samples = samples;
    f->channels = (((h[3] >> 6) & 3) == 3) ? 1 : 2;
    f->crc = (h[1] & 0x01) == 0;
    f->frame_len = (samples / 8) * bitrate_bps / rate + ((h[2] >> 1) & 1);
    return f->frame_len >= 24;
}

static bool mp3_find_frame(meta_ctx_t *c, uint32_t start, uint32_t end,
                           uint32_t *frame_off, mp3_frame_t *frame) {
    meta_scratch_t sc;
    uint32_t pos, limit;
    uint8_t hdr4[4];

    if (start >= end) return false;
    meta_scratch_split(c, &sc);
    limit = start + 512u * 1024u;
    if (limit > end) limit = end;

    pos = start;
    while (pos + 4 <= limit) {
        uint32_t n, i, got;

        n = limit - pos;
        if (n > sc.aux_len) n = sc.aux_len;
        if (n < 4) break;
        got = (uint32_t)c->src->read(c->src->ctx, pos, sc.aux, n);
        if (got < 4) break;
        for (i = 0; i + 4 <= got; i++) {
            mp3_frame_t f;
            uint32_t off, next;
            if (sc.aux[i] != 0xFF || (sc.aux[i + 1] & 0xE0) != 0xE0) continue;
            if (!mp3_header(sc.aux + i, &f)) continue;
            off = pos + i;
            next = off + f.frame_len;
            if (next > end) continue;
            if (next + 4 <= end) {
                mp3_frame_t dummy;
                if (!meta_read(c, next, hdr4, 4)) continue;
                if (!mp3_header(hdr4, &dummy)) continue;
            }
            *frame_off = off;
            *frame = f;
            return true;
        }
        if (got < n) break;
        pos += got - 3;
    }
    return false;
}

static void mp3_vbr_info(meta_ctx_t *c, const mp3_frame_t *f, uint32_t frame_off,
                         uint32_t end, uint32_t *out_frames, uint32_t *out_bytes,
                         uint32_t *out_skip) {
    uint8_t buf[32];
    uint32_t side;
    uint32_t xoff;

    *out_frames = 0;
    *out_bytes = 0;
    *out_skip = 0;

    if (f->version == 1) side = (f->channels == 1) ? 17 : 32;
    else side = (f->channels == 1) ? 9 : 17;
    xoff = frame_off + 4 + (f->crc ? 2 : 0) + side;
    if (xoff + 8 > end) return;
    if (!meta_read(c, xoff, buf, 8)) return;
    if (memcmp(buf, "Xing", 4) != 0 && memcmp(buf, "Info", 4) != 0) {
        /* VBRI lives at a fixed offset after the header */
        uint32_t voff = frame_off + 4 + 32;
        if (voff + 18 <= end && meta_read(c, voff, buf, 18) && memcmp(buf, "VBRI", 4) == 0) {
            *out_bytes = meta_u32be(buf + 10);
            *out_frames = meta_u32be(buf + 14);
        }
        return;
    }

    {
        uint32_t flags = meta_u32be(buf + 4);
        uint32_t pos = xoff + 8;
        uint32_t frames = 0, bytes = 0;

        if (flags & 0x1) {
            if (!meta_read(c, pos, buf, 4)) return;
            frames = meta_u32be(buf);
            pos += 4;
        }
        if (flags & 0x2) {
            if (!meta_read(c, pos, buf, 4)) return;
            bytes = meta_u32be(buf);
            pos += 4;
        }
        if (flags & 0x4) pos += 100;
        if (flags & 0x8) pos += 4;
        *out_frames = frames;
        *out_bytes = bytes;

        /* LAME extension: 12-bit encoder delay and padding at byte 21 */
        if (pos + 24 <= end && meta_read(c, pos, buf, 24)) {
            static const char *const enc_ids[] = {"LAME", "Lavc", "Lavf", "L3.9"};
            bool known = false;
            for (unsigned i = 0; i < sizeof enc_ids / sizeof enc_ids[0]; i++) {
                if (memcmp(buf, enc_ids[i], 4) == 0) known = true;
            }
            if (known) {
                uint32_t delay = ((uint32_t)buf[21] << 4) | (buf[22] >> 4);
                uint32_t pad = (((uint32_t)buf[22] & 0x0F) << 8) | buf[23];
                if (delay < f->samples * 4 && pad < f->samples * 4)
                    *out_skip = delay + pad;
            }
        }
    }
}

bool meta_mp3_detect(meta_ctx_t *c) {
    meta_scratch_t sc;
    uint32_t limit, pos = 0;

    meta_scratch_split(c, &sc);
    limit = c->src->size < 8192 ? c->src->size : 8192; /* skip leading junk */
    while (pos + 4 <= limit) {
        uint32_t n = limit - pos;
        uint32_t got;
        if (n > sc.aux_len) n = sc.aux_len;
        got = (uint32_t)c->src->read(c->src->ctx, pos, sc.aux, n);
        if (got < 4) break;
        for (uint32_t i = 0; i + 4 <= got; i++) {
            mp3_frame_t f;
            if (sc.aux[i] != 0xFF || (sc.aux[i + 1] & 0xE0) != 0xE0) continue;
            if (mp3_header(sc.aux + i, &f)) return true;
        }
        if (got < n) break;
        pos += got - 3;
    }
    return false;
}

media_status_t meta_parse_mp3(meta_ctx_t *c, const char *filename, media_meta_t *out) {
    meta_art_info_t art;
    uint32_t audio_off = 0, audio_end, tag_end = 0;
    mp3_frame_t frame;
    media_status_t st = MEDIA_OK;
    uint32_t frames = 0, vbr_bytes = 0, skip = 0;
    uint32_t duration = 0, bitrate;

    (void)filename;
    out->format = MEDIA_FMT_MP3;
    memset(&art, 0, sizeof art);

    audio_end = c->src->size;
    if (audio_end >= 128) {
        uint8_t id3v1[3];
        if (meta_read(c, audio_end - 128, id3v1, 3) && memcmp(id3v1, "TAG", 3) == 0)
            audio_end -= 128;
    }
    if (c->src->size >= 128) id3v1_fill(c, c->src->size - 128, out);

    {
        uint8_t hdr[10];
        if (c->src->size >= 10 && meta_read(c, 0, hdr, 10) && memcmp(hdr, "ID3", 3) == 0) {
            media_status_t tag_st = meta_id3v2_parse(c, 0, c->src->size, out, &art, &tag_end);
            if (tag_st == MEDIA_ERR_UNSUPPORTED) tag_end = 0;
            else if (tag_st != MEDIA_OK) st = tag_st;
            audio_off = tag_end;
        }
    }

    if (art.type != MEDIA_ART_NONE) meta_art_take(out, &art, true);

    if (audio_off >= audio_end) {
        out->audio_offset = audio_off;
        out->audio_size = 0;
        return st == MEDIA_OK ? MEDIA_ERR_CORRUPT : st;
    }

    if (!mp3_find_frame(c, audio_off, audio_end, &audio_off, &frame)) {
        out->audio_offset = audio_off;
        out->audio_size = audio_end - audio_off;
        return MEDIA_ERR_CORRUPT;
    }

    mp3_vbr_info(c, &frame, audio_off, audio_end, &frames, &vbr_bytes, &skip);
    (void)vbr_bytes;

    if (frames > 0) {
        uint64_t total = (uint64_t)frames * frame.samples;
        if (total > skip) total -= skip;
        else total = 0;
        duration = (uint32_t)(total * 1000u / frame.rate);
    } else {
        duration = (uint32_t)((uint64_t)(audio_end - audio_off) * 8u * 1000u /
                              frame.bitrate);
        if (duration == 0 && frame.frame_len) {
            uint32_t count = (audio_end - audio_off) / frame.frame_len;
            duration = (uint32_t)((uint64_t)count * frame.samples * 1000u / frame.rate);
        }
    }

    bitrate = frame.bitrate / 1000u;
    if (duration > 0 && audio_end > audio_off) {
        bitrate = (uint32_t)(((uint64_t)(audio_end - audio_off) * 8u + duration / 2) / duration);
    }

    out->sample_rate = frame.rate;
    out->channels = frame.channels;
    out->bits_per_sample = 0;
    out->duration_ms = duration;
    out->bitrate_kbps = bitrate;
    out->audio_offset = audio_off;
    out->audio_size = audio_end - audio_off;
    return st;
}
