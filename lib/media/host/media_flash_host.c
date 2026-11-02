/**
 * @file media_flash_host.c
 * @brief PC simulator: 16 MB flash image in a file, plus the low-level
 *        program/erase primitives used by media_cache.
 *
 * Every program/erase is mirrored into the file immediately, so the image is
 * always consistent with what the device would have in flash.
 */
#include "media_flash_host.h"

#if !defined(PICO_ON_DEVICE)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../flash_layout.h"

#define MEDIA_HOST_FLASH_DEFAULT_PATH "sim/out/flash.bin"

static uint8_t *s_image;
static FILE *s_file;

static bool write_range(uint32_t offset, const void *data, uint32_t size) {
    if (s_file == NULL || s_image == NULL) {
        return false;
    }
    if (offset > MEDIA_FLASH_SIZE || size > MEDIA_FLASH_SIZE - offset) {
        return false;
    }
    if (size == 0u) {
        return true;
    }
    if (fseek(s_file, (long)offset, SEEK_SET) != 0) {
        return false;
    }
    if (fwrite(data, 1, size, s_file) != size) {
        return false;
    }
    return fflush(s_file) == 0;
}

bool media_host_flash_init(const char *path) {
    if (s_image != NULL) {
        return true; /* init is once per process */
    }
    if (path == NULL) {
        path = MEDIA_HOST_FLASH_DEFAULT_PATH;
    }

    uint8_t *image = (uint8_t *)malloc(MEDIA_FLASH_SIZE);
    if (image == NULL) {
        return false;
    }

    bool loaded = false;
    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        if (fseek(f, 0, SEEK_END) == 0) {
            long size = ftell(f);
            if (size == (long)MEDIA_FLASH_SIZE && fseek(f, 0, SEEK_SET) == 0) {
                loaded = fread(image, 1, MEDIA_FLASH_SIZE, f) == MEDIA_FLASH_SIZE;
            }
        }
        fclose(f);
        if (!loaded) {
            free(image);
            return false; /* existing file with the wrong size: do not guess */
        }
    } else {
        /* New image: erased flash reads as 0xFF. */
        memset(image, 0xFF, MEDIA_FLASH_SIZE);
        f = fopen(path, "wb");
        if (f == NULL) {
            free(image);
            return false;
        }
        bool wrote = fwrite(image, 1, MEDIA_FLASH_SIZE, f) == MEDIA_FLASH_SIZE;
        fclose(f);
        if (!wrote) {
            free(image);
            return false;
        }
    }

    f = fopen(path, "r+b");
    if (f == NULL) {
        free(image);
        return false;
    }
    s_image = image;
    s_file = f;
    return true;
}

bool media_host_flash_load(uint32_t offset, const char *path) {
    if (s_image == NULL || path == NULL) {
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    bool ok = false;
    if (fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        if (size >= 0 && (uint32_t)size <= MEDIA_FLASH_SIZE - offset &&
            fseek(f, 0, SEEK_SET) == 0) {
            uint32_t len = (uint32_t)size;
            if (len == 0u ||
                fread(s_image + offset, 1, len, f) == (size_t)len) {
                ok = write_range(offset, s_image + offset, len);
            }
        }
    }
    fclose(f);
    return ok;
}

void media_host_flash_flush(void) {
    if (s_file != NULL) {
        fflush(s_file);
    }
}

void media_host_flash_close(void) {
    if (s_file != NULL) {
        fclose(s_file);
        s_file = NULL;
    }
    free(s_image);
    s_image = NULL;
}

uint8_t *media_host_flash_buffer(void) {
    return s_image;
}

const uint8_t *media_flash_ptr(uint32_t offset) {
    if (s_image == NULL || offset >= MEDIA_FLASH_SIZE) {
        return NULL;
    }
    return s_image + offset;
}

bool media_flash_erase(uint32_t offset, uint32_t size) {
    if (s_image == NULL || size == 0u ||
        (offset & (MEDIA_FLASH_SECTOR_SIZE - 1u)) != 0u ||
        (size & (MEDIA_FLASH_SECTOR_SIZE - 1u)) != 0u) {
        return false;
    }
    if (offset > MEDIA_FLASH_SIZE || size > MEDIA_FLASH_SIZE - offset) {
        return false;
    }
    memset(s_image + offset, 0xFF, size);
    return write_range(offset, s_image + offset, size);
}

bool media_flash_program(uint32_t offset, const void *data, uint32_t size) {
    if (s_image == NULL || data == NULL || size == 0u ||
        (offset & (MEDIA_FLASH_PAGE_SIZE - 1u)) != 0u ||
        (size & (MEDIA_FLASH_PAGE_SIZE - 1u)) != 0u) {
        return false;
    }
    if (offset > MEDIA_FLASH_SIZE || size > MEDIA_FLASH_SIZE - offset) {
        return false;
    }
    memcpy(s_image + offset, data, size);
    return write_range(offset, data, size);
}

#endif /* !PICO_ON_DEVICE */
