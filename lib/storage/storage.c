/**
 * @file storage.c
 * @brief microSD card on SPI1: mount/unmount, card info, diagnostics.
 */
#include "storage.h"

#include <stdio.h>
#include <string.h>

#include "diskio.h"
#include "ff.h"
#include "hw_config.h"
#include "sd_card.h"
#include "storage_fs.h"
#include "storage_hw.h"
#include "util.h"

static FATFS s_fs;
static bool s_driver_ready;
static bool s_mounted;
static int s_last_error;
static int s_last_fr;
static storage_info_t s_info;

int storage_last_fr(void) {
    return s_last_fr;
}

static void info_reset(void) {
    memset(&s_info, 0, sizeof(s_info));
}

bool storage_init(void) {
    if (s_driver_ready) {
        return true;
    }
    info_reset();
    s_last_error = STORAGE_OK;
    if (!sd_init_driver()) {
        s_last_error = STORAGE_ERR_UNSUPPORTED;
        return false;
    }
    s_driver_ready = true;
    return true;
}

bool storage_mount(void) {
    if (s_mounted) {
        return true;
    }
    if (!storage_init()) {
        return false;
    }
    info_reset();

    /* Breadboard wiring may not carry the full post-init clock: retry the
     * mount at lower clocks before giving up. Each retry forces a fresh card
     * init (400 kHz) so the new clock is applied by the driver. */
    static const uint32_t k_mount_hz[] = {STORAGE_SD_SPI_HZ, 12500000u, 5000000u, 1000000u};
    FRESULT fr = FR_NOT_READY;
    for (size_t i = 0; i < sizeof k_mount_hz / sizeof k_mount_hz[0]; ++i) {
        if (i > 0 && k_mount_hz[i] >= k_mount_hz[0]) {
            continue;
        }
        sd_card_t *card = sd_get_by_num(0);
        if (card != NULL) {
            card->state.m_Status |= STA_NOINIT;
        }
        (void)storage_hw_set_spi_hz(k_mount_hz[i]);
        fr = f_mount(&s_fs, STORAGE_DRIVE, 1);
        s_last_fr = (int)fr;
        if (fr == FR_OK) {
            break;
        }
        (void)f_mount(NULL, STORAGE_DRIVE, 0);
        printf("[sd] mount at %lu Hz failed: %s\n", (unsigned long)k_mount_hz[i],
               storage_fr_str((int)fr));
        if (card == NULL || card->state.card_type == SDCARD_NONE) {
            break; /* no card answered: slower clocks will not help */
        }
    }
    if (fr != FR_OK) {
        s_last_error = STORAGE_ERR_MOUNT;
        sd_card_t *card = sd_get_by_num(0);
        if (card == NULL || card->state.card_type == SDCARD_NONE) {
            s_last_error = STORAGE_ERR_NO_CARD;
        }
        return false;
    }

    sd_card_t *card = sd_get_by_num(0);
    s_info.mounted = true;
    s_info.card_type = card != NULL ? (uint8_t)card->state.card_type : (uint8_t)SDCARD_NONE;
    s_info.sector_count = card != NULL ? card->state.sectors : 0u;
    s_info.capacity_bytes = (uint64_t)s_info.sector_count * 512u;
    s_info.spi_hz = storage_hw_spi_hz();
    if (card != NULL) {
        ext_str(16, (const uint8_t *)&card->state.CID, 103, 64, sizeof(s_info.product),
                s_info.product);
        (void)snprintf(s_info.manufacturer, sizeof(s_info.manufacturer), "%02X",
                       (unsigned)ext_bits16((const uint8_t *)&card->state.CID, 127, 120));
    }
    s_info.mounted = true;
    s_mounted = true;
    s_last_error = STORAGE_OK;
    return true;
}

void storage_unmount(void) {
    if (s_driver_ready) {
        (void)f_unmount(STORAGE_DRIVE);
    }
    s_mounted = false;
    s_info.mounted = false;
}

bool storage_mounted(void) {
    return s_mounted;
}

const storage_info_t *storage_info(void) {
    return &s_info;
}

uint32_t storage_set_spi_hz(uint32_t hz) {
    const uint32_t actual = storage_hw_set_spi_hz(hz);
    if (s_mounted) {
        s_info.spi_hz = actual;
    }
    return actual;
}

const char *storage_card_type_str(uint8_t card_type) {
    switch (card_type) {
    case SDCARD_V1:
        return "SD V1";
    case SDCARD_V2:
        return "SD V2";
    case SDCARD_V2HC:
        return "SD V2HC/SDXC";
    case SDCARD_NONE:
        return "none";
    default:
        return "unknown";
    }
}

const char *storage_status_str(int status) {
    switch (status) {
    case STORAGE_OK:
        return "ok";
    case STORAGE_ERR_NO_CARD:
        return "no card responded on SPI1";
    case STORAGE_ERR_MOUNT:
        return "card answered but mount failed (filesystem?)";
    case STORAGE_ERR_NOT_MOUNTED:
        return "card is not mounted";
    case STORAGE_ERR_IO:
        return "card read error";
    case STORAGE_ERR_NO_SLOT:
        return "too many files open";
    case STORAGE_ERR_RANGE:
        return "offset outside the file";
    case STORAGE_ERR_UNSUPPORTED:
        return "storage driver could not be initialized";
    default:
        return "unknown storage error";
    }
}

const char *storage_fr_str(int fr) {
    switch (fr) {
    case FR_OK:
        return "FR_OK";
    case FR_DISK_ERR:
        return "FR_DISK_ERR";
    case FR_INT_ERR:
        return "FR_INT_ERR";
    case FR_NOT_READY:
        return "FR_NOT_READY";
    case FR_NO_FILE:
        return "FR_NO_FILE";
    case FR_NO_PATH:
        return "FR_NO_PATH";
    case FR_INVALID_NAME:
        return "FR_INVALID_NAME";
    case FR_DENIED:
        return "FR_DENIED";
    case FR_EXIST:
        return "FR_EXIST";
    case FR_INVALID_OBJECT:
        return "FR_INVALID_OBJECT";
    case FR_WRITE_PROTECTED:
        return "FR_WRITE_PROTECTED";
    case FR_INVALID_DRIVE:
        return "FR_INVALID_DRIVE";
    case FR_NOT_ENABLED:
        return "FR_NOT_ENABLED";
    case FR_NO_FILESYSTEM:
        return "FR_NO_FILESYSTEM";
    case FR_MKFS_ABORTED:
        return "FR_MKFS_ABORTED";
    case FR_TIMEOUT:
        return "FR_TIMEOUT";
    case FR_LOCKED:
        return "FR_LOCKED";
    case FR_NOT_ENOUGH_CORE:
        return "FR_NOT_ENOUGH_CORE";
    case FR_TOO_MANY_OPEN_FILES:
        return "FR_TOO_MANY_OPEN_FILES";
    case FR_INVALID_PARAMETER:
        return "FR_INVALID_PARAMETER";
    default:
        return "unknown FRESULT";
    }
}

/* ------------------------------------------------------------------------- */
/* Diagnostic                                                               */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint32_t files;
    uint32_t dirs;
    uint64_t bytes;
    uint16_t printed;
} diag_ctx_t;

static bool diag_visit(void *vctx, const storage_fs_entry_t *entry, bool is_dir) {
    diag_ctx_t *ctx = (diag_ctx_t *)vctx;
    if (is_dir) {
        ctx->dirs++;
        if (ctx->printed < 12u) {
            printf("  [dir ] %s\n", entry->path);
            ctx->printed++;
        }
        return true;
    }
    ctx->files++;
    ctx->bytes += entry->size;
    if (ctx->printed < 12u) {
        printf("  [file] %-48s %8lu B\n", entry->name, (unsigned long)entry->size);
        ctx->printed++;
    }
    return true;
}

void storage_print_diagnostic(void) {
    printf("[sd] driver: SPI1  MISO GP28  CS GP29  SCK GP30  MOSI GP31 (no card detect)\n");
    printf("[sd] init clock 400 kHz, %lu Hz after init, mode %u\n",
           (unsigned long)STORAGE_SD_SPI_HZ, (unsigned)STORAGE_SD_SPI_MODE);

    if (!storage_mount()) {
        printf("[sd] mount FAILED: %s (%d)\n", storage_status_str(s_last_error),
               s_last_error);
        return;
    }

    const storage_info_t *info = storage_info();
    printf("[sd] mounted %s  %s  %lu sectors  %lu MiB  SPI %lu Hz\n",
           STORAGE_DRIVE, storage_card_type_str(info->card_type),
           (unsigned long)info->sector_count,
           (unsigned long)(info->capacity_bytes / (1024u * 1024u)),
           (unsigned long)info->spi_hz);
    if (info->product[0] != '\0') {
        printf("[sd] CID: manufacturer %s product %s\n", info->manufacturer,
               info->product);
    }
    printf("[sd] FatFs: FAT12/16/32 and exFAT, LFN UTF-8, read-only use\n");

    diag_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    const int rc = storage_fs_walk(STORAGE_ROOT, diag_visit, &ctx);
    if (rc != 0) {
        printf("[sd] root listing failed (%d)\n", rc);
        return;
    }
    printf("[sd] root: %lu files, %lu dirs, %lu KiB\n", (unsigned long)ctx.files,
           (unsigned long)ctx.dirs, (unsigned long)(ctx.bytes / 1024u));
    if (ctx.files + ctx.dirs > ctx.printed) {
        printf("  ... (listing truncated)\n");
    }
}
