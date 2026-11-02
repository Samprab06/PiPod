/**
 * @file media_source.h
 * @brief Random-access byte source for one media file.
 *
 * Every parser and decoder reads files through this interface, so the same
 * code works on the onboard flash pack today, on the SD card later and on a
 * plain file in the PC simulator.
 */
#ifndef MEDIA_SOURCE_H
#define MEDIA_SOURCE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct media_source {
    /**
     * Read up to @p len bytes at byte offset @p off into @p dst.
     * @return bytes read (short only at end of file), or -1 on I/O error.
     */
    int32_t (*read)(void *ctx, uint32_t off, void *dst, uint32_t len);
    /** Total file size in bytes. */
    uint32_t size;
    /**
     * Flash (XIP) address of byte 0 when the file is stored contiguously in
     * memory-mapped flash, else NULL. Lets callers DMA directly from flash.
     */
    const uint8_t *xip_base;
    void *ctx;
} media_source_t;

/** Convenience wrapper: read exactly @p len bytes, false on short read or error. */
static inline bool media_read_exact(const media_source_t *src, uint32_t off, void *dst, uint32_t len) {
    return src->read(src->ctx, off, dst, len) == (int32_t)len;
}

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_SOURCE_H */
