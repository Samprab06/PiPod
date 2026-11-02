/**
 * @file media_meta.c
 * @brief Format detection and dispatch for the metadata parser.
 */
#include <string.h>

#include "media_meta_internal.h"

static void title_from_filename(char *dst, size_t cap, const char *filename) {
    const char *base, *dot = NULL;
    size_t len;

    if (!filename || cap == 0) return;
    base = filename;
    for (const char *p = filename; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    len = strlen(base);
    for (const char *p = base; *p; p++) {
        if (*p == '.') dot = p;
    }
    if (dot && dot > base) len = (size_t)(dot - base);
    if (len == 0) return;
    if (len > 0xFFFFFFu) len = 0xFFFFFFu;
    meta_text(dst, cap, (const uint8_t *)base, (uint32_t)len, 3);
}

media_status_t media_meta_parse(const media_source_t *src, const char *filename,
                                media_meta_t *out, uint8_t *scratch, size_t scratch_len) {
    meta_ctx_t c;
    uint8_t hdr[12];
    uint32_t n;
    media_status_t st;

    if (!src || !src->read || !out) return MEDIA_ERR_IO;
    if (!scratch || scratch_len < MEDIA_META_SCRATCH_MIN) return MEDIA_ERR_SCRATCH;
    if (scratch_len > 0xFFFFFFFFu) scratch_len = 0xFFFFFFFFu;

    memset(out, 0, sizeof *out);
    c.src = src;
    c.scratch = scratch;
    c.scratch_len = (uint32_t)scratch_len;

    if (src->size < 4) return MEDIA_ERR_UNSUPPORTED;
    n = src->size < sizeof hdr ? src->size : (uint32_t)sizeof hdr;
    memset(hdr, 0, sizeof hdr);
    if (!meta_read(&c, 0, hdr, n)) return MEDIA_ERR_IO;

    if (memcmp(hdr, "fLaC", 4) == 0) {
        st = meta_parse_flac(&c, filename, out);
    } else if (memcmp(hdr, "RIFF", 4) == 0 && n >= 12 &&
               memcmp(hdr + 8, "WAVE", 4) == 0) {
        st = meta_parse_wav(&c, filename, out);
    } else if (memcmp(hdr, "OggS", 4) == 0) {
        st = meta_parse_ogg(&c, filename, out);
    } else if (memcmp(hdr, "ID3", 3) == 0 || meta_mp3_detect(&c)) {
        st = meta_parse_mp3(&c, filename, out);
    } else {
        return MEDIA_ERR_UNSUPPORTED;
    }

    if (st == MEDIA_ERR_UNSUPPORTED || st == MEDIA_ERR_IO) return st;

    if (out->title[0] == '\0') title_from_filename(out->title, sizeof out->title, filename);
    return st;
}

const char *media_format_name(media_format_t fmt) {
    switch (fmt) {
    case MEDIA_FMT_MP3:
        return "MP3";
    case MEDIA_FMT_FLAC:
        return "FLAC";
    case MEDIA_FMT_WAV:
        return "WAV";
    case MEDIA_FMT_OGG_VORBIS:
        return "Ogg Vorbis";
    case MEDIA_FMT_OGG_OPUS:
        return "Ogg Opus";
    case MEDIA_FMT_UNKNOWN:
    default:
        return "unknown";
    }
}
