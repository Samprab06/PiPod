/**
 * @file media_meta_wav.c
 * @brief RIFF/WAVE: fmt, data, LIST/INFO and an `id3 ` chunk.
 */
#include <string.h>

#include "media_meta_internal.h"

typedef struct {
    uint16_t tag;
    uint16_t channels;
    uint32_t rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits;
    bool valid;
} wav_fmt_t;

static void wav_parse_fmt(meta_ctx_t *c, uint32_t off, uint32_t len, wav_fmt_t *fmt) {
    uint8_t f[40];

    memset(fmt, 0, sizeof *fmt);
    if (len < 16 || len > sizeof f) {
        if (len < 16) return;
        len = sizeof f;
    }
    if (!meta_read(c, off, f, len)) return;
    fmt->tag = meta_u16le(f);
    fmt->channels = meta_u16le(f + 2);
    fmt->rate = meta_u32le(f + 4);
    fmt->byte_rate = meta_u32le(f + 8);
    fmt->block_align = meta_u16le(f + 12);
    fmt->bits = meta_u16le(f + 14);
    if (fmt->tag == 0xFFFE && len >= 40) fmt->tag = meta_u16le(f + 24);
    fmt->valid = fmt->channels > 0 && fmt->rate > 0 && fmt->bits > 0;
}

static bool wav_key_is(const uint8_t *id, const char *lit) {
    return memcmp(id, lit, 4) == 0;
}

static void wav_list_info(meta_ctx_t *c, uint32_t off, uint32_t len, media_meta_t *out) {
    meta_scratch_t sc;
    uint32_t end = off + len;
    uint32_t p = off;
    uint8_t hdr[8];

    meta_scratch_split(c, &sc);
    if (len < 4) return;
    if (!meta_read(c, p, hdr, 4)) return;
    if (memcmp(hdr, "INFO", 4) != 0) return;
    p += 4;

    while (p + 8 <= end) {
        uint32_t clen, n;
        char *text = (char *)sc.text;

        if (!meta_read(c, p, hdr, 8)) return;
        clen = meta_u32le(hdr + 4);
        p += 8;
        if (clen > end - p) return;
        n = clen < sc.text_len ? clen : sc.text_len - 1;
        if (n > 0 && !meta_read(c, p, sc.text, n)) return;
        text[n] = '\0';

        if (wav_key_is(hdr, "INAM")) {
            if (out->title[0] == 0) meta_copy_text(out->title, sizeof out->title, text);
        } else if (wav_key_is(hdr, "IART")) {
            if (out->artist[0] == 0) meta_copy_text(out->artist, sizeof out->artist, text);
        } else if (wav_key_is(hdr, "IPRD")) {
            if (out->album[0] == 0) meta_copy_text(out->album, sizeof out->album, text);
        } else if (wav_key_is(hdr, "IGNR")) {
            if (out->genre[0] == 0) meta_copy_text(out->genre, sizeof out->genre, text);
        } else if (wav_key_is(hdr, "ICRD")) {
            if (out->year == 0) out->year = meta_parse_year(text);
        } else if (wav_key_is(hdr, "ITRK") || wav_key_is(hdr, "IPRT")) {
            uint16_t total = 0;
            uint16_t value = meta_parse_index(text, &total);
            if (out->track == 0) out->track = value;
            if (out->track_total == 0) out->track_total = total;
        }
        p += clen + (clen & 1);
    }
}

static void merge_text(char *dst, size_t cap, const char *src) {
    if (src[0] != '\0') memcpy(dst, src, cap);
}

/* The `id3 ` chunk wins over LIST/INFO for any field it provides. */
static void wav_id3_chunk(meta_ctx_t *c, uint32_t off, uint32_t len, media_meta_t *out) {
    media_meta_t tmp;
    meta_art_info_t art;
    uint32_t tag_end = 0;

    memset(&tmp, 0, sizeof tmp);
    memset(&art, 0, sizeof art);
    if (len < 10) return;
    if (meta_id3v2_parse(c, off, len, &tmp, &art, &tag_end) == MEDIA_ERR_UNSUPPORTED)
        return;

    merge_text(out->title, sizeof out->title, tmp.title);
    merge_text(out->artist, sizeof out->artist, tmp.artist);
    merge_text(out->album, sizeof out->album, tmp.album);
    merge_text(out->album_artist, sizeof out->album_artist, tmp.album_artist);
    merge_text(out->genre, sizeof out->genre, tmp.genre);
    if (tmp.track) out->track = tmp.track;
    if (tmp.track_total) out->track_total = tmp.track_total;
    if (tmp.disc) out->disc = tmp.disc;
    if (tmp.disc_total) out->disc_total = tmp.disc_total;
    if (tmp.year) out->year = tmp.year;

    if (art.type != MEDIA_ART_NONE) {
        out->art.offset = art.offset;
        out->art.size = art.size;
        out->art.type = art.type;
        out->art.encoding = art.enc;
        out->art.width = 0;
        out->art.height = 0;
        out->art.inner_offset = 0;
        out->art.inner_size = 0;
    }
}

media_status_t meta_parse_wav(meta_ctx_t *c, const char *filename, media_meta_t *out) {
    uint8_t hdr[12];
    uint32_t file = c->src->size;
    uint32_t pos = 12;
    wav_fmt_t fmt;
    bool have_data = false;
    media_status_t st = MEDIA_OK;

    (void)filename;
    if (file < 12 || !meta_read(c, 0, hdr, 12)) return MEDIA_ERR_IO;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
        return MEDIA_ERR_UNSUPPORTED;
    out->format = MEDIA_FMT_WAV;
    memset(&fmt, 0, sizeof fmt);

    while (pos + 8 <= file) {
        uint8_t ch[8];
        uint32_t clen, body;
        bool clamped = false;

        if (!meta_read(c, pos, ch, 8)) { st = MEDIA_ERR_CORRUPT; break; }
        clen = meta_u32le(ch + 4);
        body = pos + 8;
        if (clen > file - body) {
            clen = file - body;
            clamped = true;
            st = MEDIA_ERR_CORRUPT;
        }

        if (wav_key_is(ch, "fmt ")) {
            wav_parse_fmt(c, body, clen, &fmt);
        } else if (wav_key_is(ch, "data")) {
            if (!have_data) {
                out->audio_offset = body;
                out->audio_size = clen;
                have_data = true;
            }
        } else if (wav_key_is(ch, "LIST")) {
            wav_list_info(c, body, clen, out);
        } else if (wav_key_is(ch, "id3 ") || wav_key_is(ch, "ID3 ")) {
            wav_id3_chunk(c, body, clen, out);
        }

        if (clamped) break;
        pos = body + clen + (clen & 1u);
    }

    if (fmt.valid) {
        out->sample_rate = fmt.rate;
        out->channels = (uint8_t)(fmt.channels > 255 ? 0 : fmt.channels);
        out->bits_per_sample = (uint8_t)(fmt.bits > 255 ? 0 : fmt.bits);
    }

    if (have_data) {
        uint32_t byte_rate = fmt.byte_rate;
        if (byte_rate == 0 && fmt.valid)
            byte_rate = fmt.rate * (uint32_t)fmt.channels * (uint32_t)fmt.bits / 8u;
        if (byte_rate > 0)
            out->duration_ms = (uint32_t)((uint64_t)out->audio_size * 1000u / byte_rate);
        if (out->duration_ms > 0)
            out->bitrate_kbps =
                (uint32_t)(((uint64_t)out->audio_size * 8u + out->duration_ms / 2) /
                           out->duration_ms);
    } else {
        st = MEDIA_ERR_CORRUPT;
    }
    return st;
}
