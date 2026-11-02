/**
 * @file media_flash_host.h
 * @brief PC simulator backend for the 16 MB flash image.
 *
 * The simulator backs the whole flash with a file (default `sim/out/flash.bin`)
 * so the device media pack and cache code runs unchanged on the host. This
 * header is host-only: it is never included by device code.
 */
#ifndef MEDIA_FLASH_HOST_H
#define MEDIA_FLASH_HOST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Load (or create) the 16 MB file-backed flash image.
 *
 * @param path Image path, or NULL for the default `sim/out/flash.bin`.
 *             A new image is erased (0xFF) and written to disk.
 * @return false on I/O error or a file of the wrong size.
 *
 * Call once at startup, before media_pack_open()/media_cache_begin(): the
 * pointers they cache would dangle across a re-init.
 */
bool media_host_flash_init(const char *path);

/**
 * @brief Place a `pack.bin` at flash offset @p offset and flush it.
 * @return false on I/O error or if it does not fit in the flash image.
 */
bool media_host_flash_load(uint32_t offset, const char *path);

/** @brief Flush the in-memory image to the file (program/erase already do). */
void media_host_flash_flush(void);

/**
 * @brief Close the image file and free the buffer (tests / shutdown).
 *
 * Pointers previously returned by media_flash_ptr()/media_cache_ptr() become
 * invalid.
 */
void media_host_flash_close(void);

/** @brief Raw image pointer (tests poke bytes here); NULL before init. */
uint8_t *media_host_flash_buffer(void);

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_FLASH_HOST_H */
