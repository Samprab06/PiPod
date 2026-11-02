/**
 * @file media_pack.h
 * @brief Read-only "PPAK" media pack stored in the onboard QSPI flash.
 *
 * The pack is produced on the PC by scripts/pack_media.py and written with
 * picotool (or its UF2 form). Format (little-endian), see
 * docs/media_design.md:
 *
 * @code
 * header (32 bytes): "PPAK", u16 version=1, u16 file_count, u32 total_size,
 *                    u32 toc_offset, u32 toc_crc32, u8 reserved[12]
 * toc entry (80 bytes): char name[64], u32 offset, u32 size, u32 crc32,
 *                       u32 flags
 * @endcode
 *
 * Offsets are relative to the start of the pack and 4096-aligned; file data
 * is contiguous. `total_size` is the whole pack image (header + TOC + padding +
 * data) and must not exceed MEDIA_PACK_MAX.
 *
 * The pack lives at MEDIA_PACK_OFFSET and is read through the XIP window (or
 * the simulator's file-backed flash image), so `media_pack_source()` hands out
 * a media_source_t whose `xip_base` points straight at flash: callers may DMA
 * from it.
 *
 * The same media_pack.c runs on the device and in the simulator.
 */
#ifndef MEDIA_PACK_H
#define MEDIA_PACK_H

#include <stdbool.h>
#include <stdint.h>

#include "media_source.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MEDIA_PACK_MAGIC        "PPAK"
#define MEDIA_PACK_VERSION      1u
#define MEDIA_PACK_HEADER_SIZE  32u
#define MEDIA_PACK_TOC_ENTRY    80u
#define MEDIA_PACK_NAME_MAX     64u
#define MEDIA_PACK_DATA_ALIGN   4096u

/**
 * Number of media_source_t objects media_pack_source() can keep alive at the
 * same time. Each call hands out one slot; reusing a slot invalidates the
 * source that used it. Parse one file at a time (or use
 * media_pack_source_ex() with caller-owned storage) to be safe.
 */
#define MEDIA_PACK_MAX_SOURCES  4

/** One file of the pack, resolved to a flash pointer. */
typedef struct media_pack_file {
    const uint8_t *data; /**< XIP/flash pointer to byte 0. */
    uint32_t size;
    uint32_t crc32;
} media_pack_file_t;

typedef struct media_pack {
    const uint8_t *base;  /**< pack start (flash offset MEDIA_PACK_OFFSET). */
    uint32_t total_size;
    uint32_t toc_offset;
    uint32_t toc_crc32;
    uint16_t count;
    uint8_t next_slot;
    media_pack_file_t sources[MEDIA_PACK_MAX_SOURCES];
} media_pack_t;

/**
 * @name CRC-32 (IEEE 802.3, zlib compatible)
 * Identical to Python's zlib.crc32(); table-less nibble implementation.
 * @{
 */
uint32_t media_crc32(const void *data, uint32_t len);
uint32_t media_crc32_update(uint32_t crc, const void *data, uint32_t len);
/** @} */

/**
 * @brief Open and validate the pack at MEDIA_PACK_OFFSET.
 *
 * Checks the header, the TOC CRC and every entry (name, alignment, bounds).
 * Metadata only: file contents are not read.
 *
 * @return false if there is no pack or it is corrupt/incompatible.
 */
bool media_pack_open(media_pack_t *p);

uint16_t media_pack_count(const media_pack_t *p);
uint32_t media_pack_total_size(const media_pack_t *p);
uint32_t media_pack_toc_crc(const media_pack_t *p);

/**
 * @brief Cache key derived from the pack TOC CRC.
 *
 * A different pack (different files) yields a different key, so a stale scan
 * cache is invalidated. Never returns 0 (0 means "empty cache").
 */
uint32_t media_pack_cache_key(const media_pack_t *p);

/** @brief Name (POSIX relative path, UTF-8, NUL-terminated) or NULL. */
const char *media_pack_name(const media_pack_t *p, uint16_t index);

/** @brief Resolve entry @p index to a flash pointer/size/crc. */
bool media_pack_file_at(const media_pack_t *p, uint16_t index,
                        media_pack_file_t *out);

/**
 * @brief Build a media_source_t for entry @p index.
 *
 * `out->read` memcpy()s from flash (fine for metadata; bulk audio should use
 * `out->xip_base` with DMA), `out->size` is the file size and `out->xip_base`
 * the XIP address of byte 0.
 */
bool media_pack_source(media_pack_t *p, uint16_t index, media_source_t *out);

/**
 * @brief Like media_pack_source(), but the descriptor lives in @p storage,
 *        owned by the caller (no slot limit).
 */
bool media_pack_source_ex(const media_pack_t *p, uint16_t index,
                          media_pack_file_t *storage, media_source_t *out);

/** @brief Recompute the CRC of file @p index and compare it with the TOC. */
bool media_pack_verify(const media_pack_t *p, uint16_t index);

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_PACK_H */
