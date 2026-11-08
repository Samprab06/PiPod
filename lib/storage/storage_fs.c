/**
 * @file storage_fs.c
 * @brief FatFs backend of the read-only storage_fs_* interface.
 *
 * The walk is depth-first with a small static path stack and one static
 * FILINFO, so the scan costs about 4 KB of .bss and almost no stack (the Pico
 * SDK's default core-0 stack is only 2 KB). File handles come from a two-slot
 * static pool; each keeps a 512-byte read-ahead window so the metadata parser
 * and the JPEG reader do not issue one f_lseek()/f_read() per tiny read.
 *
 * Only read-only FatFs calls are used here: f_opendir, f_readdir, f_open with
 * FA_READ, f_lseek, f_read, f_size, f_close.
 */
#include "storage_fs.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "storage.h"

/* One path per recursion level (directories are visited depth first). */
static char s_dir_path[STORAGE_FS_MAX_DEPTH][STORAGE_FS_PATH_MAX];
static storage_fs_entry_t s_entry;
static FILINFO s_filinfo;

typedef struct storage_fs_file {
    FIL fil;
    bool open;
    uint32_t size;
    bool cache_valid;
    uint32_t cache_off;
    uint16_t cache_len;
    uint8_t cache[STORAGE_FS_CACHE_SIZE];
} storage_fs_file_t;

static storage_fs_file_t s_files[STORAGE_FS_MAX_OPEN];

/** Join @p dir and @p name with a single '/'; false when it does not fit. */
static bool join_path(char *dst, size_t cap, const char *dir, const char *name) {
    const size_t dl = strlen(dir);
    const size_t nl = strlen(name);
    if (dl == 0u) {
        if (nl + 1u > cap) {
            return false;
        }
        memcpy(dst, name, nl + 1u);
        return true;
    }
    const bool slash = dir[dl - 1u] == '/';
    const size_t need = dl + (slash ? 0u : 1u) + nl + 1u;
    if (need > cap) {
        return false;
    }
    memcpy(dst, dir, dl);
    size_t at = dl;
    if (!slash) {
        dst[at++] = '/';
    }
    memcpy(dst + at, name, nl);
    dst[at + nl] = '\0';
    return true;
}

static int walk_dir(const char *dir, unsigned depth, storage_fs_visit_fn cb,
                    void *ctx) {
    if (depth >= STORAGE_FS_MAX_DEPTH) {
        return 0;
    }
    DIR dh;
    FRESULT fr = f_opendir(&dh, dir);
    if (fr != FR_OK) {
        return depth == 0u ? -1 : 0;
    }
    for (;;) {
        fr = f_readdir(&dh, &s_filinfo);
        if (fr != FR_OK) {
            (void)f_closedir(&dh);
            return -2;
        }
        const char *name = s_filinfo.fname;
        if (name[0] == '\0') {
            break; /* end of directory */
        }
        /* FatFs does not return "." / ".."; skip hidden and system entries. */
        if (name[0] == '.' || (s_filinfo.fattrib & (AM_HID | AM_SYS)) != 0u) {
            continue;
        }
        const size_t nl = strlen(name);
        if (nl >= STORAGE_FS_NAME_MAX) {
            continue;
        }
        if (!join_path(s_entry.path, sizeof(s_entry.path), dir, name)) {
            continue; /* too long: not representable in the cache */
        }
        memcpy(s_entry.name, name, nl + 1u);
        const bool is_dir = (s_filinfo.fattrib & AM_DIR) != 0u;
        s_entry.size = is_dir ? 0u : (uint32_t)s_filinfo.fsize;
        s_entry.mtime =
            ((uint32_t)s_filinfo.fdate << 16) | (uint32_t)s_filinfo.ftime;

        if (!cb(ctx, &s_entry, is_dir)) {
            (void)f_closedir(&dh);
            return 0;
        }
        if (is_dir) {
            if (depth + 1u >= STORAGE_FS_MAX_DEPTH) {
                continue; /* too deep: do not ascend the stack */
            }
            /* The recursive call reuses s_entry, so keep the directory path. */
            memcpy(s_dir_path[depth + 1u], s_entry.path, sizeof(s_dir_path[0]));
            const int rc = walk_dir(s_dir_path[depth + 1u], depth + 1u, cb, ctx);
            if (rc != 0) {
                (void)f_closedir(&dh);
                return rc;
            }
        }
    }
    (void)f_closedir(&dh);
    return 0;
}

int storage_fs_walk(const char *root, storage_fs_visit_fn cb, void *ctx) {
    if (cb == NULL) {
        return -1;
    }
    if (!storage_mount()) {
        return -1;
    }
    const char *dir = root;
    if (dir == NULL || dir[0] == '\0') {
        dir = STORAGE_ROOT;
    }
    if (strlen(dir) >= STORAGE_FS_PATH_MAX) {
        return -1;
    }
    memcpy(s_dir_path[0], dir, strlen(dir) + 1u);
    return walk_dir(s_dir_path[0], 0u, cb, ctx);
}

storage_fs_file_t *storage_fs_open(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return NULL;
    }
    if (!storage_mount()) {
        return NULL;
    }
    for (size_t i = 0; i < STORAGE_FS_MAX_OPEN; i++) {
        storage_fs_file_t *file = &s_files[i];
        if (file->open) {
            continue;
        }
        const FRESULT fr = f_open(&file->fil, path, FA_READ | FA_OPEN_EXISTING);
        if (fr != FR_OK) {
            return NULL;
        }
        file->open = true;
        file->size = (uint32_t)f_size(&file->fil);
        file->cache_valid = false;
        file->cache_off = 0u;
        file->cache_len = 0u;
        return file;
    }
    return NULL;
}

uint32_t storage_fs_size(const storage_fs_file_t *file) {
    return file != NULL && file->open ? file->size : 0u;
}

int32_t storage_fs_read(storage_fs_file_t *file, uint32_t off, void *dst,
                        uint32_t len) {
    if (file == NULL || !file->open || (dst == NULL && len != 0u)) {
        return -1;
    }
    if (off >= file->size) {
        return 0;
    }
    if (len > file->size - off) {
        len = file->size - off;
    }

    uint32_t done = 0;
    while (done < len) {
        const uint32_t pos = off + done;
        const uint32_t base = pos & ~(uint32_t)(STORAGE_FS_CACHE_SIZE - 1u);
        if (!file->cache_valid || file->cache_off != base) {
            uint32_t want = file->size - base;
            if (want > STORAGE_FS_CACHE_SIZE) {
                want = STORAGE_FS_CACHE_SIZE;
            }
            FRESULT fr = f_lseek(&file->fil, base);
            if (fr != FR_OK) {
                return -1;
            }
            UINT br = 0;
            fr = f_read(&file->fil, file->cache, want, &br);
            if (fr != FR_OK) {
                return -1;
            }
            file->cache_off = base;
            file->cache_len = (uint16_t)br;
            file->cache_valid = true;
            if (br == 0u) {
                break;
            }
        }
        const uint32_t in_win = pos - file->cache_off;
        if (in_win >= file->cache_len) {
            break;
        }
        uint32_t chunk = (uint32_t)file->cache_len - in_win;
        if (chunk > len - done) {
            chunk = len - done;
        }
        memcpy((uint8_t *)dst + done, file->cache + in_win, chunk);
        done += chunk;
    }
    return (int32_t)done;
}

void storage_fs_close(storage_fs_file_t *file) {
    if (file == NULL || !file->open) {
        return;
    }
    (void)f_close(&file->fil);
    file->open = false;
    file->cache_valid = false;
    file->cache_len = 0u;
}
