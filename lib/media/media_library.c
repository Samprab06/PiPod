/**
 * @file media_library.c
 * @brief Scan the PPAK pack once, cache everything in the flash cache region,
 *        then serve the UI from a small RAM index.
 *
 * See media_library.h for the cache layout and the RAM budget. All strings
 * live in the cache and are read through media_cache_ptr(); the RAM index
 * only holds uint16 order vectors plus the per-track cache offsets.
 */
#include "media_library.h"

#include <stdio.h>
#include <string.h>

#include "flash_layout.h"
#include "media_art.h"
#include "media_cache.h"
#include "media_meta.h"
#include "media_pack.h"

/* ------------------------------------------------------------------------- */
/* On-cache layout                                                           */
/* ------------------------------------------------------------------------- */

#define LIB_USER_MAGIC0 'P'
#define LIB_USER_MAGIC1 'P'
#define LIB_USER_MAGIC2 'L'
#define LIB_USER_MAGIC3 'B'
#define LIB_ROOT_MAGIC  "PPLR"
#define LIB_LAYOUT_VERSION 1u

#define LIB_ROOT_SIZE      64u
#define LIB_TRACK_BLOCK    160u /**< fixed track record, includes title+artist */
#define LIB_ARTIST_REC     56u  /**< 48-byte name + album range */
#define LIB_ALBUM_REC      64u  /**< 48-byte name + artist/song range/art */
#define LIB_TRACK_TITLE_OFF  48u
#define LIB_TRACK_ARTIST_OFF 112u

/** Cache bytes kept free for the index written after all track blocks/covers. */
#define LIB_INDEX_RESERVE  (32u * 1024u)

/* Track block fields (little-endian). */
#define TB_FILE         0u  /* u16 pack entry index */
#define TB_ALBUM        2u  /* u16 album index */
#define TB_DISC         4u  /* u16 */
#define TB_DISC_TOTAL   6u  /* u16 */
#define TB_TRACK        8u  /* u16 */
#define TB_TRACK_TOTAL  10u /* u16 */
#define TB_DURATION     12u /* u32 ms */
#define TB_SAMPLE_RATE  16u /* u32 Hz */
#define TB_BITRATE      20u /* u32 kbps */
#define TB_AUDIO_OFF    24u /* u32 file offset */
#define TB_AUDIO_SIZE   28u /* u32 bytes */
#define TB_FORMAT       32u /* u8 media_format_t */
#define TB_CHANNELS     33u /* u8 */
#define TB_BITS         34u /* u8 bits per sample, 0 for lossy */

/* Album record fields. */
#define AR_NAME         0u  /* char[48] */
#define AR_ARTIST       48u /* u16 artist index */
#define AR_SONG_FIRST   50u /* u16 into order_track_album */
#define AR_SONG_COUNT   52u /* u16 */
#define AR_YEAR         54u /* u16 */
#define AR_ART_OFF      56u /* u32 cache offset, 0 = none */
#define AR_ART_STATE    60u /* u8 media_album_art_state_t */

/* Artist record fields. */
#define SR_NAME         0u  /* char[48] */
#define SR_ALBUM_FIRST  48u /* u16 into order_album_artist */
#define SR_ALBUM_COUNT  50u /* u16 */

/* Root record fields. */
#define ROOT_OFF_MAGIC        0u
#define ROOT_OFF_VERSION      4u
#define ROOT_OFF_SIZE         6u
#define ROOT_OFF_TRACKS       8u
#define ROOT_OFF_ALBUMS       10u
#define ROOT_OFF_ARTISTS      12u
#define ROOT_OFF_PARSED       14u
#define ROOT_OFF_SKIPPED      16u
#define ROOT_OFF_TRACK_TABLE  20u
#define ROOT_OFF_ARTIST_REC   24u
#define ROOT_OFF_ALBUM_REC    28u
#define ROOT_OFF_ORDER_ART    32u
#define ROOT_OFF_ORDER_ALB    36u
#define ROOT_OFF_ORDER_ALBA   40u
#define ROOT_OFF_ORDER_TITLE  44u
#define ROOT_OFF_ORDER_ALBUM  48u
#define ROOT_OFF_CACHE_BYTES  52u
#define ROOT_OFF_TRACK_BLOCK  56u
#define ROOT_OFF_ARTIST_REC_SZ 60u

/** Album cover state while scanning (the cached enum is media_album_art_state_t). */
#define ART_STATE_UNTRIED     0xFFu

/* ------------------------------------------------------------------------- */
/* Little-endian helpers                                                     */
/* ------------------------------------------------------------------------- */

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* ------------------------------------------------------------------------- */
/* String helpers (ASCII case folding, "The " sort prefix)                   */
/* ------------------------------------------------------------------------- */

static char lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int ci_cmp(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        const unsigned char ca = (unsigned char)lower_ascii(*a);
        const unsigned char cb = (unsigned char)lower_ascii(*b);
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
        a++;
        b++;
    }
    if (*a != '\0') {
        return 1;
    }
    if (*b != '\0') {
        return -1;
    }
    return 0;
}

/** Sorting key: skip a leading "The " (case-insensitive). */
static const char *sort_key(const char *s) {
    const char c0 = lower_ascii(s[0]);
    const char c1 = lower_ascii(s[1]);
    const char c2 = lower_ascii(s[2]);
    if (c0 == 't' && c1 == 'h' && c2 == 'e' && s[3] == ' ') {
        return s + 4;
    }
    return s;
}

static int name_cmp(const char *a, const char *b) {
    return ci_cmp(sort_key(a), sort_key(b));
}

static bool name_eq_ci(const char *a, const char *b) {
    return ci_cmp(a, b) == 0;
}

static uint32_t name_hash(const char *a, const char *b) {
    uint32_t h = 2166136261u;
    const char *parts[2];
    parts[0] = a;
    parts[1] = b;
    for (int p = 0; p < 2; p++) {
        const char *s = parts[p];
        while (*s != '\0') {
            h ^= (uint8_t)lower_ascii(*s++);
            h *= 16777619u;
        }
        h ^= 0x9Eu;
        h *= 16777619u;
    }
    return h;
}

/** Copy a NUL-terminated string from flash into a fixed field, truncating. */
static void copy_name(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    while (i + 1u < cap && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/** Copy a fixed-width NUL-terminated field from flash, respecting its size. */
static void copy_cache_str(char *dst, size_t cap, const char *src, size_t field) {
    size_t i = 0;
    while (i + 1u < cap && i < field && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* ------------------------------------------------------------------------- */
/* Scan working set (transient, static: no malloc after init)                */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t  scratch[4096];
    uint32_t art_words[MEDIA_ART_PIXELS / 2u]; /* 4-byte aligned RGB565 target */
    uint32_t art_work[MEDIA_ART_WORK_SIZE / 4u];
    char     title[MEDIA_LIB_MAX_TRACKS][MEDIA_TITLE_MAX];
    uint16_t track_album[MEDIA_LIB_MAX_TRACKS];
    uint16_t track_disc[MEDIA_LIB_MAX_TRACKS];
    uint16_t track_no[MEDIA_LIB_MAX_TRACKS];
    char     album_name[MEDIA_LIB_MAX_ALBUMS][MEDIA_NAME_MAX];
    uint16_t album_artist[MEDIA_LIB_MAX_ALBUMS];
    uint32_t album_art_off[MEDIA_LIB_MAX_ALBUMS];
    uint8_t  album_art_state[MEDIA_LIB_MAX_ALBUMS];
    uint16_t album_song_first[MEDIA_LIB_MAX_ALBUMS];
    uint16_t album_song_count[MEDIA_LIB_MAX_ALBUMS];
    char     artist_name[MEDIA_LIB_MAX_ARTISTS][MEDIA_NAME_MAX];
    uint16_t artist_album_first[MEDIA_LIB_MAX_ARTISTS];
    uint16_t artist_album_count[MEDIA_LIB_MAX_ARTISTS];
} media_scan_work_t;

static media_scan_work_t s_work;
static const media_scan_stats_t s_zero_stats;

/* ------------------------------------------------------------------------- */
/* Sorting                                                                   */
/* ------------------------------------------------------------------------- */

static int cmp_artist_ids(uint16_t a, uint16_t b) {
    const int c = name_cmp(s_work.artist_name[a], s_work.artist_name[b]);
    if (c != 0) {
        return c;
    }
    return (a < b) ? -1 : (a > b ? 1 : 0);
}

static int cmp_album_ids(uint16_t a, uint16_t b) {
    int c = name_cmp(s_work.album_name[a], s_work.album_name[b]);
    if (c != 0) {
        return c;
    }
    c = name_cmp(s_work.artist_name[s_work.album_artist[a]],
                 s_work.artist_name[s_work.album_artist[b]]);
    if (c != 0) {
        return c;
    }
    return (a < b) ? -1 : (a > b ? 1 : 0);
}

static int cmp_album_artist_ids(uint16_t a, uint16_t b) {
    if (s_work.album_artist[a] != s_work.album_artist[b]) {
        return s_work.album_artist[a] < s_work.album_artist[b] ? -1 : 1;
    }
    const int c = name_cmp(s_work.album_name[a], s_work.album_name[b]);
    if (c != 0) {
        return c;
    }
    return (a < b) ? -1 : (a > b ? 1 : 0);
}

static uint16_t track_disc_key(uint16_t index) {
    return s_work.track_disc[index] != 0u ? s_work.track_disc[index] : 0xFFFFu;
}

static uint16_t track_no_key(uint16_t index) {
    return s_work.track_no[index] != 0u ? s_work.track_no[index] : 0xFFFFu;
}

static int cmp_track_title(uint16_t a, uint16_t b) {
    const int c = name_cmp(s_work.title[a], s_work.title[b]);
    if (c != 0) {
        return c;
    }
    return (a < b) ? -1 : (a > b ? 1 : 0);
}

static int cmp_track_album(uint16_t a, uint16_t b) {
    if (s_work.track_album[a] != s_work.track_album[b]) {
        return s_work.track_album[a] < s_work.track_album[b] ? -1 : 1;
    }
    if (track_disc_key(a) != track_disc_key(b)) {
        return track_disc_key(a) < track_disc_key(b) ? -1 : 1;
    }
    if (track_no_key(a) != track_no_key(b)) {
        return track_no_key(a) < track_no_key(b) ? -1 : 1;
    }
    const int c = name_cmp(s_work.title[a], s_work.title[b]);
    if (c != 0) {
        return c;
    }
    return (a < b) ? -1 : (a > b ? 1 : 0);
}

/** Stable insertion sort; n is at most 512, so O(n^2) is cheap. */
static void sort_ids(uint16_t *v, uint16_t n, int (*cmp)(uint16_t, uint16_t)) {
    for (uint16_t i = 1; i < n; i++) {
        const uint16_t value = v[i];
        uint16_t j = i;
        while (j > 0u && cmp(v[j - 1u], value) > 0) {
            v[j] = v[j - 1u];
            j--;
        }
        v[j] = value;
    }
}

/* ------------------------------------------------------------------------- */
/* Grouping                                                                  */
/* ------------------------------------------------------------------------- */

static int find_artist(const char *name, uint16_t count) {
    for (uint16_t i = 0; i < count; i++) {
        if (name_eq_ci(s_work.artist_name[i], name)) {
            return (int)i;
        }
    }
    return -1;
}

static int find_album(uint16_t artist, const char *name, uint16_t count) {
    for (uint16_t i = 0; i < count; i++) {
        if (s_work.album_artist[i] == artist &&
            name_eq_ci(s_work.album_name[i], name)) {
            return (int)i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Cover caching                                                            */
/* ------------------------------------------------------------------------- */

static bool cache_has_room_for_art(void) {
    const uint32_t used = media_cache_used();
    return used + (uint32_t)(MEDIA_ART_PIXELS * 2u) + LIB_INDEX_RESERVE <=
           MEDIA_CACHE_SIZE;
}

/** Append the 120x120 image in s_work.art_words. False when it does not fit. */
static bool cache_art_image(uint16_t album) {
    if (!cache_has_room_for_art()) {
        s_work.album_art_state[album] = (uint8_t)MEDIA_LIB_ART_NONE;
        s_work.album_art_off[album] = 0u;
        return false;
    }
    uint32_t off = 0;
    if (!media_cache_write(s_work.art_words, (uint32_t)(MEDIA_ART_PIXELS * 2u),
                           &off)) {
        s_work.album_art_state[album] = (uint8_t)MEDIA_LIB_ART_NONE;
        s_work.album_art_off[album] = 0u;
        return false;
    }
    s_work.album_art_off[album] = off;
    return true;
}

/**
 * Try to get cover art for @p album from the current file. Decoding runs once
 * per album: the first file of the album that carries a picture decides.
 */
static void decode_album_art(media_library_t *lib, uint16_t album,
                             const media_source_t *src, const media_meta_t *meta) {
    if (s_work.album_art_state[album] != ART_STATE_UNTRIED) {
        return;
    }
    if (meta->art.type == MEDIA_ART_NONE) {
        return; /* a later track of the same album may still carry one */
    }
    lib->stats.art_decodes++;
    uint16_t *out = (uint16_t *)(void *)s_work.art_words;
    const media_art_status_t st =
        media_art_decode_120(src, &meta->art, out, s_work.art_work,
                             sizeof(s_work.art_work));
    if (st == MEDIA_ART_OK) {
        if (cache_art_image(album)) {
            s_work.album_art_state[album] = (uint8_t)MEDIA_LIB_ART_DECODED;
        }
        return;
    }
    /* No picture, unsupported format (PNG, progressive JPEG) or a broken
     * stream: draw the deterministic placeholder for this album instead. */
    media_art_placeholder_120(
        out, name_hash(s_work.artist_name[s_work.album_artist[album]],
                       s_work.album_name[album]));
    if (cache_art_image(album)) {
        s_work.album_art_state[album] = (uint8_t)MEDIA_LIB_ART_PLACEHOLDER;
        lib->stats.art_placeholders++;
    }
}

/* ------------------------------------------------------------------------- */
/* Index loading (cache hit)                                                 */
/* ------------------------------------------------------------------------- */

static bool range_ok(uint32_t off, uint32_t bytes, uint32_t cache_bytes) {
    if ((off & 3u) != 0u || off < MEDIA_CACHE_DATA_OFFSET) {
        return false;
    }
    if (bytes > cache_bytes || off > cache_bytes - bytes) {
        return false;
    }
    return true;
}

static bool load_index(media_library_t *lib) {
    uint8_t user[MEDIA_CACHE_USER_SIZE];
    if (!media_cache_user(user)) {
        return false;
    }
    if (user[0] != LIB_USER_MAGIC0 || user[1] != LIB_USER_MAGIC1 ||
        user[2] != LIB_USER_MAGIC2 || user[3] != LIB_USER_MAGIC3) {
        return false;
    }
    const uint32_t root_off = rd32(user + 4);
    if (rd32(user + 8) != LIB_LAYOUT_VERSION) {
        return false;
    }
    if ((root_off & 3u) != 0u || root_off < MEDIA_CACHE_DATA_OFFSET ||
        root_off > MEDIA_CACHE_SIZE - LIB_ROOT_SIZE) {
        return false;
    }
    const uint8_t *root = media_cache_ptr(root_off);
    if (root == NULL || memcmp(root + ROOT_OFF_MAGIC, LIB_ROOT_MAGIC, 4) != 0 ||
        rd16(root + ROOT_OFF_VERSION) != LIB_LAYOUT_VERSION ||
        rd16(root + ROOT_OFF_SIZE) != LIB_ROOT_SIZE) {
        return false;
    }
    const uint16_t nt = rd16(root + ROOT_OFF_TRACKS);
    const uint16_t na = rd16(root + ROOT_OFF_ALBUMS);
    const uint16_t nr = rd16(root + ROOT_OFF_ARTISTS);
    if (nt > MEDIA_LIB_MAX_TRACKS || na > MEDIA_LIB_MAX_ALBUMS ||
        nr > MEDIA_LIB_MAX_ARTISTS) {
        return false;
    }
    if (rd32(root + ROOT_OFF_TRACK_BLOCK) != LIB_TRACK_BLOCK ||
        rd32(root + ROOT_OFF_ARTIST_REC_SZ) != LIB_ARTIST_REC) {
        return false;
    }
    const uint32_t cache_bytes = rd32(root + ROOT_OFF_CACHE_BYTES);
    if (cache_bytes < MEDIA_CACHE_DATA_OFFSET || cache_bytes > MEDIA_CACHE_SIZE ||
        root_off + LIB_ROOT_SIZE > cache_bytes) {
        return false;
    }

    const uint32_t tt_off = rd32(root + ROOT_OFF_TRACK_TABLE);
    const uint32_t ar_off = rd32(root + ROOT_OFF_ARTIST_REC);
    const uint32_t al_off = rd32(root + ROOT_OFF_ALBUM_REC);
    const uint32_t oa_off = rd32(root + ROOT_OFF_ORDER_ART);
    const uint32_t ob_off = rd32(root + ROOT_OFF_ORDER_ALB);
    const uint32_t oba_off = rd32(root + ROOT_OFF_ORDER_ALBA);
    const uint32_t ot_off = rd32(root + ROOT_OFF_ORDER_TITLE);
    const uint32_t oal_off = rd32(root + ROOT_OFF_ORDER_ALBUM);

    if (!range_ok(tt_off, (uint32_t)nt * 4u, cache_bytes) && nt != 0u) {
        return false;
    }
    if (!range_ok(ar_off, (uint32_t)nr * LIB_ARTIST_REC, cache_bytes) && nr != 0u) {
        return false;
    }
    if (!range_ok(al_off, (uint32_t)na * LIB_ALBUM_REC, cache_bytes) && na != 0u) {
        return false;
    }
    if ((!range_ok(oa_off, (uint32_t)nr * 2u, cache_bytes) && nr != 0u) ||
        (!range_ok(ob_off, (uint32_t)na * 2u, cache_bytes) && na != 0u) ||
        (!range_ok(oba_off, (uint32_t)na * 2u, cache_bytes) && na != 0u) ||
        (!range_ok(ot_off, (uint32_t)nt * 2u, cache_bytes) && nt != 0u) ||
        (!range_ok(oal_off, (uint32_t)nt * 2u, cache_bytes) && nt != 0u)) {
        return false;
    }

    const uint8_t *p = media_cache_ptr(tt_off);
    if (p == NULL) {
        return false;
    }
    memcpy(lib->track_table, p, (size_t)nt * 4u);
    for (uint16_t i = 0; i < nt; i++) {
        const uint32_t off = lib->track_table[i];
        if ((off & 3u) != 0u || !range_ok(off, LIB_TRACK_BLOCK, cache_bytes)) {
            return false;
        }
    }

    p = media_cache_ptr(oa_off);
    if (p == NULL) {
        return false;
    }
    memcpy(lib->order_artist, p, (size_t)nr * 2u);
    p = media_cache_ptr(ob_off);
    if (p == NULL) {
        return false;
    }
    memcpy(lib->order_album, p, (size_t)na * 2u);
    p = media_cache_ptr(oba_off);
    if (p == NULL) {
        return false;
    }
    memcpy(lib->order_album_artist, p, (size_t)na * 2u);
    p = media_cache_ptr(ot_off);
    if (p == NULL) {
        return false;
    }
    memcpy(lib->order_track_title, p, (size_t)nt * 2u);
    p = media_cache_ptr(oal_off);
    if (p == NULL) {
        return false;
    }
    memcpy(lib->order_track_album, p, (size_t)nt * 2u);

    lib->artist_off = ar_off;
    lib->album_off = al_off;
    lib->track_count = nt;
    lib->album_count = na;
    lib->artist_count = nr;

    media_scan_stats_t *st = &lib->stats;
    st->parsed = rd16(root + ROOT_OFF_PARSED);
    st->skipped = rd16(root + ROOT_OFF_SKIPPED);
    st->tracks = nt;
    st->albums = na;
    st->artists = nr;
    st->cache_bytes = cache_bytes;
    st->parse_calls = 0;
    st->art_decodes = 0;
    st->art_albums = 0;
    st->art_placeholders = 0;

    for (uint16_t i = 0; i < na; i++) {
        const uint8_t *rec = media_cache_ptr(al_off + (uint32_t)i * LIB_ALBUM_REC);
        if (rec == NULL) {
            return false;
        }
        const uint32_t art_off = rd32(rec + AR_ART_OFF);
        const uint8_t art_state = rec[AR_ART_STATE];
        lib->art[i].width = (uint16_t)MEDIA_ART_SIZE;
        lib->art[i].height = (uint16_t)MEDIA_ART_SIZE;
        if (art_off != 0u && range_ok(art_off, (uint32_t)MEDIA_ART_PIXELS * 2u,
                                      cache_bytes)) {
            lib->art[i].pixels = (const uint16_t *)media_cache_ptr(art_off);
        } else {
            lib->art[i].pixels = NULL;
        }
        if (lib->art[i].pixels != NULL) {
            st->art_albums++;
            if (art_state == (uint8_t)MEDIA_LIB_ART_PLACEHOLDER) {
                st->art_placeholders++;
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Full scan                                                                */
/* ------------------------------------------------------------------------- */

static bool cache_write_or_fail(const void *data, uint32_t len, uint32_t *off) {
    return media_cache_write(data, len, off);
}

static bool full_scan(media_library_t *lib, media_scan_progress_fn cb) {
    if (!media_cache_reset()) {
        return false;
    }
    memset(&s_work, 0, sizeof(s_work));
    memset(&lib->stats, 0, sizeof(lib->stats));

    media_scan_stats_t *st = &lib->stats;
    st->files = media_pack_count(&lib->pack);
    lib->track_count = 0;
    lib->album_count = 0;
    lib->artist_count = 0;

    uint16_t nt = 0;
    uint16_t na = 0;
    uint16_t nr = 0;

    for (uint16_t i = 0; i < st->files; i++) {
        media_scan_report_t report;
        memset(&report, 0, sizeof(report));
        report.index = i;
        report.count = st->files;
        report.name = media_pack_name(&lib->pack, i);
        report.album_index = 0xFFFFu;
        report.meta_status = (int8_t)MEDIA_ERR_IO;

        media_pack_file_t file;
        if (!media_pack_file_at(&lib->pack, i, &file)) {
            st->skipped++;
            if (cb != NULL) {
                cb(&report);
            }
            continue;
        }
        media_source_t src;
        if (!media_pack_source_ex(&lib->pack, i, &file, &src)) {
            st->skipped++;
            if (cb != NULL) {
                cb(&report);
            }
            continue;
        }

        media_meta_t meta;
        memset(&meta, 0, sizeof(meta));
        const media_status_t ms =
            media_meta_parse(&src, report.name, &meta, s_work.scratch,
                             sizeof(s_work.scratch));
        st->parse_calls++;
        report.meta_status = (int8_t)ms;
        if (ms != MEDIA_OK) {
            st->skipped++;
            if (cb != NULL) {
                cb(&report);
            }
            continue;
        }
        st->parsed++;

        if (nt >= MEDIA_LIB_MAX_TRACKS) {
            st->skipped++; /* track cap: do not create an empty album */
            if (cb != NULL) {
                cb(&report);
            }
            continue;
        }

        const char *artist = meta.album_artist[0] != '\0' ? meta.album_artist
                                                          : meta.artist;
        if (artist[0] == '\0') {
            artist = "Unknown Artist";
        }
        const char *album = meta.album[0] != '\0' ? meta.album : "Unknown Album";

        int ai = find_artist(artist, nr);
        if (ai < 0) {
            if (nr >= MEDIA_LIB_MAX_ARTISTS) {
                st->skipped++; /* over capacity: keep the rest usable */
                if (cb != NULL) {
                    cb(&report);
                }
                continue;
            }
            ai = (int)nr++;
            copy_name(s_work.artist_name[ai], sizeof(s_work.artist_name[ai]),
                      artist);
        }

        int al = find_album((uint16_t)ai, album, na);
        if (al < 0) {
            if (na >= MEDIA_LIB_MAX_ALBUMS) {
                st->skipped++;
                if (cb != NULL) {
                    cb(&report);
                }
                continue;
            }
            al = (int)na++;
            copy_name(s_work.album_name[al], sizeof(s_work.album_name[al]), album);
            s_work.album_artist[al] = (uint16_t)ai;
            s_work.album_art_state[al] = ART_STATE_UNTRIED;
            s_work.album_song_first[al] = 0xFFFFu;
            s_work.album_song_count[al] = 0u;
        }

        decode_album_art(lib, (uint16_t)al, &src, &meta);

        uint8_t block[LIB_TRACK_BLOCK];
        memset(block, 0, sizeof(block));
        wr16(block + TB_FILE, i);
        wr16(block + TB_ALBUM, (uint16_t)al);
        wr16(block + TB_DISC, meta.disc);
        wr16(block + TB_DISC_TOTAL, meta.disc_total);
        wr16(block + TB_TRACK, meta.track);
        wr16(block + TB_TRACK_TOTAL, meta.track_total);
        wr32(block + TB_DURATION, meta.duration_ms);
        wr32(block + TB_SAMPLE_RATE, meta.sample_rate);
        wr32(block + TB_BITRATE, meta.bitrate_kbps);
        wr32(block + TB_AUDIO_OFF, meta.audio_offset);
        wr32(block + TB_AUDIO_SIZE, meta.audio_size);
        block[TB_FORMAT] = meta.format;
        block[TB_CHANNELS] = meta.channels;
        block[TB_BITS] = meta.bits_per_sample;
        copy_name((char *)(void *)(block + LIB_TRACK_TITLE_OFF), MEDIA_TITLE_MAX,
                  meta.title);
        copy_name((char *)(void *)(block + LIB_TRACK_ARTIST_OFF), MEDIA_NAME_MAX,
                  meta.artist);

        uint32_t off = 0;
        if (!cache_write_or_fail(block, sizeof(block), &off)) {
            return false;
        }
        lib->track_table[nt] = off;
        copy_name(s_work.title[nt], MEDIA_TITLE_MAX, meta.title);
        s_work.track_album[nt] = (uint16_t)al;
        s_work.track_disc[nt] = meta.disc;
        s_work.track_no[nt] = meta.track;
        nt++;

        report.meta = &meta;
        report.album_index = (uint16_t)al;
        report.album_art = s_work.album_art_state[al] == ART_STATE_UNTRIED
                               ? (uint8_t)MEDIA_LIB_ART_NONE
                               : s_work.album_art_state[al];
        if (cb != NULL) {
            cb(&report);
        }
    }

    /* Albums that never met a picture get a seeded placeholder. */
    for (uint16_t a = 0; a < na; a++) {
        if (s_work.album_art_state[a] != ART_STATE_UNTRIED) {
            continue;
        }
        uint16_t *out = (uint16_t *)(void *)s_work.art_words;
        media_art_placeholder_120(
            out, name_hash(s_work.artist_name[s_work.album_artist[a]],
                           s_work.album_name[a]));
        if (cache_art_image(a)) {
            s_work.album_art_state[a] = (uint8_t)MEDIA_LIB_ART_PLACEHOLDER;
            st->art_placeholders++;
        }
    }

    /* Sorted orders. */
    for (uint16_t i = 0; i < nr; i++) {
        lib->order_artist[i] = i;
    }
    for (uint16_t i = 0; i < na; i++) {
        lib->order_album[i] = i;
        lib->order_album_artist[i] = i;
    }
    for (uint16_t i = 0; i < nt; i++) {
        lib->order_track_title[i] = i;
        lib->order_track_album[i] = i;
    }
    sort_ids(lib->order_artist, nr, cmp_artist_ids);
    sort_ids(lib->order_album, na, cmp_album_ids);
    sort_ids(lib->order_album_artist, na, cmp_album_artist_ids);
    sort_ids(lib->order_track_title, nt, cmp_track_title);
    sort_ids(lib->order_track_album, nt, cmp_track_album);

    /* Per-album song ranges (album-grouped order). */
    for (uint16_t a = 0; a < na; a++) {
        s_work.album_song_first[a] = 0u;
        s_work.album_song_count[a] = 0u;
    }
    for (uint16_t i = 0; i < nt; i++) {
        const uint16_t a = s_work.track_album[lib->order_track_album[i]];
        if (s_work.album_song_count[a] == 0u) {
            s_work.album_song_first[a] = i;
        }
        s_work.album_song_count[a]++;
    }

    /* Per-artist album ranges (artist-grouped order). */
    for (uint16_t a = 0; a < nr; a++) {
        s_work.artist_album_first[a] = 0u;
        s_work.artist_album_count[a] = 0u;
    }
    for (uint16_t i = 0; i < na; i++) {
        const uint16_t a = s_work.album_artist[lib->order_album_artist[i]];
        if (s_work.artist_album_count[a] == 0u) {
            s_work.artist_album_first[a] = i;
        }
        s_work.artist_album_count[a]++;
    }

    lib->track_count = nt;
    lib->album_count = na;
    lib->artist_count = nr;
    st->tracks = nt;
    st->albums = na;
    st->artists = nr;

    /* Track offset table. */
    uint32_t tt_off = 0;
    if (nt > 0u &&
        !media_cache_write(lib->track_table, (uint32_t)nt * 4u, &tt_off)) {
        return false;
    }

    /* Artist records. */
    uint32_t ar_off = 0;
    for (uint16_t a = 0; a < nr; a++) {
        uint8_t rec[LIB_ARTIST_REC];
        memset(rec, 0, sizeof(rec));
        copy_name((char *)(void *)(rec + SR_NAME), MEDIA_NAME_MAX,
                  s_work.artist_name[a]);
        wr16(rec + SR_ALBUM_FIRST, s_work.artist_album_first[a]);
        wr16(rec + SR_ALBUM_COUNT, s_work.artist_album_count[a]);
        uint32_t off = 0;
        if (!media_cache_write(rec, sizeof(rec), &off)) {
            return false;
        }
        if (a == 0u) {
            ar_off = off;
        }
    }
    lib->artist_off = ar_off;

    /* Album records. */
    uint32_t al_off = 0;
    for (uint16_t a = 0; a < na; a++) {
        uint8_t rec[LIB_ALBUM_REC];
        memset(rec, 0, sizeof(rec));
        copy_name((char *)(void *)(rec + AR_NAME), MEDIA_NAME_MAX,
                  s_work.album_name[a]);
        wr16(rec + AR_ARTIST, s_work.album_artist[a]);
        wr16(rec + AR_SONG_FIRST, s_work.album_song_first[a]);
        wr16(rec + AR_SONG_COUNT, s_work.album_song_count[a]);
        wr32(rec + AR_ART_OFF, s_work.album_art_off[a]);
        rec[AR_ART_STATE] = s_work.album_art_state[a] == ART_STATE_UNTRIED
                                ? (uint8_t)MEDIA_LIB_ART_NONE
                                : s_work.album_art_state[a];
        uint32_t off = 0;
        if (!media_cache_write(rec, sizeof(rec), &off)) {
            return false;
        }
        if (a == 0u) {
            al_off = off;
        }
    }
    lib->album_off = al_off;

    /* Order vectors. */
    uint32_t oa_off = 0, ob_off = 0, oba_off = 0, ot_off = 0, oal_off = 0;
    if (nr > 0u && !media_cache_write(lib->order_artist, (uint32_t)nr * 2u, &oa_off)) {
        return false;
    }
    if (na > 0u && !media_cache_write(lib->order_album, (uint32_t)na * 2u, &ob_off)) {
        return false;
    }
    if (na > 0u &&
        !media_cache_write(lib->order_album_artist, (uint32_t)na * 2u, &oba_off)) {
        return false;
    }
    if (nt > 0u &&
        !media_cache_write(lib->order_track_title, (uint32_t)nt * 2u, &ot_off)) {
        return false;
    }
    if (nt > 0u &&
        !media_cache_write(lib->order_track_album, (uint32_t)nt * 2u, &oal_off)) {
        return false;
    }

    /* Root record (last record; its offset goes into the cache header). */
    uint8_t root[LIB_ROOT_SIZE];
    memset(root, 0, sizeof(root));
    memcpy(root + ROOT_OFF_MAGIC, LIB_ROOT_MAGIC, 4);
    wr16(root + ROOT_OFF_VERSION, LIB_LAYOUT_VERSION);
    wr16(root + ROOT_OFF_SIZE, LIB_ROOT_SIZE);
    wr16(root + ROOT_OFF_TRACKS, nt);
    wr16(root + ROOT_OFF_ALBUMS, na);
    wr16(root + ROOT_OFF_ARTISTS, nr);
    wr16(root + ROOT_OFF_PARSED, st->parsed);
    wr16(root + ROOT_OFF_SKIPPED, st->skipped);
    wr32(root + ROOT_OFF_TRACK_TABLE, tt_off);
    wr32(root + ROOT_OFF_ARTIST_REC, ar_off);
    wr32(root + ROOT_OFF_ALBUM_REC, al_off);
    wr32(root + ROOT_OFF_ORDER_ART, oa_off);
    wr32(root + ROOT_OFF_ORDER_ALB, ob_off);
    wr32(root + ROOT_OFF_ORDER_ALBA, oba_off);
    wr32(root + ROOT_OFF_ORDER_TITLE, ot_off);
    wr32(root + ROOT_OFF_ORDER_ALBUM, oal_off);
    wr32(root + ROOT_OFF_TRACK_BLOCK, LIB_TRACK_BLOCK);
    wr32(root + ROOT_OFF_ARTIST_REC_SZ, LIB_ARTIST_REC);
    /* The root itself is the last record: its size is part of the final used
     * size, and media_cache_commit() rounds that up to a sector, so the value
     * can be computed before appending the root (64 bytes, already aligned). */
    const uint32_t committed_bytes =
        (media_cache_used() + LIB_ROOT_SIZE + MEDIA_CACHE_SECTOR_SIZE - 1u) &
        ~(MEDIA_CACHE_SECTOR_SIZE - 1u);
    wr32(root + ROOT_OFF_CACHE_BYTES, committed_bytes);

    uint32_t root_off = 0;
    if (!media_cache_write(root, sizeof(root), &root_off)) {
        return false;
    }

    uint8_t user[MEDIA_CACHE_USER_SIZE];
    memset(user, 0, sizeof(user));
    user[0] = LIB_USER_MAGIC0;
    user[1] = LIB_USER_MAGIC1;
    user[2] = LIB_USER_MAGIC2;
    user[3] = LIB_USER_MAGIC3;
    wr32(user + 4, root_off);
    wr32(user + 8, LIB_LAYOUT_VERSION);

    if (!media_cache_commit_ex(lib->key, user)) {
        return false;
    }

    st->cache_bytes = media_cache_used();
    lib->stats.valid = false; /* set by the caller after the art shells */

    /* Build the RAM art views (the cache is committed and readable now). */
    for (uint16_t a = 0; a < na; a++) {
        lib->art[a].width = (uint16_t)MEDIA_ART_SIZE;
        lib->art[a].height = (uint16_t)MEDIA_ART_SIZE;
        const uint32_t art_off = s_work.album_art_off[a];
        lib->art[a].pixels =
            art_off != 0u ? (const uint16_t *)media_cache_ptr(art_off) : NULL;
        if (lib->art[a].pixels != NULL) {
            st->art_albums++;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                               */
/* ------------------------------------------------------------------------- */

bool media_library_scan(media_library_t *lib, media_scan_progress_fn cb) {
    if (lib == NULL) {
        return false;
    }
    memset(lib, 0, sizeof(*lib));
    lib->ui.count = NULL; /* filled below on success */
    lib->stats = s_zero_stats;

    if (!media_pack_open(&lib->pack)) {
        lib->stats.valid = false;
        return false;
    }
    lib->key = media_pack_cache_key(&lib->pack);
    lib->stats.files = media_pack_count(&lib->pack);

    if (media_cache_valid(lib->key) && load_index(lib)) {
        lib->ready = lib->track_count > 0u;
        lib->stats.valid = lib->ready;
        lib->stats.cache_hit = true;
        return true;
    }

    if (!full_scan(lib, cb)) {
        lib->ready = false;
        lib->stats.valid = false;
        return false;
    }
    lib->ready = lib->track_count > 0u;
    lib->stats.valid = lib->ready;
    return true;
}

const media_scan_stats_t *media_library_stats(const media_library_t *lib) {
    return lib != NULL ? &lib->stats : &s_zero_stats;
}

bool media_library_ready(const media_library_t *lib) {
    return lib != NULL && lib->ready;
}

/* ------------------------------------------------------------------------- */
/* ui_library_t implementation                                               */
/* ------------------------------------------------------------------------- */

static uint16_t artist_index_of(const media_library_t *lib, uint32_t id) {
    if (id < MEDIA_LIB_ARTIST_ID_BASE) {
        return 0xFFFFu;
    }
    const uint32_t idx = id - MEDIA_LIB_ARTIST_ID_BASE;
    return idx < lib->artist_count ? (uint16_t)idx : 0xFFFFu;
}

static uint16_t album_index_of(const media_library_t *lib, uint32_t id) {
    if (id < MEDIA_LIB_ALBUM_ID_BASE) {
        return 0xFFFFu;
    }
    const uint32_t idx = id - MEDIA_LIB_ALBUM_ID_BASE;
    return idx < lib->album_count ? (uint16_t)idx : 0xFFFFu;
}

static uint16_t track_index_of(const media_library_t *lib, uint32_t id) {
    if (id < MEDIA_LIB_TRACK_ID_BASE) {
        return 0xFFFFu;
    }
    const uint32_t idx = id - MEDIA_LIB_TRACK_ID_BASE;
    return idx < lib->track_count ? (uint16_t)idx : 0xFFFFu;
}

static const uint8_t *artist_rec(const media_library_t *lib, uint16_t index) {
    if (index >= lib->artist_count || lib->artist_off == 0u) {
        return NULL;
    }
    return media_cache_ptr(lib->artist_off + (uint32_t)index * LIB_ARTIST_REC);
}

static const uint8_t *album_rec(const media_library_t *lib, uint16_t index) {
    if (index >= lib->album_count || lib->album_off == 0u) {
        return NULL;
    }
    return media_cache_ptr(lib->album_off + (uint32_t)index * LIB_ALBUM_REC);
}

static const uint8_t *track_rec(const media_library_t *lib, uint16_t index) {
    if (index >= lib->track_count) {
        return NULL;
    }
    return media_cache_ptr(lib->track_table[index]);
}

static uint32_t lib_count(ui_list_kind_t list, uint32_t parent_id, void *ctx) {
    const media_library_t *lib = (const media_library_t *)ctx;
    if (lib == NULL || !lib->ready) {
        return 0u;
    }
    switch (list) {
    case UI_LIST_ARTISTS:
        return lib->artist_count;

    case UI_LIST_ALBUMS: {
        if (parent_id == UI_ID_ALL) {
            return lib->album_count;
        }
        const uint16_t a = artist_index_of(lib, parent_id);
        const uint8_t *rec = artist_rec(lib, a);
        return rec != NULL ? rd16(rec + SR_ALBUM_COUNT) : 0u;
    }

    case UI_LIST_SONGS: {
        if (parent_id == UI_ID_ALL) {
            return lib->track_count;
        }
        const uint16_t a = album_index_of(lib, parent_id);
        const uint8_t *rec = album_rec(lib, a);
        return rec != NULL ? rd16(rec + AR_SONG_COUNT) : 0u;
    }

    case UI_LIST_PLAYLISTS:
        return MEDIA_LIB_PLAYLIST_COUNT;

    case UI_LIST_PLAYLIST_SONGS:
        return parent_id == MEDIA_LIB_PLAYLIST_ALL_ID ? lib->track_count : 0u;

    default:
        return 0u;
    }
}

static bool lib_item_artist(const media_library_t *lib, uint16_t idx,
                            ui_list_item_t *out) {
    if (idx >= lib->artist_count) {
        return false;
    }
    const uint16_t artist = lib->order_artist[idx];
    const uint8_t *rec = artist_rec(lib, artist);
    if (rec == NULL) {
        return false;
    }
    out->id = MEDIA_LIB_ARTIST_ID_BASE + artist;
    copy_cache_str(out->title, sizeof(out->title), (const char *)(rec + SR_NAME),
                   MEDIA_NAME_MAX);
    const uint16_t albums = rd16(rec + SR_ALBUM_COUNT);
    (void)snprintf(out->subtitle, sizeof(out->subtitle), "%u album%s",
                   (unsigned)albums, albums == 1u ? "" : "s");
    return true;
}

static bool lib_item_album(const media_library_t *lib, uint32_t parent_id,
                           uint16_t idx, ui_list_item_t *out) {
    uint16_t album;
    if (parent_id == UI_ID_ALL) {
        if (idx >= lib->album_count) {
            return false;
        }
        album = lib->order_album[idx];
    } else {
        const uint16_t artist = artist_index_of(lib, parent_id);
        const uint8_t *arec = artist_rec(lib, artist);
        if (arec == NULL) {
            return false;
        }
        const uint16_t first = rd16(arec + SR_ALBUM_FIRST);
        const uint16_t count = rd16(arec + SR_ALBUM_COUNT);
        if (idx >= count) {
            return false;
        }
        album = lib->order_album_artist[first + idx];
    }
    const uint8_t *rec = album_rec(lib, album);
    if (rec == NULL) {
        return false;
    }
    out->id = MEDIA_LIB_ALBUM_ID_BASE + album;
    copy_cache_str(out->title, sizeof(out->title), (const char *)(rec + AR_NAME),
                   MEDIA_NAME_MAX);
    const uint16_t artist = rd16(rec + AR_ARTIST);
    const uint8_t *arec = artist_rec(lib, artist);
    if (arec != NULL) {
        copy_cache_str(out->subtitle, sizeof(out->subtitle),
                       (const char *)(arec + SR_NAME), MEDIA_NAME_MAX);
    }
    return true;
}

static bool lib_item_song(const media_library_t *lib, uint32_t parent_id,
                          uint16_t idx, ui_list_item_t *out) {
    uint16_t track;
    if (parent_id == UI_ID_ALL || parent_id == MEDIA_LIB_PLAYLIST_ALL_ID) {
        if (idx >= lib->track_count) {
            return false;
        }
        track = lib->order_track_title[idx];
    } else {
        const uint16_t album = album_index_of(lib, parent_id);
        const uint8_t *rec = album_rec(lib, album);
        if (rec == NULL) {
            return false;
        }
        const uint16_t first = rd16(rec + AR_SONG_FIRST);
        const uint16_t count = rd16(rec + AR_SONG_COUNT);
        if (idx >= count) {
            return false;
        }
        track = lib->order_track_album[first + idx];
    }
    const uint8_t *rec = track_rec(lib, track);
    if (rec == NULL) {
        return false;
    }
    out->id = MEDIA_LIB_TRACK_ID_BASE + track;
    copy_cache_str(out->title, sizeof(out->title),
                   (const char *)(rec + LIB_TRACK_TITLE_OFF), MEDIA_TITLE_MAX);
    copy_cache_str(out->subtitle, sizeof(out->subtitle),
                   (const char *)(rec + LIB_TRACK_ARTIST_OFF), MEDIA_NAME_MAX);
    return true;
}

static bool lib_get_item(ui_list_kind_t list, uint32_t parent_id, uint32_t index,
                         ui_list_item_t *out, void *ctx) {
    const media_library_t *lib = (const media_library_t *)ctx;
    if (lib == NULL || !lib->ready || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    switch (list) {
    case UI_LIST_ARTISTS:
        return lib_item_artist(lib, (uint16_t)index, out);

    case UI_LIST_ALBUMS:
        return lib_item_album(lib, parent_id, (uint16_t)index, out);

    case UI_LIST_SONGS:
        return lib_item_song(lib, parent_id, (uint16_t)index, out);

    case UI_LIST_PLAYLISTS:
        if (index != 0u) {
            return false;
        }
        out->id = MEDIA_LIB_PLAYLIST_ALL_ID;
        copy_name(out->title, sizeof(out->title), "All Songs");
        (void)snprintf(out->subtitle, sizeof(out->subtitle), "%u song%s",
                       (unsigned)lib->track_count,
                       lib->track_count == 1u ? "" : "s");
        return true;

    case UI_LIST_PLAYLIST_SONGS:
        if (parent_id != MEDIA_LIB_PLAYLIST_ALL_ID) {
            return false;
        }
        return lib_item_song(lib, parent_id, (uint16_t)index, out);

    default:
        return false;
    }
}

static const gfx_image_t *lib_get_album_art(uint32_t album_id, void *ctx) {
    media_library_t *lib = (media_library_t *)ctx;
    if (lib == NULL || !lib->ready) {
        return NULL;
    }
    const uint16_t album = album_index_of(lib, album_id);
    if (album >= MEDIA_LIB_MAX_ALBUMS || album >= lib->album_count) {
        return NULL;
    }
    const gfx_image_t *img = &lib->art[album];
    return img->pixels != NULL ? img : NULL;
}

const ui_library_t *media_library_ui(media_library_t *lib) {
    if (lib == NULL || !lib->ready) {
        return NULL;
    }
    lib->ui.count = lib_count;
    lib->ui.get_item = lib_get_item;
    lib->ui.get_album_art = lib_get_album_art;
    lib->ui.ctx = lib;
    return &lib->ui;
}

/* ------------------------------------------------------------------------- */
/* Track metadata + audio source                                             */
/* ------------------------------------------------------------------------- */

static int32_t audio_view_read(void *ctx, uint32_t off, void *dst, uint32_t len) {
    media_library_t *lib = (media_library_t *)ctx;
    if (lib == NULL) {
        return -1;
    }
    if (dst == NULL && len != 0u) {
        return -1;
    }
    if (off > lib->audio_view.size) {
        return -1;
    }
    uint32_t avail = lib->audio_view.size - off;
    if (len > avail) {
        len = avail;
    }
    if (len != 0u) {
        memcpy(dst, lib->audio_view.base + off, len);
    }
    return (int32_t)len;
}

bool media_library_track(media_library_t *lib, uint32_t track_id,
                         media_track_info_t *out) {
    if (lib == NULL || out == NULL || !lib->ready) {
        return false;
    }
    const uint16_t track = track_index_of(lib, track_id);
    if (track == 0xFFFFu) {
        return false;
    }
    const uint8_t *rec = track_rec(lib, track);
    if (rec == NULL) {
        return false;
    }
    const uint16_t album = rd16(rec + TB_ALBUM);
    const uint8_t *arec = album_rec(lib, album);
    if (arec == NULL) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->id = track_id;
    out->index = track;
    out->album_id = MEDIA_LIB_ALBUM_ID_BASE + album;
    copy_cache_str(out->title, sizeof(out->title),
                   (const char *)(rec + LIB_TRACK_TITLE_OFF), MEDIA_TITLE_MAX);
    copy_cache_str(out->artist, sizeof(out->artist),
                   (const char *)(rec + LIB_TRACK_ARTIST_OFF), MEDIA_NAME_MAX);
    copy_cache_str(out->album, sizeof(out->album), (const char *)(arec + AR_NAME),
                   MEDIA_NAME_MAX);
    out->disc = rd16(rec + TB_DISC);
    out->disc_total = rd16(rec + TB_DISC_TOTAL);
    out->track = rd16(rec + TB_TRACK);
    out->track_total = rd16(rec + TB_TRACK_TOTAL);
    out->duration_ms = rd32(rec + TB_DURATION);
    out->sample_rate = rd32(rec + TB_SAMPLE_RATE);
    out->bitrate_kbps = rd32(rec + TB_BITRATE);
    out->channels = rec[TB_CHANNELS];
    out->bits_per_sample = rec[TB_BITS];
    out->format = rec[TB_FORMAT];
    out->audio_offset = rd32(rec + TB_AUDIO_OFF);
    out->audio_size = rd32(rec + TB_AUDIO_SIZE);

    media_pack_file_t file;
    const uint16_t file_index = rd16(rec + TB_FILE);
    if (media_pack_file_at(&lib->pack, file_index, &file) &&
        out->audio_offset <= file.size) {
        uint32_t size = out->audio_size;
        if (size > file.size - out->audio_offset) {
            size = file.size - out->audio_offset;
        }
        lib->audio_view.base = file.data + out->audio_offset;
        lib->audio_view.size = size;
        out->source.read = audio_view_read;
        out->source.size = size;
        out->source.xip_base = lib->audio_view.base;
        out->source.ctx = lib;
    }
    return true;
}
