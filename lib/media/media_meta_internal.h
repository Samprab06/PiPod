/**
 * @file media_meta_internal.h
 * @brief Private helpers shared by the media_meta implementation files.
 *
 * Not part of the public API.  Everything here is allocation-free: temporary
 * memory comes from the caller's scratch buffer.
 */
#ifndef MEDIA_META_INTERNAL_H
#define MEDIA_META_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "media_meta.h"

/** Parser context: byte source plus the caller's scratch buffer. */
typedef struct {
    const media_source_t *src;
    uint8_t *scratch;
    uint32_t scratch_len;
} meta_ctx_t;

/** Scratch sub-buffers (sizes depend on scratch_len; 2048 is the minimum). */
typedef struct {
    uint8_t *seq;      /**< raw read window for meta_seq_t */
    uint32_t seq_len;
    uint8_t *text;     /**< tag text being converted */
    uint32_t text_len;
    uint8_t *aux;      /**< format-specific temporary data */
    uint32_t aux_len;
} meta_scratch_t;

void meta_scratch_split(meta_ctx_t *c, meta_scratch_t *s);

/** Read exactly @p len bytes; false on short read or I/O error. */
bool meta_read(meta_ctx_t *c, uint32_t off, void *dst, uint32_t len);

uint16_t meta_u16le(const uint8_t *p);
uint32_t meta_u32le(const uint8_t *p);
uint32_t meta_u32be(const uint8_t *p);
uint32_t meta_u24be(const uint8_t *p);
uint64_t meta_u64be(const uint8_t *p);
uint64_t meta_u64le(const uint8_t *p);
uint32_t meta_syncsafe(const uint8_t *p);

/**
 * Decode tag text to UTF-8.
 * @param enc 0 = ISO-8859-1, 1 = UTF-16 with BOM (LE when absent),
 *            2 = UTF-16BE without BOM, 3 = UTF-8.
 * Trailing spaces and NULs are trimmed; output is truncated on a character
 * boundary and always NUL-terminated.
 */
void meta_text(char *dst, size_t cap, const uint8_t *src, uint32_t len, uint8_t enc);

/** Convert an ASCII/UTF-8 C string the same way as meta_text(enc=3). */
void meta_text_cstr(char *dst, size_t cap, const char *src);

/** Parse "3/12" / "03" into track and track_total (0 when absent). */
uint16_t meta_parse_index(const char *s, uint16_t *total);

/** Parse the leading 4-digit year of "2021", "2021-05-04", ... (0 if none). */
uint16_t meta_parse_year(const char *s);

/** Name for an ID3v1 genre index, NULL when out of range. */
const char *meta_genre_name(unsigned index);

/** Apply "(13)"/"13" style ID3v2 genres; leaves other text untouched. */
void meta_genre_apply(char *genre, size_t cap, const char *text);

/** JPEG/PNG/other from a few magic bytes (0 when unknown). */
uint8_t meta_art_type_magic(const uint8_t *p, uint32_t len);

/** Copy UTF-8 text, truncating on a character boundary. */
void meta_copy_text(char *dst, size_t cap, const char *src);

/**
 * Apply one Vorbis comment (FLAC VORBIS_COMMENT / Ogg comment header).
 * @p key is not NUL-terminated (length @p klen); @p value is UTF-8 text.
 */
void meta_comment_apply(media_meta_t *out, const char *key, uint32_t klen,
                        const char *value);

/* ----------------------------------------------------------------------- */
/* Sequential reader with optional ID3 unsynchronisation.                  */
/*                                                                         */
/* Returns bytes as they appear after de-unsynchronisation while keeping   */
/* the exact raw file offset of the next byte, which is what art refs      */
/* need.  Unsync state ("0xFF 0x00 means 0xFF") is resolved lazily so it   */
/* also works across read-window boundaries.                               */
/* ----------------------------------------------------------------------- */
typedef struct {
    const media_source_t *src;
    uint8_t *buf;
    uint32_t cap;
    uint32_t start;     /**< raw offset of buf[0] */
    uint32_t len;       /**< valid bytes in buf */
    uint32_t idx;       /**< next byte in buf */
    uint32_t end;       /**< raw read limit (exclusive) */
    bool unsync;
    bool pending_ff;    /**< last byte returned was 0xFF; drop a following 0x00 */
    bool io_error;
} meta_seq_t;

bool meta_seq_init(meta_seq_t *s, const media_source_t *src, uint8_t *buf, uint32_t cap,
                   uint32_t off, uint32_t end, bool unsync);

/** Next de-unsynchronised byte, or -1 at the end of the range / on error. */
int meta_seq_u8(meta_seq_t *s);

/** Skip @p n bytes of the logical (de-unsynchronised) stream. */
bool meta_seq_skip(meta_seq_t *s, uint32_t n);

/** Skip @p n raw on-disk bytes (used for frame-level unsynchronisation). */
bool meta_seq_skip_raw(meta_seq_t *s, uint32_t n);

/** Raw file offset of the next byte that meta_seq_u8() would return. */
uint32_t meta_seq_raw(const meta_seq_t *s);

bool meta_seq_at_end(const meta_seq_t *s);

/* ----------------------------------------------------------------------- */
/* ID3v2 (implemented in media_meta_mp3.c, reused by the WAV `id3 ` chunk) */
/* ----------------------------------------------------------------------- */
typedef struct {
    uint32_t offset;   /**< absolute file offset of the encoded picture */
    uint32_t size;     /**< encoded length in the file */
    uint8_t type;      /**< media_art_type_t */
    uint8_t enc;       /**< media_art_enc_t */
} meta_art_info_t;

/**
 * Parse an ID3v2 tag found at @p tag_off (at most @p avail bytes are part of
 * the container).  Fills tag fields only when the corresponding field is
 * still empty, and stores the best picture in @p art (may be NULL).
 * @p tag_end receives the offset just past the tag (including a v2.4 footer);
 * it is valid whenever the header could be parsed, even on MEDIA_ERR_CORRUPT.
 */
media_status_t meta_id3v2_parse(meta_ctx_t *c, uint32_t tag_off, uint32_t avail,
                                media_meta_t *out, meta_art_info_t *art,
                                uint32_t *tag_end);

/* ----------------------------------------------------------------------- */
/* Container parsers                                                       */
/* ----------------------------------------------------------------------- */
media_status_t meta_parse_mp3(meta_ctx_t *c, const char *filename, media_meta_t *out);
/** Content sniffing: true when a valid MPEG audio frame starts near offset 0. */
bool meta_mp3_detect(meta_ctx_t *c);
media_status_t meta_parse_flac(meta_ctx_t *c, const char *filename, media_meta_t *out);
media_status_t meta_parse_wav(meta_ctx_t *c, const char *filename, media_meta_t *out);
media_status_t meta_parse_ogg(meta_ctx_t *c, const char *filename, media_meta_t *out);

/** Fill out->art from a meta_art_info_t when the new picture is better. */
void meta_art_take(media_meta_t *out, const meta_art_info_t *art, bool prefer);

#endif /* MEDIA_META_INTERNAL_H */
