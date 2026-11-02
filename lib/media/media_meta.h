/**
 * @file media_meta.h
 * @brief Tag and stream-info extraction for MP3, FLAC, WAV and Ogg (Vorbis/Opus).
 *
 * No allocation: the caller supplies a scratch buffer. All strings are
 * NUL-terminated UTF-8, truncated on a character boundary.
 */
#ifndef MEDIA_META_H
#define MEDIA_META_H

#include <stddef.h>
#include <stdint.h>

#include "media_source.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MEDIA_FMT_UNKNOWN = 0,
    MEDIA_FMT_MP3,
    MEDIA_FMT_FLAC,
    MEDIA_FMT_WAV,
    MEDIA_FMT_OGG_VORBIS,
    MEDIA_FMT_OGG_OPUS,
} media_format_t;

typedef enum {
    MEDIA_ART_NONE = 0,
    MEDIA_ART_JPEG,
    MEDIA_ART_PNG,
    MEDIA_ART_OTHER,
} media_art_type_t;

/** How the embedded picture bytes are stored inside the file. */
typedef enum {
    MEDIA_ART_ENC_RAW = 0,      /**< plain bytes at [offset, offset+size) */
    MEDIA_ART_ENC_ID3_UNSYNC,   /**< ID3v2 unsynchronised: every 0xFF 0x00 means 0xFF */
    MEDIA_ART_ENC_BASE64,       /**< base64 text (Ogg METADATA_BLOCK_PICTURE), see art.inner_* */
} media_art_enc_t;

typedef struct {
    uint32_t offset;     /**< file offset of the encoded picture bytes */
    uint32_t size;       /**< encoded length in the file */
    uint8_t type;        /**< media_art_type_t */
    uint8_t encoding;    /**< media_art_enc_t */
    uint16_t width;      /**< from the container if known, else 0 */
    uint16_t height;
    /** For BASE64: offset/length of the image data inside the *decoded* block
     *  (it is wrapped in a FLAC PICTURE structure). Unused otherwise. */
    uint32_t inner_offset;
    uint32_t inner_size;
} media_art_ref_t;

#define MEDIA_TITLE_MAX 64
#define MEDIA_NAME_MAX 48
#define MEDIA_GENRE_MAX 32

typedef struct {
    uint8_t format;                   /**< media_format_t */
    char title[MEDIA_TITLE_MAX];      /**< falls back to the file name (no extension) when untagged */
    char artist[MEDIA_NAME_MAX];
    char album[MEDIA_NAME_MAX];
    char album_artist[MEDIA_NAME_MAX];
    char genre[MEDIA_GENRE_MAX];
    uint16_t track, track_total;      /**< 0 = unknown */
    uint16_t disc, disc_total;
    uint16_t year;
    uint32_t duration_ms;             /**< 0 = unknown */
    uint32_t sample_rate;
    uint32_t bitrate_kbps;            /**< average */
    uint8_t channels;
    uint8_t bits_per_sample;          /**< 0 for lossy formats */
    uint32_t audio_offset;            /**< first byte of audio data (after tags/headers) */
    uint32_t audio_size;
    media_art_ref_t art;
} media_meta_t;

typedef enum {
    MEDIA_OK = 0,
    MEDIA_ERR_IO = -1,
    MEDIA_ERR_UNSUPPORTED = -2,       /**< not a recognised format */
    MEDIA_ERR_CORRUPT = -3,           /**< recognised but malformed; partial fields may be set */
    MEDIA_ERR_SCRATCH = -4,           /**< scratch buffer too small */
} media_status_t;

/** Minimum scratch size the parser accepts. */
#define MEDIA_META_SCRATCH_MIN 2048u

/**
 * Detect the format and fill @p out. @p filename (may be NULL) is used for
 * the title fallback and as a format hint only; detection is by content.
 */
media_status_t media_meta_parse(const media_source_t *src, const char *filename, media_meta_t *out,
                                uint8_t *scratch, size_t scratch_len);

const char *media_format_name(media_format_t fmt);

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_META_H */
