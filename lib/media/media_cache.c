/**
 * @file media_cache.c
 * @brief Append-only cache writer/reader. Shared unchanged between the RP2350
 *        and the simulator: low-level flash access goes through the two
 *        primitives in flash_layout.h, which wrap flash_safe_execute() on the
 *        device and the file-backed image on the host.
 */
#include "media_cache.h"

#include <string.h>

#include "flash_layout.h"
#include "media_pack.h" /* media_crc32() */

/* ---- little-endian helpers ------------------------------------------- */

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* ---- cache state ------------------------------------------------------ */

typedef struct {
    bool active;
    uint32_t key;
    uint32_t used;      /**< next free offset in the cache region */
    uint32_t buf_base;  /**< flash offset of buf[0], sector aligned */
    uint32_t buf_len;   /**< bytes valid in buf, including the sector prefix */
    uint32_t programmed;/**< bytes of this sector already programmed */
    bool erased;        /**< sector has been erased during this buffer pass */
    uint8_t buf[MEDIA_CACHE_SECTOR_SIZE];
} media_cache_state_t;

static media_cache_state_t s_cache;

/* ---- header ----------------------------------------------------------- */

static bool header_ok(const uint8_t *hdr, uint32_t *key, uint32_t *used) {
    if (memcmp(hdr, MEDIA_CACHE_MAGIC, 4) != 0) {
        return false;
    }
    if (rd16(hdr + 4) != MEDIA_CACHE_VERSION) {
        return false;
    }
    if (media_crc32(hdr, 16) != rd32(hdr + 16)) {
        return false;
    }
    uint32_t u = rd32(hdr + 12);
    if (u < MEDIA_CACHE_DATA_OFFSET || u > MEDIA_CACHE_SIZE ||
        (u & 3u) != 0u) {
        return false;
    }
    if (key != NULL) {
        *key = rd32(hdr + 8);
    }
    if (used != NULL) {
        *used = u;
    }
    return true;
}

static bool write_header(uint32_t key, uint32_t used, const uint8_t *user) {
    if (used < MEDIA_CACHE_DATA_OFFSET || used > MEDIA_CACHE_SIZE ||
        (used & 3u) != 0u) {
        return false;
    }
    uint8_t hdr[MEDIA_CACHE_HEADER_SIZE];
    uint8_t page[MEDIA_CACHE_PAGE_SIZE];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, MEDIA_CACHE_MAGIC, 4);
    wr16(hdr + 4, MEDIA_CACHE_VERSION);
    wr32(hdr + 8, key);
    wr32(hdr + 12, used);
    wr32(hdr + 16, media_crc32(hdr, 16));
    if (user != NULL) {
        memcpy(hdr + 20, user, MEDIA_CACHE_USER_SIZE);
    }
    memset(page, 0xFF, sizeof(page));
    memcpy(page, hdr, sizeof(hdr));
    if (!media_flash_erase(MEDIA_CACHE_OFFSET, MEDIA_CACHE_SECTOR_SIZE)) {
        return false;
    }
    return media_flash_program(MEDIA_CACHE_OFFSET, page, sizeof(page));
}

/* ---- buffered sector programming -------------------------------------- */

/**
 * Start buffering the 4 KB sector that contains s_cache.used.
 *
 * Live bytes before `used` in that sector are copied into the RAM buffer; the
 * rest is 0xFF. The sector is erased on the first flush and rewritten from the
 * buffer. Commit() always leaves `used` on a sector boundary, so in normal
 * operation this only restores a committed prefix after a crash during a
 * previous scan; the tiny erase/program window is the one non-atomic step.
 */
static bool sector_begin(void) {
    uint32_t base = s_cache.used & ~(MEDIA_CACHE_SECTOR_SIZE - 1u);
    uint32_t prefix = s_cache.used - base;
    const uint8_t *flash = media_flash_ptr(MEDIA_CACHE_OFFSET + base);
    if (flash == NULL) {
        return false;
    }
    if (prefix != 0u) {
        memcpy(s_cache.buf, flash, prefix);
    }
    memset(s_cache.buf + prefix, 0xFF, MEDIA_CACHE_SECTOR_SIZE - prefix);
    s_cache.buf_base = base;
    s_cache.buf_len = prefix;
    s_cache.programmed = 0;
    s_cache.erased = false;
    return true;
}

/**
 * Erase the buffered sector (once) and program its pages.
 *
 * During the scan only whole 256-byte pages are programmed, so bytes can never
 * be written to a page twice: the final partial page stays in RAM until
 * media_cache_commit() flushes it (@p final). Data is therefore guaranteed
 * readable (media_cache_ptr) only after a commit.
 */
static bool cache_flush(bool final) {
    if (s_cache.buf_len == 0u) {
        return true;
    }
    if (!s_cache.erased) {
        if (!media_flash_erase(MEDIA_CACHE_OFFSET + s_cache.buf_base,
                               MEDIA_CACHE_SECTOR_SIZE)) {
            return false;
        }
        s_cache.erased = true;
    }
    uint32_t end;
    if (final) {
        end = (s_cache.buf_len + (MEDIA_CACHE_PAGE_SIZE - 1u)) &
              ~(MEDIA_CACHE_PAGE_SIZE - 1u);
    } else {
        end = s_cache.buf_len & ~(MEDIA_CACHE_PAGE_SIZE - 1u);
    }
    if (end > MEDIA_CACHE_SECTOR_SIZE) {
        end = MEDIA_CACHE_SECTOR_SIZE;
    }
    if (end > s_cache.programmed) {
        if (!media_flash_program(MEDIA_CACHE_OFFSET + s_cache.buf_base +
                                     s_cache.programmed,
                                 s_cache.buf + s_cache.programmed,
                                 end - s_cache.programmed)) {
            return false;
        }
        s_cache.programmed = end;
    }
    return true;
}

/* ---- public API ------------------------------------------------------- */

bool media_cache_begin(void) {
    const uint8_t *hdr = media_flash_ptr(MEDIA_CACHE_OFFSET);
    if (hdr == NULL) {
        return false;
    }
    uint32_t key = 0;
    uint32_t used = 0;
    if (!header_ok(hdr, &key, &used)) {
        return media_cache_reset();
    }
    s_cache.active = true;
    s_cache.key = key;
    /* Resume on a page boundary so the last (possibly partially programmed)
     * page of the previous run is never rewritten. */
    s_cache.used = (used + (MEDIA_CACHE_PAGE_SIZE - 1u)) &
                   ~(MEDIA_CACHE_PAGE_SIZE - 1u);
    s_cache.buf_base = 0;
    s_cache.buf_len = 0;
    s_cache.programmed = 0;
    s_cache.erased = false;
    return true;
}

bool media_cache_reset(void) {
    s_cache.active = false;
    s_cache.buf_len = 0;
    s_cache.programmed = 0;
    s_cache.erased = false;
    if (!write_header(0u, MEDIA_CACHE_DATA_OFFSET, NULL)) {
        return false;
    }
    s_cache.active = true;
    s_cache.key = 0u;
    s_cache.used = MEDIA_CACHE_DATA_OFFSET;
    s_cache.buf_base = 0;
    return true;
}

bool media_cache_write(const void *data, uint32_t len, uint32_t *out_offset) {
    if (!s_cache.active) {
        return false;
    }
    if (data == NULL && len != 0u) {
        return false;
    }
    if (s_cache.used > MEDIA_CACHE_SIZE ||
        len > MEDIA_CACHE_SIZE - s_cache.used) {
        return false; /* region full: no wrap, the caller starts a new scan */
    }
    uint32_t alloc = (len + 3u) & ~3u;
    if (alloc > MEDIA_CACHE_SIZE - s_cache.used) {
        return false; /* the 4-byte padding does not fit either */
    }
    uint32_t start = s_cache.used;

    const uint8_t *src = (const uint8_t *)data;
    uint32_t remaining = len;
    while (remaining != 0u) {
        if (s_cache.buf_len == 0u && !sector_begin()) {
            return false;
        }
        uint32_t pos = s_cache.used - s_cache.buf_base;
        uint32_t space = MEDIA_CACHE_SECTOR_SIZE - pos;
        uint32_t take = remaining < space ? remaining : space;
        memcpy(s_cache.buf + pos, src, take);
        src += take;
        remaining -= take;
        s_cache.used += take;
        s_cache.buf_len = pos + take;

        if (remaining == 0u && (take & 3u) != 0u) {
            uint32_t pad = 4u - (take & 3u);
            memset(s_cache.buf + s_cache.buf_len, 0, pad);
            s_cache.buf_len += pad;
            s_cache.used += pad;
        }
        if (!cache_flush(false)) {
            s_cache.active = false;
            return false;
        }
        if (s_cache.buf_len == MEDIA_CACHE_SECTOR_SIZE) {
            s_cache.buf_len = 0;
            s_cache.programmed = 0;
            s_cache.erased = false;
        }
    }

    if (out_offset != NULL) {
        *out_offset = start;
    }
    return true;
}

bool media_cache_commit(uint32_t key) {
    return media_cache_commit_ex(key, NULL);
}

bool media_cache_commit_ex(uint32_t key, const uint8_t user[MEDIA_CACHE_USER_SIZE]) {
    if (!s_cache.active) {
        return false;
    }
    if (!cache_flush(true)) {
        s_cache.active = false;
        return false;
    }
    /* Start later appends on a fresh sector: a committed sector is never
     * erased again, and the last programmed page is left untouched. */
    uint32_t committed = (s_cache.used + (MEDIA_CACHE_SECTOR_SIZE - 1u)) &
                         ~(MEDIA_CACHE_SECTOR_SIZE - 1u);
    if (!write_header(key, committed, user)) {
        return false;
    }
    s_cache.key = key;
    s_cache.used = committed;
    s_cache.buf_base = 0;
    s_cache.buf_len = 0;
    s_cache.programmed = 0;
    s_cache.erased = false;
    return true;
}

bool media_cache_user(uint8_t user[MEDIA_CACHE_USER_SIZE]) {
    if (user == NULL) {
        return false;
    }
    memset(user, 0, MEDIA_CACHE_USER_SIZE);
    const uint8_t *hdr = media_flash_ptr(MEDIA_CACHE_OFFSET);
    if (hdr == NULL) {
        return false;
    }
    if (!header_ok(hdr, NULL, NULL)) {
        return false;
    }
    memcpy(user, hdr + 20, MEDIA_CACHE_USER_SIZE);
    return true;
}

bool media_cache_valid(uint32_t key) {
    if (key == 0u) {
        return false; /* 0 marks an empty/uncommitted cache */
    }
    const uint8_t *hdr = media_flash_ptr(MEDIA_CACHE_OFFSET);
    if (hdr == NULL) {
        return false;
    }
    uint32_t stored_key = 0;
    if (!header_ok(hdr, &stored_key, NULL)) {
        return false;
    }
    return stored_key == key;
}

uint32_t media_cache_used(void) {
    return s_cache.active ? s_cache.used : 0u;
}

const uint8_t *media_cache_ptr(uint32_t offset) {
    if (offset >= MEDIA_CACHE_SIZE) {
        return NULL;
    }
    return media_flash_ptr(MEDIA_CACHE_OFFSET + offset);
}
