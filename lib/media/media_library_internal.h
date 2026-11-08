/**
 * @file media_library_internal.h
 * @brief Shared scan engine used by the flash-pack scan (media_library.c) and
 *        the SD scan (media_library_sd.c).
 *
 * The engine writes the track/album/artist records and covers into the flash
 * cache exactly as before; the only difference is where the bytes come from.
 * All entry points run on one core (the boot scan) and use media_library.c's
 * static work area.
 */
#ifndef MEDIA_LIBRARY_INTERNAL_H
#define MEDIA_LIBRARY_INTERNAL_H

#include "media_library.h"

#ifdef __cplusplus
extern "C" {
#endif

/** lib->source_kind values (also stored in the cache root record). */
#define MEDIA_LIB_SOURCE_PACK 0u
#define MEDIA_LIB_SOURCE_SD   1u

/** Longest SD path stored in the cache (must match STORAGE_FS_PATH_MAX). */
#define MEDIA_SD_PATH_MAX 256u

/** Start a full scan: reset the cache and the static work area. */
bool media_scan_begin(media_library_t *lib, uint16_t files, uint8_t source_kind);

/**
 * @brief Parse one file and append its track/album/artist records and art.
 *
 * Reports progress through @p cb exactly like the pack loop does.
 * @return the new track index; -1 when the file was skipped; -2 when writing
 *         the cache failed (the caller must abort the scan).
 */
int16_t media_scan_one(media_library_t *lib, media_scan_progress_fn cb,
                       uint16_t index, uint16_t count, const char *name,
                       const media_source_t *src);

/**
 * @brief Store the SD path of @p track_index in the cache (SD scans only).
 *
 * Called after media_scan_one() accepted the file. The path table is written
 * by media_scan_finish().
 */
bool media_scan_write_path(media_library_t *lib, uint16_t track_index,
                           const char *path);

/** Finish a full scan: placeholders, sorted orders, index records, commit. */
bool media_scan_finish(media_library_t *lib);

/**
 * @brief Try to load the committed index for lib->key (no parsing).
 *
 * Wraps the pack scanner's load_index() so both backends share it.
 */
bool media_scan_load_cached(media_library_t *lib);

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_LIBRARY_INTERNAL_H */
