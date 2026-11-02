/**
 * @file flash_layout.h
 * @brief The 16 MB onboard QSPI flash map, shared by the device and the PC
 *        simulator.
 *
 * The device and the simulator run exactly the same media pack and cache code;
 * only the two tiny flash primitives below differ (XIP pointer + erase/program).
 *
 * @code
 *   0x000000  +----------------------------+
 *             | firmware (max 1 MB)        |
 *   0x100000  +----------------------------+
 *             | PPAK media pack (max 14 MB)|
 *   0xF00000  +----------------------------+
 *             | scan cache (1 MB)          |
 *   0x1000000 +----------------------------+
 * @endcode
 *
 * All offsets in this header are *flash offsets* (0 .. MEDIA_FLASH_SIZE), not
 * XIP addresses. The device sees flash offset 0 at XIP address
 * @ref MEDIA_XIP_BASE (0x10000000).
 */
#ifndef MEDIA_FLASH_LAYOUT_H
#define MEDIA_FLASH_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEDIA_FLASH_SIZE   0x01000000u /**< 16 MB QSPI flash. */
#define MEDIA_PACK_OFFSET  0x00100000u /**< Media pack start (1 MB). */
#define MEDIA_PACK_MAX     0x00E00000u /**< Media pack capacity (14 MB). */
#define MEDIA_CACHE_OFFSET 0x00F00000u /**< Scan cache start (15 MB). */
#define MEDIA_CACHE_SIZE   0x00100000u /**< Scan cache size (1 MB). */
#define MEDIA_FIRMWARE_MAX 0x00100000u /**< Firmware must fit in 1 MB. */

/** XIP address of flash offset 0 on the RP2350. */
#define MEDIA_XIP_BASE     0x10000000u

/** QSPI erase/program granularity (also used by the simulator backend). */
#define MEDIA_FLASH_SECTOR_SIZE 4096u
#define MEDIA_FLASH_PAGE_SIZE   256u

#if defined(__cplusplus)
#define MEDIA_FLASH_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define MEDIA_FLASH_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

MEDIA_FLASH_STATIC_ASSERT(MEDIA_FIRMWARE_MAX <= MEDIA_PACK_OFFSET,
                          "firmware region overlaps the media pack");
MEDIA_FLASH_STATIC_ASSERT(MEDIA_PACK_OFFSET + MEDIA_PACK_MAX == MEDIA_CACHE_OFFSET,
                          "media pack must end exactly at the cache region");
MEDIA_FLASH_STATIC_ASSERT(MEDIA_CACHE_OFFSET + MEDIA_CACHE_SIZE == MEDIA_FLASH_SIZE,
                          "cache region must end exactly at the end of flash");
MEDIA_FLASH_STATIC_ASSERT(MEDIA_CACHE_SIZE >= 2u * 4096u,
                          "cache region must contain a header sector plus data");

/**
 * @brief Pointer to flash offset @p offset.
 *
 * On the device this is plain XIP: `XIP_BASE + offset`. On the host it points
 * into the 16 MB file-backed image created by `media_host_flash_init()`.
 *
 * @return a valid pointer for 0 <= offset < MEDIA_FLASH_SIZE, NULL otherwise
 *         (the device does not bounds-check).
 */
#if defined(PICO_ON_DEVICE)
static inline const uint8_t *media_flash_ptr(uint32_t offset) {
    return (const uint8_t *)(uintptr_t)(MEDIA_XIP_BASE + offset);
}
#else
const uint8_t *media_flash_ptr(uint32_t offset);
#endif

/**
 * @brief Erase @p size bytes of flash starting at @p offset (0xFF filled).
 *
 * Both values must be multiples of the 4 KB sector size.
 *
 * On the device this is `flash_range_erase()` wrapped in
 * `flash_safe_execute()` with a 2 s enter/exit timeout. Callers must ensure:
 *  - the other core has called `flash_safe_execute_core_init()` (or is idle),
 *  - this never runs during audio playback (an erase stalls all XIP),
 *  - it only runs during the library scan.
 *
 * @return true on success.
 */
bool media_flash_erase(uint32_t offset, uint32_t size);

/**
 * @brief Program @p size bytes from @p data at flash offset @p offset.
 *
 * @p offset and @p size must be multiples of the 256-byte flash page size and
 * the target must have been erased first. Same core-safety rules as
 * media_flash_erase().
 *
 * @return true on success.
 */
bool media_flash_program(uint32_t offset, const void *data, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_FLASH_LAYOUT_H */
