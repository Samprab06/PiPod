/**
 * @file storage_fs.h
 * @brief Tiny read-only filesystem interface used by lib/media on top of the
 *        SD card.
 *
 * lib/media never calls FatFs directly. It walks a directory tree and reads
 * files through these few functions, so the exact same scan code runs on the
 * device (FatFs backend, storage_fs.c) and on a PC host (stdio backend,
 * lib/media/host/storage_fs_host.c) in the host tests.
 *
 * The interface is deliberately small and single-threaded:
 *
 *  - storage_fs_walk() enumerates a directory tree depth-first. Directories are
 *    visited before their contents; the callback decides whether to descend.
 *  - storage_fs_open()/read()/close() provide positional (pread-style) reads.
 *    The FatFs backend keeps a small read-ahead cache per open file and uses
 *    f_lseek() + f_read() underneath.
 *
 * Nothing here can write to the card.
 */
#ifndef STORAGE_FS_H
#define STORAGE_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Longest path (including the drive prefix) the scanners accept. */
#define STORAGE_FS_PATH_MAX 256u
/** Longest file/directory base name the scanners accept. */
#define STORAGE_FS_NAME_MAX 96u
/** Deepest directory recursion (the scan stops descending below this). */
#define STORAGE_FS_MAX_DEPTH 12u
/** Read-ahead window used by the FatFs backend (bytes, power of two). */
#define STORAGE_FS_CACHE_SIZE 512u
/** Open-file slots. The scan needs one; the player keeps one for audio. */
#define STORAGE_FS_MAX_OPEN 2u

/** One directory entry, as handed to the walk callback. */
typedef struct {
    /** Path used to open the entry, including the root prefix ("0:/Music/x.mp3"). */
    char path[STORAGE_FS_PATH_MAX];
    /** Base name only ("x.mp3"); used for the title fallback. */
    char name[STORAGE_FS_NAME_MAX];
    /** File size in bytes (0 for directories). */
    uint32_t size;
    /**
     * Last-modification stamp for the cache key: the packed FAT date/time on
     * the device, the host's mtime on a PC. 0 when unknown.
     */
    uint32_t mtime;
} storage_fs_entry_t;

/**
 * @brief Walk @p root recursively, depth first.
 *
 * Directories are reported before their contents. Returning false from the
 * callback stops the walk (not an error). Symbolic links are not followed on
 * the host; the FAT backend has none.
 *
 * @return 0 on success (including a callback-requested stop), -1 when the root
 *         cannot be opened / the card cannot be mounted, -2 on an I/O error.
 */
typedef bool (*storage_fs_visit_fn)(void *ctx, const storage_fs_entry_t *entry,
                                    bool is_dir);
int storage_fs_walk(const char *root, storage_fs_visit_fn cb, void *ctx);

/** Opaque read-only file handle (backend-allocated, at most STORAGE_FS_MAX_OPEN). */
typedef struct storage_fs_file storage_fs_file_t;

/**
 * @brief Open @p path read-only.
 * @return a handle, or NULL when the file cannot be opened or no slot is free.
 */
storage_fs_file_t *storage_fs_open(const char *path);

/**
 * @brief Read up to @p len bytes at byte offset @p off.
 *
 * Short only at end of file. Returns -1 on an I/O error. The handle remembers
 * its size, so reads past the end return 0.
 */
int32_t storage_fs_read(storage_fs_file_t *file, uint32_t off, void *dst,
                        uint32_t len);

/** @brief File size in bytes, or 0 for an invalid handle. */
uint32_t storage_fs_size(const storage_fs_file_t *file);

/** @brief Close a handle. Safe with NULL. */
void storage_fs_close(storage_fs_file_t *file);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_FS_H */
