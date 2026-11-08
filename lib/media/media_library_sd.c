/**
 * @file media_library_sd.c
 * @brief Build the media library from an SD card directory tree.
 *
 * Two passes over the card:
 *
 *  1. a directory walk that hashes the file list (path, size, FAT modification
 *     stamp) into the cache key;
 *  2. on a cache miss, a second walk that parses tags/art through the same
 *     engine as the flash-pack scan and writes the index + covers to the flash
 *     cache region, plus one cached path per track so audio can be opened later
 *     without the flash pack.
 *
 * A cache hit still walks the directories (to know the key) but opens no file
 * and calls neither media_meta_parse() nor the JPEG decoder.
 */
#include <stdio.h>
#include <string.h>

#include "media_cache.h"
#include "media_library.h"
#include "media_library_internal.h"
#include "storage.h"
#include "storage_fs.h"

/** Hash of the whole file list, plus the counters reported in the stats. */
typedef struct {
    uint32_t hash;
    uint16_t files;
    uint16_t ignored;
    bool cap_hit;
} sd_key_ctx_t;

/** Positional read view of the file currently being parsed. */
typedef struct {
    storage_fs_file_t *file;
    uint32_t size;
} sd_scan_view_t;

typedef struct {
    media_library_t *lib;
    media_scan_progress_fn cb;
    uint16_t count;
    uint16_t index;
    bool ok;
} sd_parse_ctx_t;

static char lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool has_ext(const char *name, const char *ext) {
    const size_t n = strlen(name);
    const size_t e = strlen(ext);
    if (n < e) {
        return false;
    }
    for (size_t i = 0; i < e; i++) {
        if (lower_ascii(name[n - e + i]) != ext[i]) {
            return false;
        }
    }
    return true;
}

/** Extensions the metadata parser understands (see media_meta.h). */
static bool is_media_file(const char *name) {
    return has_ext(name, ".mp3") || has_ext(name, ".flac") ||
           has_ext(name, ".wav") || has_ext(name, ".ogg") ||
           has_ext(name, ".opus");
}

static void hash_bytes(uint32_t *h, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        *h ^= p[i];
        *h *= 16777619u;
    }
}

/* ------------------------------------------------------------------------- */
/* Pass 1: cache key                                                         */
/* ------------------------------------------------------------------------- */

static bool key_visit(void *vctx, const storage_fs_entry_t *entry, bool is_dir) {
    sd_key_ctx_t *ctx = (sd_key_ctx_t *)vctx;
    if (is_dir) {
        return true;
    }
    if (entry->name[0] == '.' || !is_media_file(entry->name)) {
        ctx->ignored++;
        return true;
    }
    if (ctx->files >= MEDIA_SD_MAX_FILES) {
        ctx->cap_hit = true;
        return false; /* library truncated: stop hashing */
    }
    hash_bytes(&ctx->hash, entry->path, strlen(entry->path));
    hash_bytes(&ctx->hash, &entry->size, sizeof(entry->size));
    hash_bytes(&ctx->hash, &entry->mtime, sizeof(entry->mtime));
    const uint8_t sep = 0xFFu;
    hash_bytes(&ctx->hash, &sep, 1u);
    ctx->files++;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Pass 2: parse                                                             */
/* ------------------------------------------------------------------------- */

static int32_t sd_scan_read(void *ctx, uint32_t off, void *dst, uint32_t len) {
    sd_scan_view_t *view = (sd_scan_view_t *)ctx;
    if (view == NULL || view->file == NULL) {
        return -1;
    }
    if (off > view->size) {
        return -1;
    }
    uint32_t avail = view->size - off;
    if (len > avail) {
        len = avail;
    }
    if (len == 0u) {
        return 0;
    }
    return storage_fs_read(view->file, off, dst, len);
}

static bool parse_visit(void *vctx, const storage_fs_entry_t *entry, bool is_dir) {
    sd_parse_ctx_t *ctx = (sd_parse_ctx_t *)vctx;
    if (is_dir) {
        return true;
    }
    if (entry->name[0] == '.' || !is_media_file(entry->name)) {
        return true; /* already counted in files_ignored by the key pass */
    }
    if (ctx->index >= ctx->count) {
        return false; /* same cap as the key pass */
    }
    const uint16_t index = ctx->index++;

    storage_fs_file_t *file = storage_fs_open(entry->path);
    if (file == NULL) {
        media_scan_report_t report;
        memset(&report, 0, sizeof(report));
        report.index = index;
        report.count = ctx->count;
        report.name = entry->name;
        report.album_index = 0xFFFFu;
        report.meta_status = (int8_t)MEDIA_ERR_IO;
        ctx->lib->stats.skipped++;
        if (ctx->cb != NULL) {
            ctx->cb(&report);
        }
        return true;
    }

    sd_scan_view_t view;
    view.file = file;
    view.size = entry->size;
    media_source_t src;
    src.read = sd_scan_read;
    src.size = entry->size;
    src.xip_base = NULL;
    src.ctx = &view;

    const int16_t track =
        media_scan_one(ctx->lib, ctx->cb, index, ctx->count, entry->name, &src);
    storage_fs_close(file);

    if (track == -2) {
        ctx->ok = false;
        return false; /* cache write failure */
    }
    if (track >= 0 &&
        !media_scan_write_path(ctx->lib, (uint16_t)track, entry->path)) {
        ctx->ok = false;
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Audio source for a library track                                          */
/* ------------------------------------------------------------------------- */

static int32_t sd_audio_read(void *ctx, uint32_t off, void *dst, uint32_t len) {
    media_library_t *lib = (media_library_t *)ctx;
    if (lib == NULL || lib->sd_audio_file == NULL) {
        return -1;
    }
    if (off > lib->sd_audio_size) {
        return -1;
    }
    uint32_t avail = lib->sd_audio_size - off;
    if (len > avail) {
        len = avail;
    }
    if (len == 0u) {
        return 0;
    }
    return storage_fs_read((storage_fs_file_t *)lib->sd_audio_file,
                           lib->sd_audio_offset + off, dst, len);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool sd_track_open(media_library_t *lib, uint16_t track_index,
                          uint32_t audio_offset, uint32_t audio_size,
                          media_source_t *out) {
    if (lib->sd_audio_file != NULL) {
        storage_fs_close((storage_fs_file_t *)lib->sd_audio_file);
        lib->sd_audio_file = NULL;
    }
    const uint8_t *entry =
        media_cache_ptr(lib->track_path_off + (uint32_t)track_index * 4u);
    if (entry == NULL) {
        return false;
    }
    const uint32_t path_off = rd32(entry);
    const char *path = (const char *)media_cache_ptr(path_off);
    if (path == NULL || path[0] == '\0') {
        return false;
    }

    storage_fs_file_t *file = storage_fs_open(path);
    if (file == NULL) {
        return false;
    }
    const uint32_t file_size = storage_fs_size(file);
    if (audio_offset > file_size) {
        storage_fs_close(file);
        return false;
    }
    if (audio_size > file_size - audio_offset) {
        audio_size = file_size - audio_offset;
    }

    lib->sd_audio_file = file;
    lib->sd_audio_offset = audio_offset;
    lib->sd_audio_size = audio_size;
    out->read = sd_audio_read;
    out->size = audio_size;
    out->xip_base = NULL;
    out->ctx = lib;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Public entry point                                                        */
/* ------------------------------------------------------------------------- */

static void sd_track_close(media_library_t *lib) {
    if (lib->sd_audio_file != NULL) {
        storage_fs_close((storage_fs_file_t *)lib->sd_audio_file);
        lib->sd_audio_file = NULL;
    }
}

bool media_library_scan_sd(media_library_t *lib, const char *root_dir,
                           media_scan_progress_fn cb) {
    if (lib == NULL) {
        return false;
    }
    memset(lib, 0, sizeof(*lib));
    memset(&lib->stats, 0, sizeof(lib->stats));

    const char *root = root_dir;
    if (root == NULL || root[0] == '\0') {
        root = STORAGE_ROOT;
    }

    sd_key_ctx_t key;
    memset(&key, 0, sizeof(key));
    key.hash = 2166136261u;
    hash_bytes(&key.hash, "SDLB", 4u);
    if (storage_fs_walk(root, key_visit, &key) != 0) {
        lib->stats.valid = false;
        return false;
    }
    lib->key = key.hash != 0u ? key.hash : 1u;
    lib->source_kind = MEDIA_LIB_SOURCE_SD;
    lib->stats.files = key.files;
    lib->stats.files_ignored = key.ignored;
    lib->stats.file_cap_hit = key.cap_hit;

    if (media_cache_valid(lib->key) && media_scan_load_cached(lib) &&
        lib->source_kind == MEDIA_LIB_SOURCE_SD) {
        lib->sd_open = sd_track_open;
        lib->sd_close = sd_track_close;
        lib->ready = lib->track_count > 0u;
        lib->stats.valid = lib->ready;
        lib->stats.cache_hit = true;
        return true;
    }

    if (!media_scan_begin(lib, key.files, MEDIA_LIB_SOURCE_SD)) {
        lib->stats.valid = false;
        return false;
    }
    lib->stats.files_ignored = key.ignored;
    lib->stats.file_cap_hit = key.cap_hit;

    sd_parse_ctx_t parse;
    memset(&parse, 0, sizeof(parse));
    parse.lib = lib;
    parse.cb = cb;
    parse.count = key.files;
    parse.ok = true;

    if (storage_fs_walk(root, parse_visit, &parse) != 0 || !parse.ok) {
        lib->stats.valid = false;
        return false;
    }
    if (!media_scan_finish(lib)) {
        lib->stats.valid = false;
        return false;
    }
    lib->sd_open = sd_track_open;
    lib->sd_close = sd_track_close;
    lib->ready = lib->track_count > 0u;
    lib->stats.valid = lib->ready;
    return true;
}
