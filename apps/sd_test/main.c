/**
 * @file main.c
 * @brief sd_test: bring-up tool for the microSD card on SPI1.
 *
 * Mounts the card, prints the card type/capacity, recursively lists every
 * audio file and parses each .mp3 with the same metadata engine the player
 * uses (title, artist, album, track number, duration, embedded art type and
 * size). Nothing is decoded and the card is never written.
 *
 * UART0 only (GP0 TX / GP1 RX, 115200 8N1) and no display, so it can be
 * flashed before the panel is wired. Pins come from pipod_pins.h.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/time.h"

#include "pipod_pins.h" /* must precede anything that uses the board map */

#include "media_meta.h"
#include "storage.h"
#include "storage_fs.h"

#define SD_TEST_SCRATCH 4096u

static uint8_t s_scratch[SD_TEST_SCRATCH];

typedef struct {
    uint32_t files;
    uint32_t audio;
    uint32_t mp3;
    uint32_t mp3_parsed;
    uint32_t mp3_failed;
    uint64_t art_bytes;
    uint64_t audio_bytes;
} sd_totals_t;

/* Positional view of one open file for media_meta_parse(). */
typedef struct {
    storage_fs_file_t *file;
    uint32_t size;
} file_view_t;

static int32_t view_read(void *ctx, uint32_t off, void *dst, uint32_t len) {
    file_view_t *view = (file_view_t *)ctx;
    if (view == NULL || view->file == NULL) {
        return -1;
    }
    if (off > view->size) {
        return -1;
    }
    uint32_t avail = view->size - off;
    if (len > avail) {
        len = avail;
    }
    if (len == 0u) {
        return 0;
    }
    return storage_fs_read(view->file, off, dst, len);
}

static char lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool has_ext(const char *name, const char *ext) {
    const size_t n = strlen(name);
    const size_t e = strlen(ext);
    if (n < e) {
        return false;
    }
    for (size_t i = 0; i < e; i++) {
        if (lower_ascii(name[n - e + i]) != ext[i]) {
            return false;
        }
    }
    return true;
}

static bool is_audio_file(const char *name) {
    return has_ext(name, ".mp3") || has_ext(name, ".flac") ||
           has_ext(name, ".wav") || has_ext(name, ".ogg") ||
           has_ext(name, ".opus");
}

static const char *art_type_name(uint8_t type) {
    switch (type) {
        case MEDIA_ART_JPEG:
            return "jpeg";
        case MEDIA_ART_PNG:
            return "png";
        case MEDIA_ART_OTHER:
            return "other";
        default:
            return "none";
    }
}

static void print_time(uint32_t ms) {
    if (ms == 0u) {
        printf("unknown");
    } else {
        printf("%lu:%02lu", (unsigned long)(ms / 60000u),
               (unsigned long)((ms / 1000u) % 60u));
    }
}

/** Parse one .mp3 and print what the metadata engine found. */
static void parse_mp3(const storage_fs_entry_t *entry, sd_totals_t *totals) {
    storage_fs_file_t *file = storage_fs_open(entry->path);
    if (file == NULL) {
        printf("      parse FAIL (open)\n");
        totals->mp3_failed++;
        return;
    }

    file_view_t view;
    view.file = file;
    view.size = storage_fs_size(file);

    media_source_t src;
    src.read = view_read;
    src.size = view.size;
    src.xip_base = NULL;
    src.ctx = &view;

    media_meta_t meta;
    const media_status_t rc =
        media_meta_parse(&src, entry->name, &meta, s_scratch, sizeof(s_scratch));
    storage_fs_close(file);

    if (rc != MEDIA_OK) {
        printf("      parse FAIL (%d)\n", (int)rc);
        totals->mp3_failed++;
        return;
    }
    totals->mp3_parsed++;
    totals->art_bytes += meta.art.size;

    printf("      %-5s %s - %s  [%s]", media_format_name(meta.format),
           meta.artist[0] != '\0' ? meta.artist : "(no artist)",
           meta.title[0] != '\0' ? meta.title : "(untitled)",
           meta.album[0] != '\0' ? meta.album : "(no album)");
    if (meta.track != 0u) {
        if (meta.track_total != 0u) {
            printf("  track %u/%u", (unsigned)meta.track, (unsigned)meta.track_total);
        } else {
            printf("  track %u", (unsigned)meta.track);
        }
    }
    printf("  ");
    print_time(meta.duration_ms);
    printf("  %lu Hz %uch %lukbps", (unsigned long)meta.sample_rate,
           (unsigned)meta.channels, (unsigned long)meta.bitrate_kbps);
    printf("  art %s", art_type_name(meta.art.type));
    if (meta.art.type != MEDIA_ART_NONE) {
        printf(" %lu B @ %lu", (unsigned long)meta.art.size,
               (unsigned long)meta.art.offset);
        if (meta.art.width != 0u && meta.art.height != 0u) {
            printf(" %ux%u", (unsigned)meta.art.width, (unsigned)meta.art.height);
        }
    }
    printf("\n");
}

static bool visit(void *vctx, const storage_fs_entry_t *entry, bool is_dir) {
    sd_totals_t *totals = (sd_totals_t *)vctx;
    if (is_dir) {
        printf("[dir ] %s/\n", entry->path);
        return true;
    }
    totals->files++;
    if (!is_audio_file(entry->name)) {
        return true;
    }
    totals->audio++;
    totals->audio_bytes += entry->size;
    printf("[file] %-60s %lu B\n", entry->path, (unsigned long)entry->size);

    if (has_ext(entry->name, ".mp3")) {
        totals->mp3++;
        parse_mp3(entry, totals);
    }
    return true;
}

int main(void) {
    stdio_init_all();
    sleep_ms(100);

    printf("\n=== PiPod sd_test (microSD on SPI1) ===\n");
    printf("[pins] MISO GP%u CS GP%u SCK GP%u MOSI GP%u (no card detect)\n",
           (unsigned)PIPOD_PIN_SD_MISO, (unsigned)PIPOD_PIN_SD_CS,
           (unsigned)PIPOD_PIN_SD_SCK, (unsigned)PIPOD_PIN_SD_MOSI);

    const absolute_time_t t0 = get_absolute_time();

    if (!storage_mount()) {
        printf("[sd] mount FAILED\n");
        storage_print_diagnostic();
        printf("[sd] check 3.3 V power, the SPI1 wires and the card format "
               "(FAT32 or exFAT)\n");
        return 1;
    }

    const storage_info_t *info = storage_info();
    const uint32_t mount_ms =
        (uint32_t)(absolute_time_diff_us(t0, get_absolute_time()) / 1000u);
    const unsigned long mb = (unsigned long)(info->capacity_bytes / (1024ull * 1024ull));
    printf("[sd] mounted %s in %lu ms  %lu MB (%lu sectors)  SPI %lu Hz\n",
           storage_card_type_str(info->card_type), (unsigned long)mount_ms, mb,
           (unsigned long)info->sector_count, (unsigned long)info->spi_hz);
    printf("[sd] CID product \"%s\" manufacturer \"%s\"\n", info->product,
           info->manufacturer);

    sd_totals_t totals;
    memset(&totals, 0, sizeof(totals));

    const absolute_time_t walk_t0 = get_absolute_time();
    const int walk_rc = storage_fs_walk(STORAGE_ROOT, visit, &totals);
    const uint32_t walk_ms =
        (uint32_t)(absolute_time_diff_us(walk_t0, get_absolute_time()) / 1000u);

    printf("\n=== totals ===\n");
    printf("[sd] walk: %d, %lu ms  entries %lu  audio %lu  mp3 %lu\n", walk_rc,
           (unsigned long)walk_ms, (unsigned long)totals.files,
           (unsigned long)totals.audio, (unsigned long)totals.mp3);
    printf("[sd] mp3 parsed %lu, failed %lu  audio bytes %lu  embedded art %lu B\n",
           (unsigned long)totals.mp3_parsed, (unsigned long)totals.mp3_failed,
           (unsigned long)totals.audio_bytes, (unsigned long)totals.art_bytes);
    printf("[sd] total time %lu ms\n",
           (unsigned long)(absolute_time_diff_us(t0, get_absolute_time()) / 1000u));

    return totals.mp3_failed == 0u ? 0 : 2;
}
