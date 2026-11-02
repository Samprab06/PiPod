/**
 * @file media_cache.h
 * @brief Append-only scan-result cache in the 1 MB cache region.
 *
 * The cache lives at MEDIA_CACHE_OFFSET and is written once per library scan
 * (library index + decoded album art), then read by the UI.
 *
 * Layout:
 * @code
 *   0x000000  header sector (4 KB), 32-byte header at offset 0:
 *             "PPCH", u16 version, u16 reserved, u32 key, u32 used,
 *             u32 crc32 (over the first 16 bytes), u8 user[12]
 *   0x001000  records: appended 4-byte aligned, programmed in 256-byte pages
 * @endcode
 *
 * The 12 `user` bytes are opaque to this module and written with the header at
 * commit time, so one of them can hold the offset of the cache's own index
 * (see lib/media/media_library.h). They are not covered by the header CRC and
 * are zero after media_cache_reset().
 *
 * Records are opaque byte blobs; the first bytes of each record are the
 * caller's business (index entry, RGB565 art, ...). Allocations never move:
 * `media_cache_ptr(offset)` stays valid until the cache is reset.
 *
 * @warning Flash program/erase stalls all XIP on both cores and the audio
 *          DMA. Only call the write path during the library scan, before
 *          playback, and never while audio is running. On the device the
 *          other core must have called `flash_safe_execute_core_init()` (or
 *          must be idle); see docs/media_design.md "Flash writes". The write
 *          functions are not thread safe: call them from one core only.
 *
 * The same media_cache.c runs on the device and in the simulator.
 */
#ifndef MEDIA_CACHE_H
#define MEDIA_CACHE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEDIA_CACHE_MAGIC        "PPCH"
#define MEDIA_CACHE_VERSION      1u
#define MEDIA_CACHE_HEADER_SIZE  32u
#define MEDIA_CACHE_DATA_OFFSET  4096u /**< first record; sector 0 is header-only */
#define MEDIA_CACHE_SECTOR_SIZE  4096u
#define MEDIA_CACHE_PAGE_SIZE    256u
#define MEDIA_CACHE_USER_SIZE    12u   /**< opaque bytes committed with the header */

/**
 * @brief Prepare the cache for reading/writing.
 *
 * Validates the existing header (magic, version, CRC, bounds). If it is valid,
 * its `used`/`key` are adopted so a crashed scan can continue (the resume
 * offset is rounded up to the next 256-byte flash page so the partially
 * programmed last page of a committed scan is never rewritten). If the header
 * is invalid (blank flash, old format, corruption) the header sector is erased
 * and an empty cache is created.
 *
 * Call media_cache_reset() first when the pack changed and old records must
 * not be appended to.
 *
 * @return false if flash access fails.
 */
bool media_cache_begin(void);

/**
 * @brief Erase the header sector and start an empty cache (used = 4 KB).
 * @return false if flash access fails.
 */
bool media_cache_reset(void);

/**
 * @brief Append @p len bytes to the cache.
 *
 * @p out_offset receives the 4-byte aligned offset of the new record inside
 * the cache region (use media_cache_ptr() to read it back). Data is buffered
 * in a 4 KB RAM page and programmed in 256-byte pages, erasing 4 KB sectors as
 * the append crosses them, so a record may span sectors. Whole pages are
 * programmed immediately; the final partial page is programmed by
 * media_cache_commit(), so records are guaranteed readable (and durable) only
 * after a commit.
 *
 * @return false when the write fails or the 1 MB region is full.
 */
bool media_cache_write(const void *data, uint32_t len, uint32_t *out_offset);

/**
 * @brief Write the cache header with @p key and the current used size.
 *
 * A previously committed cache with a different key becomes invalid, which is
 * how a changed media pack discards old scan results. Call after the last
 * media_cache_write(). Never call during playback.
 *
 * The recorded size is rounded up to the next 4 KB sector, so a later append
 * starts in a fresh sector and never erases committed data (up to 4 KB is
 * left unused). Key 0 is reserved for "empty cache" and never validates.
 *
 * @return false if flash access fails.
 */
bool media_cache_commit(uint32_t key);

/**
 * @brief Like media_cache_commit(), but also stores 12 opaque user bytes.
 *
 * The bytes are written together with the header (no extra erase/program) and
 * returned by media_cache_user() after the cache is committed. A caller can
 * put the offset of its own index block there so it does not have to reserve a
 * fixed cache offset.
 *
 * @param user 12 bytes to store, or NULL to store zeros.
 * @return false if flash access fails.
 */
bool media_cache_commit_ex(uint32_t key, const uint8_t user[MEDIA_CACHE_USER_SIZE]);

/**
 * @brief Read back the 12 user bytes stored with the committed header.
 *
 * Purely a flash read (no media_cache_begin() needed). The buffer is zeroed
 * when the header is invalid or no user bytes were stored.
 *
 * @return true if a valid cache header was found.
 */
bool media_cache_user(uint8_t user[MEDIA_CACHE_USER_SIZE]);

/**
 * @brief Does a committed cache for @p key exist?
 *
 * Purely a flash read: safe from any core, does not need
 * media_cache_begin(). Key 0 always returns false.
 */
bool media_cache_valid(uint32_t key);

/** @brief Bytes used so far in the cache region (including the header sector). */
uint32_t media_cache_used(void);

/**
 * @brief Pointer to cache offset @p offset (XIP on device, RAM image on host).
 * @return NULL when @p offset is outside the cache region.
 */
const uint8_t *media_cache_ptr(uint32_t offset);

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_CACHE_H */
