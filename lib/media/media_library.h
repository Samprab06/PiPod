/**
 * @file media_library.h
 * @brief Library index built from the PPAK media pack: scan once, cache it in
 *        flash, serve the UI from the cache with a small RAM index.
 *
 * The flow at boot (core 0, before the UI core starts and before any audio):
 *
 * @code
 * static media_library_t lib;
 * if (media_library_scan(&lib, report_cb) && media_library_stats(&lib)->tracks > 0) {
 *     const ui_library_t *ui_lib = media_library_ui(&lib);   // hand to ui_init()
 * }
 * @endcode
 *
 * media_library_scan() opens the PPAK pack at MEDIA_PACK_OFFSET. When the scan
 * cache already holds an index for this pack (media_pack_cache_key() matches)
 * it loads the cached index instead of parsing any tags: a cache hit performs
 * zero media_meta_parse() calls and zero JPEG decodes. Otherwise it parses
 * every file once, decodes one cover per album, writes the index and the
 * covers to the cache region and commits it.
 *
 * Flash writes only happen during this scan. Call it before the second core is
 * started (see docs/media_design.md "Flash writes").
 *
 * On-flash cache layout (all offsets are cache-region offsets, see
 * media_cache.h; records are 4-byte aligned):
 *
 * @code
 *   0x000000  media_cache header sector (the 12 user bytes hold "PPLB",
 *             the root offset and the layout version)
 *   0x001000  track blocks      160 bytes each  (metadata + title + artist)
 *             album covers      28,800 bytes each (120x120 RGB565)
 *   ...        track offset table u32[track_count]
 *             artist records    56 bytes each
 *             album records     64 bytes each
 *             sorted orders     u16[artist_count] x 3
 *                               u16[track_count]  x 2
 *             root record       64 bytes (last record; its offset is in the
 *                               cache header's user bytes)
 * @endcode
 *
 * The covers are interleaved with the track blocks (a cover is written when
 * the album is first seen). Every album gets a decoded cover or a seeded
 * placeholder while the cache has room; the cache holds at most ~29 covers
 * (1 MB region, 28,800 bytes each, minus the index), so albums that do not fit
 * get no cached image and the UI draws its own placeholder for them.
 *
 * RAM: after the scan the index vectors are kept in the library struct
 * (uint16 ids only); every string is read from the cache through
 * media_cache_ptr(). sizeof(media_library_t) is about 6 KB. The transient scan
 * buffers (titles for sorting, album/artist tables, one 120x120 RGB565 cover
 * and the TJpgDec pool) are static in media_library.c: about 60 KB that stay
 * allocated in .bss (there is no malloc after init).
 */
#ifndef MEDIA_LIBRARY_H
#define MEDIA_LIBRARY_H

#include <stdbool.h>
#include <stdint.h>

#include "media_meta.h"
#include "media_pack.h"
#include "media_source.h"
#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Static capacities. Larger packs are truncated (and reported in the stats). */
#define MEDIA_LIB_MAX_TRACKS  512u
#define MEDIA_LIB_MAX_ALBUMS  128u
#define MEDIA_LIB_MAX_ARTISTS 128u

/**
 * Safety cap on the number of audio files an SD scan indexes. The library only
 * keeps MEDIA_LIB_MAX_TRACKS, but the scan still has to walk every file to
 * build the cache key; 4096 keeps a pathological card from stalling the boot.
 */
#define MEDIA_SD_MAX_FILES 4096u

/*
 * Library ids. Artist/album/track ids never collide with each other and are
 * never 0, so a 0 id can be used as "none" by callers.
 */
#define MEDIA_LIB_ARTIST_ID_BASE  0x00010000u
#define MEDIA_LIB_ALBUM_ID_BASE   0x00020000u
#define MEDIA_LIB_TRACK_ID_BASE   0x00030000u
#define MEDIA_LIB_PLAYLIST_ALL_ID 0x00040000u

/** Built-in playlist count (one: "All Songs"). */
#define MEDIA_LIB_PLAYLIST_COUNT  1u

/** Result of the album cover attempt, for the boot report. */
typedef enum {
    MEDIA_LIB_ART_NONE = 0,        /**< no room in the cache: UI placeholder */
    MEDIA_LIB_ART_DECODED = 1,     /**< embedded JPEG decoded and cached */
    MEDIA_LIB_ART_PLACEHOLDER = 2, /**< seeded placeholder cached */
} media_album_art_state_t;

/** One progress callback invocation per pack entry. */
typedef struct {
    uint16_t index;            /**< 0-based pack entry index */
    uint16_t count;            /**< pack file count */
    const char *name;          /**< TOC path (UTF-8, NUL-terminated) */
    const media_meta_t *meta;  /**< parsed metadata, NULL when skipped */
    int8_t meta_status;        /**< media_status_t result */
    uint16_t album_index;      /**< library album index, 0xFFFF when skipped */
    uint8_t album_art;         /**< media_album_art_state_t of that album */
} media_scan_report_t;

/** Called for every pack entry during a full scan (never on a cache hit).
 *  Runs on the scan core; keep it short (a UART print is fine). */
typedef void (*media_scan_progress_fn)(const media_scan_report_t *report);

/** Counters describing the last scan. */
typedef struct {
    bool     valid;          /**< an index is loaded and ready */
    bool     cache_hit;      /**< loaded from the cache, no parsing */
    uint16_t files;          /**< pack entries (SD: media files found) */
    uint16_t parsed;         /**< entries media_meta_parse() accepted */
    uint16_t skipped;        /**< unsupported/corrupt/over-capacity entries */
    uint16_t tracks;         /**< library tracks */
    uint16_t albums;
    uint16_t artists;
    uint32_t parse_calls;    /**< media_meta_parse() calls made by this scan */
    uint32_t art_decodes;    /**< media_art_decode_120() calls */
    uint16_t art_albums;     /**< albums with a cached cover (real or placeholder) */
    uint16_t art_placeholders; /**< of art_albums, how many are placeholders */
    uint32_t cache_bytes;    /**< media_cache_used() after the scan */
    uint16_t files_ignored;  /**< SD: non-audio files/dirs skipped by extension */
    bool     file_cap_hit;   /**< SD: MEDIA_SD_MAX_FILES reached, scan truncated */
} media_scan_stats_t;

/** Full metadata of one library track, for the player and the decoder. */
typedef struct {
    uint32_t id;          /**< MEDIA_LIB_TRACK_ID_BASE + index */
    uint16_t index;       /**< 0-based library index */
    uint32_t album_id;    /**< MEDIA_LIB_ALBUM_ID_BASE + album index */
    char     title[MEDIA_TITLE_MAX];
    char     artist[MEDIA_NAME_MAX];
    char     album[MEDIA_NAME_MAX];
    uint16_t disc, disc_total;
    uint16_t track, track_total;
    uint32_t duration_ms;
    uint32_t sample_rate;
    uint32_t bitrate_kbps;
    uint8_t  channels;
    uint8_t  bits_per_sample;
    uint8_t  format;      /**< media_format_t */

    uint32_t audio_offset; /**< first audio byte inside the pack file */
    uint32_t audio_size;   /**< audio byte count */
    /**
     * Audio byte source for the future decoder: source.size == audio_size and
     * source.xip_base points at the first audio byte in flash, so a decoder
     * can DMA straight from XIP (see lib/media/media_stream.h). source.read()
     * is a memcpy helper for the same range. The source is valid until the
     * next media_library_track() call on the same library (one shared view).
     */
    media_source_t source;
} media_track_info_t;

/** One library, statically allocated by the app. Fields are private; use the
 *  accessors below. */
typedef struct media_library media_library_t;

/**
 * Private hook installed by media_library_sd.c: open the file backing track
 * @p track_index read-only and describe its audio payload. Kept as a function
 * pointer so the flash-pack-only build (and its host test) does not need to
 * link the SD backend.
 */
typedef bool (*media_library_sd_open_fn)(media_library_t *lib,
                                         uint16_t track_index,
                                         uint32_t audio_offset,
                                         uint32_t audio_size,
                                         media_source_t *out);

/** Private hook that closes the SD audio file, if any (see media_library_release). */
typedef void (*media_library_sd_close_fn)(media_library_t *lib);

struct media_library {
    /* private */
    media_pack_t pack;
    bool         ready;
    uint32_t     key;
    uint16_t     track_count;
    uint16_t     album_count;
    uint16_t     artist_count;
    uint32_t     artist_off;
    uint32_t     album_off;
    uint32_t     track_table[MEDIA_LIB_MAX_TRACKS];
    uint16_t     order_artist[MEDIA_LIB_MAX_ARTISTS];
    uint16_t     order_album[MEDIA_LIB_MAX_ALBUMS];
    uint16_t     order_album_artist[MEDIA_LIB_MAX_ALBUMS];
    uint16_t     order_track_title[MEDIA_LIB_MAX_TRACKS];
    uint16_t     order_track_album[MEDIA_LIB_MAX_TRACKS];
    gfx_image_t  art[MEDIA_LIB_MAX_ALBUMS];
    media_scan_stats_t stats;
    ui_library_t ui;
    struct {
        const uint8_t *base;
        uint32_t size;
    } audio_view;

    /* SD backend (zero for a flash-pack library). */
    uint8_t  source_kind;      /**< MEDIA_LIB_SOURCE_PACK / _SD */
    uint32_t track_path_off;   /**< cache offset of u32[track_count] path offsets */
    void    *sd_audio_file;    /**< open storage_fs_file_t for the current track */
    uint32_t sd_audio_offset;  /**< payload start inside that file */
    uint32_t sd_audio_size;    /**< payload size of that track */
    media_library_sd_open_fn sd_open;
    media_library_sd_close_fn sd_close;
};

/**
 * @brief Open the pack and build (or load) the library index.
 *
 * @param lib  caller-owned, zero-initialised storage (a static is expected).
 * @param cb   optional per-file progress callback; only called on a full scan.
 * @return true when an index for the current pack is ready. A pack with zero
 *         usable tracks still returns true (check stats.tracks), so the caller
 *         can distinguish "no pack" from "empty pack".
 */
bool media_library_scan(media_library_t *lib, media_scan_progress_fn cb);

/**
 * @brief Build (or load) the library index from an SD card directory tree.
 *
 * Walks @p root_dir recursively (depth <= STORAGE_FS_MAX_DEPTH), accepts
 * *.mp3, *.flac, *.wav, *.ogg and *.opus (case-insensitive), parses tags and
 * embedded art with the same code as media_library_scan(), and fills the same
 * library structures and ui_library_t. The flash scan cache is reused: the key
 * is a hash of the file list (path, size and FAT modification stamp), so a
 * second boot with the same card loads the index and all covers without
 * touching any file on the card except the directory walk needed for the key.
 *
 * Mount the card first (storage_mount()); this function also mounts it lazily
 * through the storage backend. The card is never written to.
 *
 * The 1 MB flash cache holds one index at a time: an SD scan replaces a cached
 * flash-pack index and vice versa; the source kind is stored in the cache root
 * record, so each scanner ignores a cache written by the other.
 *
 * @param lib      caller-owned, zero-initialised storage (a static is expected).
 * @param root_dir directory to scan, e.g. STORAGE_ROOT or "0:/Music"; NULL or
 *                 "" means STORAGE_ROOT.
 * @param cb       optional per-file progress callback (full scans only).
 * @return true when an index is ready. On a mount failure or an unreadable
 *         card, false and stats.valid == false.
 */
bool media_library_scan_sd(media_library_t *lib, const char *root_dir,
                           media_scan_progress_fn cb);

/** @brief ui_library_t view for ui_init(), or NULL when no index is ready. */
const ui_library_t *media_library_ui(media_library_t *lib);

/** @brief Last-scan counters (never NULL; valid=false before the first scan). */
const media_scan_stats_t *media_library_stats(const media_library_t *lib);

/** @brief True when an index is ready and has at least one track. */
bool media_library_ready(const media_library_t *lib);

/**
 * @brief Fill full metadata and the audio byte source for @p track_id.
 * @return false for an unknown id (or a library that is not ready).
 */
bool media_library_track(media_library_t *lib, uint32_t track_id,
                         media_track_info_t *out);

/**
 * @brief Release the open SD audio file of the current track, if any.
 *
 * The library stays valid and the next media_library_track() reopens the file.
 * Call this before storage_unmount() or before scanning another library while
 * a track source is still open: the storage layer has a small fixed number of
 * open-file slots (STORAGE_FS_MAX_OPEN). A no-op for flash-pack libraries.
 */
void media_library_release(media_library_t *lib);

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_LIBRARY_H */
