/**
 * @file media_art.h
 * @brief Album art decoding: embedded JPEG -> 120x120 RGB565, without malloc.
 *
 * Library scan path: media_meta_parse() (media_meta.h) reports where the
 * picture bytes live in the file as a media_art_ref_t; this module decodes
 * them into a caller-owned 120x120 RGB565 buffer that is then written to the
 * flash cache region and handed to the UI as a `gfx_image_t` (lib/gfx/
 * gfx_image.h), width and height = ::MEDIA_ART_SIZE.
 *
 * Pipeline, all streaming through media_source_t (no assumption that the whole
 * picture fits in RAM):
 *  - filter the encoded byte range into a JPEG byte stream: plain (RAW), ID3v2
 *    unsynchronised 0xFF 0x00 -> 0xFF (ID3_UNSYNC), or base64 text decoded and
 *    then restricted to [inner_offset, inner_offset + inner_size) so the FLAC
 *    PICTURE / Ogg METADATA_BLOCK_PICTURE wrapper is skipped (BASE64);
 *  - TJpgDec R0.03 (third_party/tjpgd) with JD_FORMAT 1 (RGB565) and
 *    JD_USE_SCALE 1: the 1/2, 1/4 or 1/8 prescale is chosen so that the
 *    shorter side of the scaled image is >= ::MEDIA_ART_SIZE and as small as
 *    possible (so the prescaled image is at most twice the target size in
 *    each dimension);
 *  - centre crop to a square and area-average ("box filter") resize to
 *    ::MEDIA_ART_SIZE x ::MEDIA_ART_SIZE. The same exact integer area weights
 *    both downscale and upscale (artwork smaller than 120x120).
 *
 * Only baseline JPEG (SOF0) is decodable. TJpgDec does not implement
 * progressive JPEG, so those (and PNG, WebP, ...) return
 * ::MEDIA_ART_ERR_FORMAT: the caller should fall back to
 * media_art_placeholder_120().
 *
 * Pixel format: native `uint16_t` 0xRRRRRGGGGGGBBBBB (5-6-5), row-major, no
 * padding - exactly what lib/gfx expects and what the ILI9341 takes, so the
 * decoded image can be DMA'd from the cache region as-is.
 *
 * Memory: nothing is allocated here. The caller provides
 *  - `out_rgb565`: ::MEDIA_ART_PIXELS * 2 = 28800 bytes,
 *  - `work`: ::MEDIA_ART_WORK_SIZE bytes (4-byte aligned) for the TJpgDec
 *    session.
 * Inside the call the scaler keeps two output-row accumulators (~2.9 KB) and
 * the filtering reader on the stack, and one static 7.7 KB buffer holds the
 * cropped part of the MCU band that TJpgDec is currently decoding (TJpgDec
 * hands out whole MCUs, column by column, so rows need re-ordering). That
 * static buffer makes the decoder non-reentrant: call it from one task (the
 * library scan on core 0), not concurrently from the UI.
 * The decode runs once per file at scan time, so a single shared work buffer
 * and output buffer can be reused for every file.
 *
 * ::MEDIA_ART_OK is 0 and every other code is negative, so test success with
 * `status == MEDIA_ART_OK`, not with `status <= 0`. ::MEDIA_ART_ERR_NONE is
 * also a non-failure ("this file has no art"): fall back to the placeholder.
 */
#ifndef MEDIA_ART_H
#define MEDIA_ART_H

#include <stddef.h>
#include <stdint.h>

#include "media_meta.h"
#include "media_source.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Edge length of the decoded square, in pixels (UI layout decision). */
#define MEDIA_ART_SIZE 120
/** Number of RGB565 pixels in one decoded image (28800 bytes). */
#define MEDIA_ART_PIXELS (MEDIA_ART_SIZE * MEDIA_ART_SIZE)

/**
 * Size of the TJpgDec working pool, in bytes; must be 4-byte aligned.
 *
 * Derived from the TJpgDec allocator (jd_prepare) with JD_FASTDECODE 1,
 * JD_FORMAT 1 and JD_SZBUF 512, worst case of the supported subset:
 *   512  input buffer (JD_SZBUF)
 * +1108 two DC + two AC Huffman tables at the ITU-T.81 sizes (12/162 symbols)
 * +1024 four quantisation tables (4 x 64 x int32)
 * + 576 MCU/IDCT work buffer for a 16x16 (4:2:0) MCU
 * + 768 MCU buffer, (4 + 2) x 64 x int16
 * ------
 *  3988, rounded up to 4096.
 * The common case (JPEGs carrying two quantisation tables) needs 3476 bytes;
 * the extra 512 keep files with four tables working. Files whose tables are
 * larger than the ITU-T.81 examples fail with ::MEDIA_ART_ERR_DECODE.
 * lib/media/test/art_host_test.c measures the real requirement of every
 * fixture through TJpgDec and reports it.
 */
#define MEDIA_ART_WORK_SIZE 4096

/** Result of a decode attempt. */
typedef enum {
    MEDIA_ART_OK = 0,       /**< 120x120 RGB565 written to out_rgb565. */
    MEDIA_ART_ERR_NONE = -1, /**< the reference holds no art (size 0 / MEDIA_ART_NONE). */
    MEDIA_ART_ERR_FORMAT = -2, /**< not baseline JPEG, progressive JPEG, or corrupt stream. */
    MEDIA_ART_ERR_DECODE = -3, /**< JPEG valid but TJpgDec failed (memory, geometry, internal). */
    MEDIA_ART_ERR_IO = -4,     /**< media_source_t read error / file shorter than the reference. */
} media_art_status_t;

/**
 * Decode the album art referenced by @p ref into a 120x120 RGB565 image.
 *
 * @param src        file the picture lives in (media_source.h).
 * @param ref        picture location/encoding from media_meta_parse(); may be
 *                   NULL, in which case ::MEDIA_ART_ERR_NONE is returned.
 * @param out_rgb565 ::MEDIA_ART_PIXELS native uint16_t, row-major, 5-6-5.
 *                   Unspecified unless ::MEDIA_ART_OK is returned.
 * @param work       caller buffer of at least ::MEDIA_ART_WORK_SIZE bytes,
 *                   4-byte aligned (the TJpgDec pool).
 * @param work_len   size of @p work in bytes.
 * @retval MEDIA_ART_OK        decoded.
 * @retval MEDIA_ART_ERR_NONE  @p ref is NULL / empty: nothing to draw, use the
 *                             placeholder.
 * @retval MEDIA_ART_ERR_FORMAT unsupported container type (PNG, ...),
 *                             progressive JPEG, or corrupt JPEG data.
 * @retval MEDIA_ART_ERR_DECODE TJpgDec could not decode the stream (bad
 *                             parameters, work buffer too small, geometry).
 * @retval MEDIA_ART_ERR_IO    the source reported a read error.
 *
 * @note `src`, @p out_rgb565 and @p work must not be NULL with
 *       @p work_len < ::MEDIA_ART_WORK_SIZE: those are programming errors and
 *       also return ::MEDIA_ART_ERR_DECODE.
 * @note Not reentrant: the cropped MCU band lives in a static buffer (see the
 *       file-level comment). Safe on the scan task only.
 */
media_art_status_t media_art_decode_120(const media_source_t *src, const media_art_ref_t *ref,
                                        uint16_t *out_rgb565, void *work, size_t work_len);

/**
 * Draw placeholder art when a file has no embedded picture (or one this
 * decoder cannot handle): a seeded two-tone gradient with a soft vignette and
 * a centred music-note glyph, exactly ::MEDIA_ART_SIZE squared.
 *
 * @param out_rgb565 destination, ::MEDIA_ART_PIXELS native uint16_t.
 * @param seed       any value; the same seed always draws the same image, so
 *                   the placeholder is stable per file (e.g. a hash of the
 *                   file name or the TOC index).
 */
void media_art_placeholder_120(uint16_t *out_rgb565, uint32_t seed);

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_ART_H */
