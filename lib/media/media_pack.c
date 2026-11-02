/**
 * @file media_pack.c
 * @brief PPAK reader. Shared unchanged between the RP2350 and the simulator:
 *        all flash access goes through media_flash_ptr() from flash_layout.h.
 */
#include "media_pack.h"

#include <string.h>

#include "flash_layout.h"

/* ---- little-endian helpers ------------------------------------------- */

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* ---- CRC-32 (IEEE 802.3, zlib compatible) ----------------------------
 * 64-byte nibble table: four lookups per byte instead of eight iterations and
 * no 1 KB table. 14 MB verifies in roughly a second on the RP2350. */

static const uint32_t s_crc_nibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t media_crc32_update(uint32_t crc, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    while (len-- != 0u) {
        crc ^= *p++;
        crc = (crc >> 4) ^ s_crc_nibble[crc & 0x0Fu];
        crc = (crc >> 4) ^ s_crc_nibble[crc & 0x0Fu];
    }
    return crc;
}

uint32_t media_crc32(const void *data, uint32_t len) {
    return ~media_crc32_update(0xFFFFFFFFu, data, len);
}

/* ---- entry validation ------------------------------------------------- */

/** A TOC name must be a relative POSIX path that fits in 64 bytes. */
static bool name_valid(const uint8_t *name) {
    uint32_t len = 0;
    while (len < MEDIA_PACK_NAME_MAX && name[len] != 0u) {
        len++;
    }
    if (len == 0u || len == MEDIA_PACK_NAME_MAX) {
        return false;
    }
    if (name[0] == '/') {
        return false;
    }
    uint32_t comp = 0;
    for (uint32_t i = 0; i <= len; i++) {
        if (i == len || name[i] == '/') {
            uint32_t clen = i - comp;
            if (clen == 0u) {
                return false; /* empty component ("//" or trailing '/') */
            }
            if (clen == 1u && name[comp] == '.') {
                return false;
            }
            if (clen == 2u && name[comp] == '.' && name[comp + 1] == '.') {
                return false;
            }
            comp = i + 1u;
        } else if (name[i] == '\\' || name[i] < 0x20u || name[i] == 0x7Fu) {
            return false;
        }
    }
    return true;
}

static bool entry_at(const media_pack_t *p, uint16_t index,
                     media_pack_file_t *out, const uint8_t **name_out) {
    if (p == NULL || p->base == NULL || index >= p->count) {
        return false;
    }
    const uint8_t *entry =
        p->base + p->toc_offset + (uint32_t)index * MEDIA_PACK_TOC_ENTRY;
    if (!name_valid(entry)) {
        return false;
    }
    uint32_t off = rd32(entry + 64);
    uint32_t size = rd32(entry + 68);
    uint32_t crc = rd32(entry + 72);
    uint32_t data_start =
        p->toc_offset + (uint32_t)p->count * MEDIA_PACK_TOC_ENTRY;
    if (off < data_start) {
        return false; /* overlaps the header/TOC */
    }
    if ((off & (MEDIA_PACK_DATA_ALIGN - 1u)) != 0u) {
        return false;
    }
    if (size > p->total_size || off > p->total_size - size) {
        return false; /* past the end of the pack (and overflow-safe) */
    }
    if (out != NULL) {
        out->data = p->base + off;
        out->size = size;
        out->crc32 = crc;
    }
    if (name_out != NULL) {
        *name_out = entry;
    }
    return true;
}

/* ---- public API ------------------------------------------------------- */

bool media_pack_open(media_pack_t *p) {
    if (p == NULL) {
        return false;
    }
    memset(p, 0, sizeof(*p));

    const uint8_t *base = media_flash_ptr(MEDIA_PACK_OFFSET);
    if (base == NULL || memcmp(base, MEDIA_PACK_MAGIC, 4) != 0) {
        return false;
    }

    uint16_t version = rd16(base + 4);
    uint16_t count = rd16(base + 6);
    uint32_t total_size = rd32(base + 8);
    uint32_t toc_offset = rd32(base + 12);
    uint32_t toc_crc = rd32(base + 16);

    if (version != MEDIA_PACK_VERSION) {
        return false;
    }
    if (total_size < MEDIA_PACK_HEADER_SIZE || total_size > MEDIA_PACK_MAX) {
        return false;
    }
    if (toc_offset < MEDIA_PACK_HEADER_SIZE || toc_offset > total_size) {
        return false;
    }
    uint32_t toc_bytes = (uint32_t)count * MEDIA_PACK_TOC_ENTRY;
    if (toc_bytes > total_size - toc_offset) {
        return false;
    }
    if (media_crc32(base + toc_offset, toc_bytes) != toc_crc) {
        return false;
    }

    p->base = base;
    p->total_size = total_size;
    p->toc_offset = toc_offset;
    p->toc_crc32 = toc_crc;
    p->count = count;
    p->next_slot = 0;

    for (uint16_t i = 0; i < count; i++) {
        if (!entry_at(p, i, NULL, NULL)) {
            memset(p, 0, sizeof(*p));
            return false;
        }
    }
    return true;
}

uint16_t media_pack_count(const media_pack_t *p) {
    return p != NULL ? p->count : 0u;
}

uint32_t media_pack_total_size(const media_pack_t *p) {
    return p != NULL ? p->total_size : 0u;
}

uint32_t media_pack_toc_crc(const media_pack_t *p) {
    return p != NULL ? p->toc_crc32 : 0u;
}

uint32_t media_pack_cache_key(const media_pack_t *p) {
    /* Mix the TOC CRC so the value is well distributed and never 0 (0 marks an
     * empty cache header). Any change to the pack changes the TOC CRC. */
    uint32_t k = p != NULL ? p->toc_crc32 : 0u;
    k ^= k >> 16;
    k *= 0x7FEB352Du;
    k ^= k >> 15;
    k *= 0x846CA68Bu;
    k ^= k >> 16;
    return k != 0u ? k : 1u;
}

const char *media_pack_name(const media_pack_t *p, uint16_t index) {
    const uint8_t *name = NULL;
    if (!entry_at(p, index, NULL, &name)) {
        return NULL;
    }
    return (const char *)name;
}

bool media_pack_file_at(const media_pack_t *p, uint16_t index,
                        media_pack_file_t *out) {
    return entry_at(p, index, out, NULL);
}

static int32_t pack_read(void *ctx, uint32_t off, void *dst, uint32_t len) {
    media_pack_file_t *file = (media_pack_file_t *)ctx;
    if (file == NULL || (dst == NULL && len != 0u)) {
        return -1;
    }
    if (off > file->size) {
        return -1;
    }
    uint32_t avail = file->size - off;
    if (len > avail) {
        len = avail;
    }
    if (len != 0u) {
        memcpy(dst, file->data + off, len);
    }
    return (int32_t)len;
}

bool media_pack_source_ex(const media_pack_t *p, uint16_t index,
                          media_pack_file_t *storage, media_source_t *out) {
    if (storage == NULL || out == NULL) {
        return false;
    }
    if (!entry_at(p, index, storage, NULL)) {
        return false;
    }
    out->read = pack_read;
    out->size = storage->size;
    out->xip_base = storage->data;
    out->ctx = storage;
    return true;
}

bool media_pack_source(media_pack_t *p, uint16_t index, media_source_t *out) {
    if (p == NULL || out == NULL) {
        return false;
    }
    media_pack_file_t *slot = &p->sources[p->next_slot];
    if (!media_pack_source_ex(p, index, slot, out)) {
        return false;
    }
    p->next_slot = (uint8_t)((p->next_slot + 1u) % MEDIA_PACK_MAX_SOURCES);
    return true;
}

bool media_pack_verify(const media_pack_t *p, uint16_t index) {
    media_pack_file_t file;
    if (!entry_at(p, index, &file, NULL)) {
        return false;
    }
    return media_crc32(file.data, file.size) == file.crc32;
}
