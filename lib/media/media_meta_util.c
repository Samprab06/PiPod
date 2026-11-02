/**
 * @file media_meta_util.c
 * @brief Small allocation-free helpers: scratch split, byte order, tag text
 *        decoding (ISO-8859-1 / UTF-16 / UTF-8 -> UTF-8), number parsing,
 *        genre names, art magic and the de-unsynchronising byte reader.
 */
#include <string.h>

#include "media_meta_internal.h"

/* ----------------------------------------------------------------------- */
/* scratch + byte order                                                    */
/* ----------------------------------------------------------------------- */
void meta_scratch_split(meta_ctx_t *c, meta_scratch_t *s) {
    uint32_t total = c->scratch_len;
    uint32_t seq = total / 4;
    uint32_t text = total / 2;

    if (seq > 1024) seq = 1024;
    if (seq < 256) seq = 256;
    if (text > 1024) text = 1024;
    if (text < 512) text = 512;
    if (seq + text > total) {
        seq = total / 2;
        text = total - seq;
    }
    s->seq = c->scratch;
    s->seq_len = seq;
    s->text = c->scratch + seq;
    s->text_len = text;
    s->aux = c->scratch + seq + text;
    s->aux_len = total - seq - text;
}

bool meta_read(meta_ctx_t *c, uint32_t off, void *dst, uint32_t len) {
    if (len == 0) return true;
    if (off > c->src->size || len > c->src->size - off) return false;
    return c->src->read(c->src->ctx, off, dst, len) == (int32_t)len;
}

uint16_t meta_u16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

uint32_t meta_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

uint32_t meta_u32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

uint32_t meta_u24be(const uint8_t *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

uint64_t meta_u64be(const uint8_t *p) {
    return ((uint64_t)meta_u32be(p) << 32) | meta_u32be(p + 4);
}

uint64_t meta_u64le(const uint8_t *p) {
    return (uint64_t)meta_u32le(p) | ((uint64_t)meta_u32le(p + 4) << 32);
}

uint32_t meta_syncsafe(const uint8_t *p) {
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7) | (uint32_t)(p[3] & 0x7F);
}

/* ----------------------------------------------------------------------- */
/* UTF-8 output                                                            */
/* ----------------------------------------------------------------------- */
typedef struct {
    char *dst;
    size_t cap; /* including the terminating NUL */
    size_t used;
    bool full;
} utf8_out_t;

static void out_cp(utf8_out_t *o, uint32_t cp) {
    uint8_t tmp[4] = {0, 0, 0, 0};
    size_t n;

    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        tmp[0] = (uint8_t)cp;
        n = 1;
    } else if (cp < 0x800) {
        tmp[0] = (uint8_t)(0xC0 | (cp >> 6));
        tmp[1] = (uint8_t)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        tmp[0] = (uint8_t)(0xE0 | (cp >> 12));
        tmp[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (uint8_t)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        tmp[0] = (uint8_t)(0xF0 | (cp >> 18));
        tmp[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        tmp[3] = (uint8_t)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (o->used + n >= o->cap) {
        o->full = true; /* truncate on a character boundary, stop converting */
        return;
    }
    memcpy(o->dst + o->used, tmp, n);
    o->used += n;
}

static void out_byte(utf8_out_t *o, uint8_t b) { out_cp(o, b); }

static void out_finish(utf8_out_t *o) {
    if (o->cap == 0) return;
    o->dst[o->used] = '\0';
}

/* Decode one UTF-8 sequence; returns bytes consumed (>=1) and sets *cp. */
static uint32_t utf8_next(const uint8_t *s, uint32_t len, uint32_t *cp) {
    uint8_t b = s[0];

    if (b < 0x80) {
        *cp = b;
        return 1;
    }
    if ((b & 0xE0) == 0xC0) {
        if (len < 2 || (s[1] & 0xC0) != 0x80) goto bad;
        *cp = ((uint32_t)(b & 0x1F) << 6) | (s[1] & 0x3F);
        if (*cp < 0x80) goto bad;
        return 2;
    }
    if ((b & 0xF0) == 0xE0) {
        if (len < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) goto bad;
        *cp = ((uint32_t)(b & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) |
              (s[2] & 0x3F);
        if (*cp < 0x800) goto bad;
        return 3;
    }
    if ((b & 0xF8) == 0xF0) {
        if (len < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 ||
            (s[3] & 0xC0) != 0x80)
            goto bad;
        *cp = ((uint32_t)(b & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
              ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        if (*cp < 0x10000 || *cp > 0x10FFFF) goto bad;
        return 4;
    }
bad:
    *cp = 0xFFFD;
    return 1;
}

void meta_text(char *dst, size_t cap, const uint8_t *src, uint32_t len, uint8_t enc) {
    utf8_out_t o;

    if (!dst || cap == 0) return;
    o.dst = dst;
    o.cap = cap;
    o.used = 0;
    o.full = false;

    if (enc == 0) {
        /* ISO-8859-1 */
        while (len > 0 && (src[len - 1] == 0x00 || src[len - 1] == ' ')) len--;
        for (uint32_t i = 0; i < len && !o.full; i++) {
            if (src[i] == 0x00) break;
            out_byte(&o, src[i]);
        }
    } else if (enc == 3) {
        /* UTF-8 */
        while (len > 0 && (src[len - 1] == 0x00 || src[len - 1] == ' ')) len--;
        if (len >= 3 && src[0] == 0xEF && src[1] == 0xBB && src[2] == 0xBF) {
            src += 3;
            len -= 3;
        }
        uint32_t i = 0;
        while (i < len && !o.full) {
            if (src[i] == 0x00) break;
            uint32_t cp;
            i += utf8_next(src + i, len - i, &cp);
            out_cp(&o, cp);
        }
    } else {
        /* UTF-16, enc 1 = BOM, enc 2 = big endian without BOM */
        bool be;
        if (enc == 2) {
            be = true;
        } else if (len >= 2 && src[0] == 0xFF && src[1] == 0xFE) {
            be = false;
            src += 2;
            len -= 2;
        } else if (len >= 2 && src[0] == 0xFE && src[1] == 0xFF) {
            be = true;
            src += 2;
            len -= 2;
        } else {
            be = false;
        }
        len &= ~(uint32_t)1;
        while (len >= 2) {
            uint16_t u = be ? (uint16_t)((src[len - 2] << 8) | src[len - 1])
                            : (uint16_t)(src[len - 2] | (src[len - 1] << 8));
            if (u != 0x0000 && u != 0x0020) break;
            len -= 2;
        }
        uint32_t i = 0;
        while (i + 1 < len && !o.full) {
            uint16_t u = be ? (uint16_t)((src[i] << 8) | src[i + 1])
                            : (uint16_t)(src[i] | (src[i + 1] << 8));
            i += 2;
            if (u == 0x0000) break;
            uint32_t cp = u;
            if (u >= 0xD800 && u <= 0xDBFF) {
                if (i + 1 < len) {
                    uint16_t lo = be ? (uint16_t)((src[i] << 8) | src[i + 1])
                                     : (uint16_t)(src[i] | (src[i + 1] << 8));
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000u + (((uint32_t)(u - 0xD800)) << 10) +
                             (uint32_t)(lo - 0xDC00);
                        i += 2;
                    } else {
                        cp = 0xFFFD;
                    }
                } else {
                    cp = 0xFFFD;
                }
            } else if (u >= 0xDC00 && u <= 0xDFFF) {
                cp = 0xFFFD;
            }
            out_cp(&o, cp);
        }
    }
    out_finish(&o);
}

void meta_text_cstr(char *dst, size_t cap, const char *src) {
    meta_text(dst, cap, (const uint8_t *)src, src ? (uint32_t)strlen(src) : 0, 3);
}

/* ----------------------------------------------------------------------- */
/* numbers, genre, art magic                                               */
/* ----------------------------------------------------------------------- */
static uint16_t parse_uint(const char **sp, uint16_t limit) {
    const char *s = *sp;
    uint32_t v = 0;
    unsigned digits = 0;

    while (*s == ' ' || *s == '\t') s++;
    while (*s >= '0' && *s <= '9') {
        if (v <= 0xFFFF) v = v * 10 + (uint32_t)(*s - '0');
        s++;
        if (++digits > 5) break;
    }
    *sp = s;
    if (digits == 0) return 0;
    if (v > limit) v = limit;
    return (uint16_t)v;
}

uint16_t meta_parse_index(const char *s, uint16_t *total) {
    uint16_t first, second = 0;

    if (total) *total = 0;
    if (!s) return 0;
    first = parse_uint(&s, 9999);
    if (*s == '/') {
        s++;
        second = parse_uint(&s, 9999);
    }
    if (total) *total = second;
    return first;
}

uint16_t meta_parse_year(const char *s) {
    uint16_t v = 0;
    unsigned digits = 0;

    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    while (digits < 4 && s[digits] >= '0' && s[digits] <= '9') digits++;
    if (digits < 4) return 0;
    for (unsigned i = 0; i < 4; i++) v = (uint16_t)(v * 10 + (s[i] - '0'));
    return v;
}

/* Standard ID3v1 genres 0..79. */
static const char *const genre_names[] = {
    "Blues",        "Classic Rock", "Country",        "Dance",
    "Disco",        "Funk",         "Grunge",         "Hip-Hop",
    "Jazz",         "Metal",        "New Age",        "Oldies",
    "Other",        "Pop",          "R&B",            "Rap",
    "Reggae",       "Rock",         "Techno",         "Industrial",
    "Alternative",  "Ska",          "Death Metal",    "Pranks",
    "Soundtrack",   "Euro-Techno",  "Ambient",        "Trip-Hop",
    "Vocal",        "Jazz+Funk",    "Fusion",         "Trance",
    "Classical",    "Instrumental", "Acid",           "House",
    "Game",         "Sound Clip",   "Gospel",         "Noise",
    "AlternRock",   "Bass",         "Soul",           "Punk",
    "Space",        "Meditative",   "Instrumental Pop", "Instrumental Rock",
    "Ethnic",       "Gothic",       "Darkwave",       "Techno-Industrial",
    "Electronic",   "Pop-Folk",     "Eurodance",      "Dream",
    "Southern Rock", "Comedy",      "Cult",           "Gangsta",
    "Top 40",       "Christian Rap", "Pop/Funk",      "Jungle",
    "Native American", "Cabaret",   "New Wave",       "Psychedelic",
    "Rave",         "Showtunes",    "Trailer",        "Lo-Fi",
    "Tribal",       "Acid Punk",    "Acid Jazz",      "Polka",
    "Retro",        "Musical",      "Rock & Roll",    "Hard Rock",
};

const char *meta_genre_name(unsigned index) {
    if (index >= sizeof(genre_names) / sizeof(genre_names[0])) return NULL;
    return genre_names[index];
}

void meta_genre_apply(char *genre, size_t cap, const char *text) {
    const char *s = text;
    const char *name;
    unsigned idx = 0, digits = 0;

    while (*s == ' ') s++;
    if (*s == '(') {
        s++;
        while (*s >= '0' && *s <= '9' && digits < 3) {
            idx = idx * 10 + (unsigned)(*s - '0');
            digits++;
            s++;
        }
        if (digits == 0 || *s != ')' ) return;
        s++;
        while (*s == ' ') s++;
        if (*s != '\0') return; /* "(13)Pop" - keep the text as written */
    } else {
        while (*s >= '0' && *s <= '9' && digits < 3) {
            idx = idx * 10 + (unsigned)(*s - '0');
            digits++;
            s++;
        }
        if (digits == 0 || *s != '\0') return;
    }
    name = meta_genre_name(idx);
    if (!name) return;
    meta_text_cstr(genre, cap, name);
}

uint8_t meta_art_type_magic(const uint8_t *p, uint32_t len) {
    if (len >= 2 && p[0] == 0xFF && p[1] == 0xD8) return MEDIA_ART_JPEG;
    if (len >= 4 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G')
        return MEDIA_ART_PNG;
    return MEDIA_ART_OTHER;
}

/* ----------------------------------------------------------------------- */
/* meta_seq_t                                                              */
/* ----------------------------------------------------------------------- */
bool meta_seq_init(meta_seq_t *s, const media_source_t *src, uint8_t *buf, uint32_t cap,
                   uint32_t off, uint32_t end, bool unsync) {
    if (!s || !buf || cap == 0) return false;
    s->src = src;
    s->buf = buf;
    s->cap = cap;
    s->start = off;
    s->len = 0;
    s->idx = 0;
    s->end = end;
    s->unsync = unsync;
    s->pending_ff = false;
    s->io_error = false;
    return true;
}

static bool seq_fill(meta_seq_t *s) {
    uint32_t pos = s->start + s->idx;
    uint32_t n;
    int32_t got;

    if (pos >= s->end) {
        s->start = pos;
        s->len = 0;
        s->idx = 0;
        return false;
    }
    n = s->end - pos;
    if (n > s->cap) n = s->cap;
    got = s->src->read(s->src->ctx, pos, s->buf, n);
    if (got <= 0) {
        s->start = pos;
        s->len = 0;
        s->idx = 0;
        s->io_error = true;
        return false;
    }
    s->start = pos;
    s->len = (uint32_t)got;
    s->idx = 0;
    return true;
}

int meta_seq_u8(meta_seq_t *s) {
    for (;;) {
        uint8_t b;
        if (s->idx >= s->len) {
            if (!seq_fill(s)) return -1;
        }
        b = s->buf[s->idx++];
        if (s->pending_ff) {
            s->pending_ff = false;
            if (b == 0x00) continue; /* inserted byte: drop it */
        }
        if (s->unsync && b == 0xFF) s->pending_ff = true;
        return (int)b;
    }
}

bool meta_seq_skip(meta_seq_t *s, uint32_t n) {
    if (n == 0) return true;
    if (!s->unsync) {
        uint32_t raw = s->start + s->idx;
        if (raw > s->end || n > s->end - raw) return false;
        s->start = raw + n;
        s->len = 0;
        s->idx = 0;
        return true;
    }
    while (n > 0) {
        if (meta_seq_u8(s) < 0) return false;
        n--;
    }
    return true;
}

bool meta_seq_skip_raw(meta_seq_t *s, uint32_t n) {
    uint32_t raw = s->start + s->idx;

    if (raw > s->end || n > s->end - raw) return false;
    s->start = raw + n;
    s->len = 0;
    s->idx = 0;
    s->pending_ff = false;
    return true;
}

uint32_t meta_seq_raw(const meta_seq_t *s) { return s->start + s->idx; }

bool meta_seq_at_end(const meta_seq_t *s) {
    return s->idx >= s->len && s->start + s->idx >= s->end;
}

void meta_copy_text(char *dst, size_t cap, const char *src) {
    meta_text(dst, cap, (const uint8_t *)src, src ? (uint32_t)strlen(src) : 0, 3);
}

static bool key_is(const char *key, uint32_t klen, const char *lit) {
    size_t n = strlen(lit);

    if (klen != n) return false;
    for (size_t i = 0; i < n; i++) {
        char c = key[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c != lit[i]) return false;
    }
    return true;
}

void meta_comment_apply(media_meta_t *out, const char *key, uint32_t klen,
                        const char *value) {
    uint16_t total = 0;
    uint16_t number;

    if (key_is(key, klen, "TITLE")) {
        if (out->title[0] == 0) meta_copy_text(out->title, sizeof out->title, value);
    } else if (key_is(key, klen, "ARTIST")) {
        if (out->artist[0] == 0) meta_copy_text(out->artist, sizeof out->artist, value);
    } else if (key_is(key, klen, "ALBUM")) {
        if (out->album[0] == 0) meta_copy_text(out->album, sizeof out->album, value);
    } else if (key_is(key, klen, "ALBUMARTIST")) {
        if (out->album_artist[0] == 0)
            meta_copy_text(out->album_artist, sizeof out->album_artist, value);
    } else if (key_is(key, klen, "GENRE")) {
        if (out->genre[0] == 0) meta_copy_text(out->genre, sizeof out->genre, value);
    } else if (key_is(key, klen, "TRACKNUMBER")) {
        number = meta_parse_index(value, &total);
        if (out->track == 0) out->track = number;
        if (out->track_total == 0) out->track_total = total;
    } else if (key_is(key, klen, "TRACKTOTAL") || key_is(key, klen, "TOTALTRACKS")) {
        number = meta_parse_index(value, NULL);
        if (out->track_total == 0) out->track_total = number;
    } else if (key_is(key, klen, "DISCNUMBER")) {
        number = meta_parse_index(value, &total);
        if (out->disc == 0) out->disc = number;
        if (out->disc_total == 0) out->disc_total = total;
    } else if (key_is(key, klen, "DISCTOTAL") || key_is(key, klen, "TOTALDISCS")) {
        number = meta_parse_index(value, NULL);
        if (out->disc_total == 0) out->disc_total = number;
    } else if (key_is(key, klen, "DATE") || key_is(key, klen, "YEAR")) {
        if (out->year == 0) out->year = meta_parse_year(value);
    }
}

void meta_art_take(media_meta_t *out, const meta_art_info_t *art, bool prefer) {
    if (!art || art->type == MEDIA_ART_NONE || art->size == 0) return;
    if (!prefer && out->art.type != MEDIA_ART_NONE) return;
    out->art.offset = art->offset;
    out->art.size = art->size;
    out->art.type = art->type;
    out->art.encoding = art->enc;
    out->art.width = 0;
    out->art.height = 0;
    out->art.inner_offset = 0;
    out->art.inner_size = 0;
}
