/**
 * @file media_meta_ogg.c
 * @brief Ogg Vorbis / Ogg Opus: identification header, comment header
 *        (streamed across pages, no packet buffering), METADATA_BLOCK_PICTURE
 *        (base64) and duration from the last page granule.
 */
#include <string.h>

#include "media_meta_internal.h"

typedef struct {
    const media_source_t *src;
    uint8_t *win;
    uint32_t win_cap;
    uint32_t win_off;
    uint32_t win_len;
    uint32_t win_idx;

    uint32_t file_end;
    uint32_t next_page;
    uint32_t body_off;
    uint32_t body_end;
    uint32_t pos;
    uint8_t nsegs;
    uint8_t segs[255];
    uint16_t seg;
    uint16_t seg_off;
    uint32_t granule;
    bool page_open;
} ogg_stream_t;

typedef struct {
    bool have;
    uint32_t offset;
    uint32_t size;
    uint32_t inner_offset;
    uint32_t inner_size;
    uint8_t type;
    uint16_t width;
    uint16_t height;
} ogg_art_t;

static bool ogg_open_page(ogg_stream_t *st) {
    uint8_t h[27];
    uint8_t segs[255];
    uint8_t n;
    uint32_t body_off, body_len = 0;

    if (st->next_page >= st->file_end || st->file_end - st->next_page < 27)
        return false;
    if (st->src->read(st->src->ctx, st->next_page, h, sizeof h) != (int32_t)sizeof h)
        return false;
    if (memcmp(h, "OggS", 4) != 0 || h[4] != 0) return false;
    n = h[26];
    if (st->next_page + 27 + n > st->file_end) return false;
    if (n > 0 && st->src->read(st->src->ctx, st->next_page + 27, segs, n) != (int32_t)n)
        return false;
    for (unsigned i = 0; i < n; i++) body_len += segs[i];
    body_off = st->next_page + 27 + n;
    if (body_off > st->file_end || body_len > st->file_end - body_off) return false;

    st->nsegs = n;
    memcpy(st->segs, segs, n);
    st->body_off = body_off;
    st->body_end = body_off + body_len;
    st->next_page = body_off + body_len;
    st->pos = body_off;
    st->seg = 0;
    st->seg_off = 0;
    st->win_len = 0;
    st->win_idx = 0;
    st->win_off = body_off;
    st->granule = (uint32_t)meta_u64le(h + 6);
    return true;
}

static int ogg_read(ogg_stream_t *st, uint32_t *raw) {
    for (;;) {
        if (!st->page_open) {
            if (!ogg_open_page(st)) return -1;
            st->page_open = true;
        }
        if (st->seg >= st->nsegs) {
            st->page_open = false;
            continue;
        }
        if (st->segs[st->seg] == 0) { /* empty packet */
            st->seg++;
            st->seg_off = 0;
            continue;
        }
        if (st->win_idx >= st->win_len) {
            uint32_t n = st->body_end - st->pos;
            int32_t got;
            if (n == 0) {
                st->page_open = false;
                continue;
            }
            if (n > st->win_cap) n = st->win_cap;
            got = st->src->read(st->src->ctx, st->pos, st->win, n);
            if (got <= 0) return -1;
            st->win_off = st->pos;
            st->win_len = (uint32_t)got;
            st->win_idx = 0;
        }
        *raw = st->win_off + st->win_idx;
        {
            uint8_t b = st->win[st->win_idx++];
            st->pos++;
            st->seg_off++;
            if (st->seg_off >= st->segs[st->seg]) {
                st->seg++;
                st->seg_off = 0;
            }
            return (int)b;
        }
    }
}

static bool ogg_skip_bytes(ogg_stream_t *st, uint32_t n) {
    uint32_t raw;
    while (n > 0) {
        if (ogg_read(st, &raw) < 0) return false;
        n--;
    }
    return true;
}

/** Skip the rest of the packet at the current position. */
static bool ogg_skip_packet(ogg_stream_t *st) {
    for (;;) {
        uint16_t len;

        if (!st->page_open) {
            if (!ogg_open_page(st)) return false;
            st->page_open = true;
        }
        if (st->seg >= st->nsegs) {
            st->page_open = false;
            continue;
        }
        len = st->segs[st->seg];
        st->pos += (uint32_t)(len - st->seg_off);
        st->seg_off = len;
        st->win_len = 0;
        st->win_idx = 0;
        st->seg++;
        st->seg_off = 0;
        if (len < 255) return true;
    }
}

/** File offset of the next packet's first byte (page headers skipped). */
static uint32_t ogg_audio_pos(ogg_stream_t *st) {
    for (;;) {
        if (!st->page_open) {
            if (!ogg_open_page(st)) return st->file_end;
            st->page_open = true;
        }
        if (st->seg >= st->nsegs) {
            st->page_open = false;
            continue;
        }
        if (st->segs[st->seg] == 0) {
            st->seg++;
            st->seg_off = 0;
            continue;
        }
        return st->pos;
    }
}

/* ----------------------------------------------------------------------- */
/* base64 -> FLAC PICTURE header prefix                                    */
/* ----------------------------------------------------------------------- */
typedef struct {
    uint32_t acc;
    uint8_t nbits;
    uint8_t *out;
    uint32_t cap;
    uint32_t len;
} b64_t;

static void b64_feed(b64_t *b, int ch) {
    int v;
    if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
    else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
    else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
    else if (ch == '+') v = 62;
    else if (ch == '/') v = 63;
    else return;
    b->acc = (b->acc << 6) | (uint32_t)v;
    b->nbits = (uint8_t)(b->nbits + 6);
    if (b->nbits >= 8) {
        b->nbits = (uint8_t)(b->nbits - 8);
        if (b->len < b->cap) b->out[b->len++] = (uint8_t)((b->acc >> b->nbits) & 0xFF);
    }
}

static uint8_t mime_type(const uint8_t *mime, uint32_t len) {
    if (len >= 10 && memcmp(mime, "image/jpeg", 10) == 0) return MEDIA_ART_JPEG;
    if (len >= 9 && memcmp(mime, "image/png", 9) == 0) return MEDIA_ART_PNG;
    return MEDIA_ART_OTHER;
}

/** Read the FLAC PICTURE wrapper out of the decoded base64 prefix. */
static bool picture_header(const uint8_t *p, uint32_t len, ogg_art_t *art) {
    uint32_t ptype, mlen, dlen, q, w, h, dsize;
    uint8_t type;

    if (len < 32) return false;
    ptype = meta_u32be(p);
    mlen = meta_u32be(p + 4);
    if (mlen > 64 || mlen > len - 12) return false;
    q = 8 + mlen;
    dlen = meta_u32be(p + q);
    q += 4;
    if (dlen > len - q) return false;
    q += dlen;
    if (q + 20 > len) return false;
    w = meta_u32be(p + q);
    h = meta_u32be(p + q + 4);
    dsize = meta_u32be(p + q + 16);
    q += 20;

    type = mime_type(p + 8, mlen);
    if (q + 4 <= len) {
        uint8_t sniff = meta_art_type_magic(p + q, len - q);
        if (sniff != MEDIA_ART_OTHER) type = sniff;
    }
    (void)ptype;
    art->inner_offset = q;
    art->inner_size = dsize;
    art->width = w > 0xFFFF ? 0 : (uint16_t)w;
    art->height = h > 0xFFFF ? 0 : (uint16_t)h;
    art->type = type;
    return true;
}

/* ----------------------------------------------------------------------- */
/* comment header                                                          */
/* ----------------------------------------------------------------------- */
static bool key_is_picture(const char *key, uint32_t klen) {
    static const char lit[] = "METADATA_BLOCK_PICTURE";
    if (klen != sizeof lit - 1) return false;
    for (uint32_t i = 0; i < klen; i++) {
        char c = key[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c != lit[i]) return false;
    }
    return true;
}

static bool ogg_comment(ogg_stream_t *st, media_meta_t *out, meta_scratch_t *sc,
                        bool is_opus, ogg_art_t *art) {
    uint8_t magic[8];
    unsigned mlen = is_opus ? 8 : 7;
    uint32_t raw, vlen = 0, count;
    uint32_t budget = 8u * 1024u * 1024u;

    for (unsigned i = 0; i < mlen; i++) {
        int ch = ogg_read(st, &raw);
        if (ch < 0) return false;
        magic[i] = (uint8_t)ch;
    }
    if (is_opus) {
        if (memcmp(magic, "OpusTags", 8) != 0) return false;
    } else {
        if (memcmp(magic, "\x03vorbis", 7) != 0) return false;
    }

    for (unsigned i = 0; i < 4; i++) {
        int ch = ogg_read(st, &raw);
        if (ch < 0) return false;
        vlen |= (uint32_t)ch << (8 * i);
    }
    if (vlen > 1u << 20) return false;
    if (!ogg_skip_bytes(st, vlen)) return false;

    for (unsigned i = 0; i < 4; i++) {
        int ch = ogg_read(st, &raw);
        if (ch < 0) return false;
        if (i == 0) count = (uint32_t)ch;
        else count |= (uint32_t)ch << (8 * i);
    }
    if (count > 4096) count = 4096;

    for (uint32_t c = 0; c < count; c++) {
        uint32_t len = 0, consumed = 0;
        char key[32];
        uint32_t klen = 0;
        bool has_eq = false;
        uint32_t value_len, take;

        for (unsigned i = 0; i < 4; i++) {
            int ch = ogg_read(st, &raw);
            if (ch < 0) return false;
            len |= (uint32_t)ch << (8 * i);
        }
        if (len > budget) return false;
        budget -= len;

        while (consumed < len) {
            int ch;
            if (consumed >= sizeof key) break; /* key far too long */
            ch = ogg_read(st, &raw);
            if (ch < 0) return false;
            consumed++;
            if (ch == '=') {
                has_eq = true;
                break;
            }
            key[klen++] = (char)ch;
        }
        if (consumed > len) return false;
        value_len = len - consumed;

        if (!has_eq) {
            if (!ogg_skip_bytes(st, value_len)) return false;
            continue;
        }

        if (key_is_picture(key, klen)) {
            b64_t dec;
            uint32_t first_raw = 0, last_raw = 0;
            bool first = true, contiguous = true;
            uint32_t sz0 = 0;

            dec.acc = 0;
            dec.nbits = 0;
            dec.out = sc->aux;
            dec.cap = sc->aux_len;
            dec.len = 0;

            for (uint32_t i = 0; i < value_len; i++) {
                int ch = ogg_read(st, &raw);
                if (ch < 0) return false;
                if (first) {
                    first_raw = raw;
                    first = false;
                } else if (raw != last_raw + 1) {
                    contiguous = false;
                }
                last_raw = raw;
                b64_feed(&dec, ch);
            }
            sz0 = value_len;

            art->have = true;
            art->offset = contiguous ? first_raw : 0;
            art->size = contiguous ? sz0 : 0;
            art->type = MEDIA_ART_OTHER;
            if (!picture_header(dec.out, dec.len, art)) {
                /* truncated prefix: keep the base64 reference but nothing else */
                art->inner_offset = 0;
                art->inner_size = 0;
                art->width = 0;
                art->height = 0;
            } else if (!contiguous) {
                /* value spans pages: no single file range (TODO: page-aware
                   art reader); the FLAC PICTURE fields are still reported */
                art->type = MEDIA_ART_OTHER;
            }
        } else {
            take = value_len < sc->text_len - 1 ? value_len : sc->text_len - 1;
            for (uint32_t i = 0; i < take; i++) {
                int ch = ogg_read(st, &raw);
                if (ch < 0) return false;
                sc->text[i] = (uint8_t)ch;
            }
            sc->text[take] = '\0';
            if (!ogg_skip_bytes(st, value_len - take)) return false;
            meta_comment_apply(out, key, klen, (const char *)sc->text);
        }
    }
    return true;
}

/* ----------------------------------------------------------------------- */
/* duration                                                                */
/* ----------------------------------------------------------------------- */
static bool find_last_page(meta_ctx_t *c, uint8_t *buf, uint32_t cap,
                           uint32_t before, uint32_t *page_off) {
    uint32_t size = c->src->size;
    uint32_t base = size > 65536 ? size - 65536 : 0;
    uint32_t end = before;

    if (end > size) end = size;
    if (end < base) return false;
    while (end > base) {
        uint32_t start = (end - base > cap) ? end - cap : base;
        uint32_t n = end - start;
        if (n < 4) break;
        if (!meta_read(c, start, buf, n)) return false;
        for (uint32_t i = n - 4;; i--) {
            if (memcmp(buf + i, "OggS", 4) == 0 && start + i + 27 <= size) {
                *page_off = start + i;
                return true;
            }
            if (i == 0) break;
        }
        if (start == base) break;
        end = start + 3;
    }
    return false;
}

static bool last_granule(meta_ctx_t *c, meta_scratch_t *sc, uint32_t *granule) {
    uint8_t *buf = sc->aux;
    uint32_t cap = sc->aux_len;
    uint32_t before = c->src->size;
    uint8_t h[27];

    for (unsigned tries = 0; tries < 32; tries++) {
        uint32_t page;
        uint64_t g;

        if (!find_last_page(c, buf, cap, before, &page)) return false;
        if (!meta_read(c, page, h, sizeof h)) return false;
        g = meta_u64le(h + 6);
        if (g != 0xFFFFFFFFFFFFFFFFULL) {
            *granule = (uint32_t)g;
            return true;
        }
        before = page;
    }
    return false;
}

/* ----------------------------------------------------------------------- */
/* entry point                                                             */
/* ----------------------------------------------------------------------- */
media_status_t meta_parse_ogg(meta_ctx_t *c, const char *filename, media_meta_t *out) {
    meta_scratch_t sc;
    uint8_t hdr[27];
    uint32_t file = c->src->size;
    uint32_t body_off, body_len;
    uint8_t nsegs;
    uint8_t id[32];
    uint32_t id_len;
    bool is_opus;
    ogg_stream_t st;
    ogg_art_t art;
    uint32_t pre_skip = 0, nominal = 0, input_rate = 0;
    uint32_t granule = 0;

    (void)filename;
    if (file < 27 || !meta_read(c, 0, hdr, 27)) return MEDIA_ERR_IO;
    if (memcmp(hdr, "OggS", 4) != 0) return MEDIA_ERR_UNSUPPORTED;
    nsegs = hdr[26];
    body_off = 27 + nsegs;
    if (body_off > file) return MEDIA_ERR_CORRUPT;
    body_len = 0;
    {
        uint8_t segs[255];
        if (nsegs && !meta_read(c, 27, segs, nsegs)) return MEDIA_ERR_CORRUPT;
        for (unsigned i = 0; i < nsegs; i++) body_len += segs[i];
    }
    if (body_len == 0 || body_off > file || body_len > file - body_off)
        return MEDIA_ERR_CORRUPT;

    id_len = body_len < sizeof id ? body_len : (uint32_t)sizeof id;
    if (!meta_read(c, body_off, id, id_len)) return MEDIA_ERR_CORRUPT;

    if (id_len >= 7 && id[0] == 0x01 && memcmp(id + 1, "vorbis", 6) == 0) {
        is_opus = false;
        out->format = MEDIA_FMT_OGG_VORBIS;
        if (id_len < 28) return MEDIA_ERR_CORRUPT;
        out->channels = id[11];
        out->sample_rate = meta_u32le(id + 12);
        nominal = meta_u32le(id + 20);
        out->bits_per_sample = 0;
    } else if (id_len >= 8 && memcmp(id, "OpusHead", 8) == 0) {
        is_opus = true;
        out->format = MEDIA_FMT_OGG_OPUS;
        if (id_len < 19) return MEDIA_ERR_CORRUPT;
        out->channels = id[9];
        pre_skip = meta_u16le(id + 10);
        input_rate = meta_u32le(id + 12); /* original rate, informational */
        (void)input_rate;
        out->sample_rate = 48000; /* Opus always decodes at 48 kHz */
        out->bits_per_sample = 0;
    } else {
        return MEDIA_ERR_UNSUPPORTED;
    }

    meta_scratch_split(c, &sc);
    memset(&st, 0, sizeof st);
    st.src = c->src;
    st.win = sc.seq;
    st.win_cap = sc.seq_len;
    st.file_end = file;
    st.next_page = 0;
    st.page_open = false;

    memset(&art, 0, sizeof art);
    if (!ogg_skip_packet(&st)) return MEDIA_ERR_CORRUPT;      /* identification */
    if (!ogg_comment(&st, out, &sc, is_opus, &art)) return MEDIA_ERR_CORRUPT;
    if (!is_opus) {
        if (!ogg_skip_packet(&st)) return MEDIA_ERR_CORRUPT; /* setup header */
    }

    out->audio_offset = ogg_audio_pos(&st);
    out->audio_size = file > out->audio_offset ? file - out->audio_offset : 0;

    if (last_granule(c, &sc, &granule)) {
        uint64_t samples = granule;
        uint32_t rate = out->sample_rate ? out->sample_rate : 48000;
        if (is_opus) {
            samples = granule > pre_skip ? (uint64_t)(granule - pre_skip) : 0;
            rate = 48000;
        }
        out->duration_ms = (uint32_t)(samples * 1000u / rate);
    }

    if (out->duration_ms > 0 && out->audio_size > 0) {
        out->bitrate_kbps =
            (uint32_t)(((uint64_t)out->audio_size * 8u + out->duration_ms / 2) /
                       out->duration_ms);
    } else if (!is_opus && nominal > 0) {
        out->bitrate_kbps = nominal / 1000u;
    }

    if (art.have) {
        out->art.offset = art.offset;
        out->art.size = art.size;
        out->art.type = art.type;
        out->art.encoding = MEDIA_ART_ENC_BASE64;
        out->art.width = art.width;
        out->art.height = art.height;
        out->art.inner_offset = art.inner_offset;
        out->art.inner_size = art.inner_size;
    }
    return MEDIA_OK;
}
