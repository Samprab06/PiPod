/**
 * @file storage_fs_host.c
 * @brief Host (PC) backend of the read-only storage_fs_* interface.
 *
 * Lets the SD scan run against an ordinary directory in the host test, so the
 * exact media_library_scan_sd() code path (directory walk, cache key, tag/art
 * parsing, path table, audio source) is exercised without a card reader.
 *
 * Host-only: never compiled into the firmware.
 */
#include "storage_fs.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef struct storage_fs_file {
    FILE *fp;
    bool open;
    uint32_t size;
} storage_fs_file_t;

#if defined(_WIN32)
#define HOST_STAT stat
#else
#define HOST_STAT lstat
#endif

static storage_fs_file_t s_files[STORAGE_FS_MAX_OPEN];

static bool join_path(char *dst, size_t cap, const char *dir, const char *name) {
    const size_t dl = strlen(dir);
    const size_t nl = strlen(name);
    const bool slash = dl == 0u || dir[dl - 1u] == '/';
    const size_t need = dl + (slash ? 0u : 1u) + nl + 1u;
    if (need > cap) {
        return false;
    }
    memcpy(dst, dir, dl);
    size_t at = dl;
    if (!slash) {
        dst[at++] = '/';
    }
    memcpy(dst + at, name, nl + 1u);
    return true;
}

static int walk_dir(const char *dir, unsigned depth, storage_fs_visit_fn cb,
                    void *ctx) {
    if (depth >= STORAGE_FS_MAX_DEPTH) {
        return 0;
    }
    DIR *dh = opendir(dir);
    if (dh == NULL) {
        return depth == 0u ? -1 : 0;
    }
    int rc = 0;
    struct dirent *de;
    while ((de = readdir(dh)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        if (strlen(de->d_name) >= STORAGE_FS_NAME_MAX) {
            continue;
        }
        storage_fs_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        if (!join_path(entry.path, sizeof(entry.path), dir, de->d_name)) {
            continue;
        }
        memcpy(entry.name, de->d_name, strlen(de->d_name) + 1u);

        struct stat st;
        if (HOST_STAT(entry.path, &st) != 0) {
            continue;
        }
        const bool is_dir = S_ISDIR(st.st_mode);
        if (!is_dir && !S_ISREG(st.st_mode)) {
            continue; /* symlink, socket, ...: FAT has none either */
        }
        entry.size = is_dir ? 0u : (uint32_t)st.st_size;
        entry.mtime = (uint32_t)st.st_mtime;

        if (!cb(ctx, &entry, is_dir)) {
            break;
        }
        if (is_dir) {
            rc = walk_dir(entry.path, depth + 1u, cb, ctx);
            if (rc != 0) {
                break;
            }
        }
    }
    (void)closedir(dh);
    return rc;
}

int storage_fs_walk(const char *root, storage_fs_visit_fn cb, void *ctx) {
    if (cb == NULL || root == NULL || root[0] == '\0') {
        return -1;
    }
    return walk_dir(root, 0u, cb, ctx);
}

storage_fs_file_t *storage_fs_open(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < STORAGE_FS_MAX_OPEN; i++) {
        storage_fs_file_t *file = &s_files[i];
        if (file->open) {
            continue;
        }
        FILE *fp = fopen(path, "rb");
        if (fp == NULL) {
            return NULL;
        }
        if (fseek(fp, 0, SEEK_END) != 0) {
            fclose(fp);
            return NULL;
        }
        const long end = ftell(fp);
        if (end < 0) {
            fclose(fp);
            return NULL;
        }
        file->fp = fp;
        file->open = true;
        file->size = (uint32_t)end;
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
    if (len == 0u) {
        return 0;
    }
    if (fseek(file->fp, (long)off, SEEK_SET) != 0) {
        return -1;
    }
    const size_t got = fread(dst, 1, len, file->fp);
    return (int32_t)got;
}

void storage_fs_close(storage_fs_file_t *file) {
    if (file == NULL || !file->open) {
        return;
    }
    fclose(file->fp);
    file->fp = NULL;
    file->open = false;
    file->size = 0u;
}
