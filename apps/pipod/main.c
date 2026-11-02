/**
 * @file main.c
 * @brief pipod: the PiPod UI driven by a simulated music library.
 *
 * Core split (docs/design.md):
 *   core 0  input adapters, fake audio engine, fake decoder/I2S load
 *   core 1  display + UI (ui_init/ui_task/ui_get_perf live here by contract)
 *
 * The display is initialised on core 0 before core 1 starts; after that the UI
 * core owns it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/clocks.h"
#include "ili9341.h"
#include "pico/critical_section.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include "gfx_render.h"
#include "media_library.h"
#include "ui.h"

#include "audio_load_sim.h"
#include "input_buttons.h"
#include "input_uart.h"
#include "input_wheel.h"
#include "sim_library.h"
#include "sim_player.h"

#define UI_TARGET_FPS   30u
#define POLL_INTERVAL_US 2000u /**< button poll cadence */
#define REPORT_INTERVAL_US 5000000u
#define FRAME_INTERVAL_US (1000000u / UI_TARGET_FPS)

/** Snapshot of UI-core-only counters, copied to core 0 under a critical
 *  section. */
typedef struct {
    bool               valid;
    ui_perf_t          ui;
    gfx_render_stats_t gfx;
    ili9341_stats_t    panel;
} demo_perf_t;

static demo_perf_t      g_perf;
static critical_section_t g_perf_cs;
static volatile bool    s_ui_ready;

/* The real media library lives in flash; the UI gets whichever provider is
 * ready (real pack first, demo tables as the fallback). Both are set before
 * core 1 starts. */
static media_library_t       g_media_lib;
static const ui_library_t   *g_library;
static bool                  g_media_active;

/* --- core 1: display + UI ---------------------------------------------- */

static void collect_perf(void) {
    ui_perf_t p;
    ui_get_perf(&p, true);

    gfx_render_stats_t gs;
    gfx_render_get_stats(&gs);
    gfx_render_reset_stats();

    ili9341_stats_t is;
    ili9341_get_stats(&is);
    ili9341_reset_stats();

    critical_section_enter_blocking(&g_perf_cs);
    g_perf.ui = p;
    g_perf.gfx = gs;
    g_perf.panel = is;
    g_perf.valid = true;
    critical_section_exit(&g_perf_cs);
}

static void ui_core_entry(void) {
    const ui_config_t cfg = {
        .library = g_library,
        .target_fps = UI_TARGET_FPS,
        .initial_volume = SIM_DEMO_INITIAL_VOLUME,
        .long_press_ms = 600,
    };
    ui_init(&cfg);
    s_ui_ready = true;

    absolute_time_t next_frame = get_absolute_time();
    absolute_time_t last_stats = next_frame;

    for (;;) {
        ui_task();

        next_frame = delayed_by_us(next_frame, FRAME_INTERVAL_US);
        if (time_reached(next_frame)) {
            /* Fell more than a frame behind: re-phase instead of bursting. */
            next_frame = delayed_by_us(get_absolute_time(), FRAME_INTERVAL_US);
        }
        sleep_until(next_frame);

        const absolute_time_t now = get_absolute_time();
        if (absolute_time_diff_us(last_stats, now) >= (int64_t)REPORT_INTERVAL_US) {
            collect_perf();
            last_stats = now;
        }
    }
}

/* --- core 0: reporting -------------------------------------------------- */

static void print_perf_report(void) {
    demo_perf_t p;
    critical_section_enter_blocking(&g_perf_cs);
    p = g_perf;
    const bool valid = g_perf.valid;
    critical_section_exit(&g_perf_cs);

    if (!valid) {
        printf("[perf] UI counters not ready yet\n");
    } else {
        uint32_t slots = p.ui.frames + p.ui.idle_frames;
        uint32_t fps10 = (p.ui.elapsed_ms != 0u) ? (slots * 10000u / p.ui.elapsed_ms) : 0u;
        printf("[perf] ui: %lu.%lu fps  cpu %lu%%  frames %lu  idle %lu  "
               "frame avg %lu us  max %lu us  px/frame avg %lu  dma wait %lu us  "
               "dropped %lu\n",
               (unsigned long)(fps10 / 10u), (unsigned long)(fps10 % 10u),
               (unsigned long)p.ui.cpu_load_pct, (unsigned long)p.ui.frames,
               (unsigned long)p.ui.idle_frames, (unsigned long)p.ui.avg_frame_us,
               (unsigned long)p.ui.max_frame_us, (unsigned long)p.ui.avg_pixels,
               (unsigned long)p.ui.dma_wait_us, (unsigned long)p.ui.dropped_inputs);
        printf("[perf] gfx: frames %lu  rects %lu  bands %lu  pixels %lu  "
               "last %lu us  max %lu us\n",
               (unsigned long)p.gfx.frames, (unsigned long)p.gfx.rects,
               (unsigned long)p.gfx.bands, (unsigned long)p.gfx.pixels,
               (unsigned long)p.gfx.last_render_us, (unsigned long)p.gfx.max_render_us);
        printf("[perf] ili9341: %lu bytes  %lu xfers  dma busy %lu us\n",
               (unsigned long)p.panel.bytes_sent, (unsigned long)p.panel.transfers,
               (unsigned long)p.panel.dma_busy_us);
    }
    sim_player_print_status();
    audio_sim_print_report(false);
    /* Reset after every UART write so the blocking serial output is not
     * counted as part of the next audio-jitter window. */
    audio_sim_reset();
}

/** UART 'P' hook: perf report from core 0 without touching UI-core APIs. */
static void perf_hook(void) {
    print_perf_report();
}

/* --- media library scan report ----------------------------------------- */

static const char *art_type_name(const media_meta_t *meta) {
    switch (meta->art.type) {
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

static const char *art_result_name(uint8_t state) {
    switch (state) {
        case MEDIA_LIB_ART_DECODED:
            return "decoded";
        case MEDIA_LIB_ART_PLACEHOLDER:
            return "placeholder";
        default:
            return "not cached";
    }
}

/** Called once per pack entry during a full scan (never on a cache hit). */
static void scan_report(const media_scan_report_t *r) {
    if (r == NULL) {
        return;
    }
    if (r->meta == NULL) {
        printf("[scan] %2u/%u %-30s SKIPPED (status %d)\n", (unsigned)(r->index + 1u),
               (unsigned)r->count, r->name != NULL ? r->name : "?",
               (int)r->meta_status);
        return;
    }
    const media_meta_t *m = r->meta;
    const unsigned secs = m->duration_ms / 1000u;
    printf("[scan] %2u/%u %-26s %-5s %-24s %-22s %-22s %u:%02u %5lu Hz %uch "
           "%4lu kbps  art %s/%s\n",
           (unsigned)(r->index + 1u), (unsigned)r->count,
           r->name != NULL ? r->name : "?", media_format_name(m->format),
           m->title[0] != '\0' ? m->title : "(untitled)",
           m->artist[0] != '\0' ? m->artist : "(no artist)",
           m->album[0] != '\0' ? m->album : "(no album)", secs / 60u, secs % 60u,
           (unsigned long)m->sample_rate, (unsigned)m->channels,
           (unsigned long)m->bitrate_kbps, art_type_name(m),
           art_result_name(r->album_art));
}

/** Per-track report when the index came from the cache (no file is parsed). */
static void print_cached_tracks(void) {
    const media_scan_stats_t *st = media_library_stats(&g_media_lib);
    const ui_library_t *lib = media_library_ui(&g_media_lib);
    for (uint16_t i = 0; i < st->tracks; i++) {
        media_track_info_t ti;
        if (!media_library_track(&g_media_lib, MEDIA_LIB_TRACK_ID_BASE + i, &ti)) {
            continue;
        }
        const unsigned secs = ti.duration_ms / 1000u;
        const bool art = lib != NULL &&
                         lib->get_album_art(ti.album_id, lib->ctx) != NULL;
        printf("[scan] %2u/%u %-26s %-5s %-24s %-22s %-22s %u:%02u %5lu Hz %uch "
               "%4lu kbps  art %s\n",
               (unsigned)(i + 1u), (unsigned)st->tracks, "(cache)",
               media_format_name(ti.format),
               ti.title[0] != '\0' ? ti.title : "(untitled)",
               ti.artist[0] != '\0' ? ti.artist : "(no artist)",
               ti.album[0] != '\0' ? ti.album : "(no album)", secs / 60u, secs % 60u,
               (unsigned long)ti.sample_rate, (unsigned)ti.channels,
               (unsigned long)ti.bitrate_kbps, art ? "cached" : "none");
    }
}

/** Open the pack and build/load the index. Runs on core 0 before core 1. */
static void init_media_library(void) {
    const absolute_time_t t0 = get_absolute_time();
    const bool scanned = media_library_scan(&g_media_lib, scan_report);
    const uint32_t scan_ms =
        (uint32_t)(absolute_time_diff_us(t0, get_absolute_time()) / 1000);

    const media_scan_stats_t *st = media_library_stats(&g_media_lib);
    if (scanned && st->valid && media_library_ready(&g_media_lib)) {
        if (st->cache_hit) {
            print_cached_tracks();
        }
        g_library = media_library_ui(&g_media_lib);
        g_media_active = true;
        printf("[media] %s: %u tracks, %u albums, %u artists from %u files\n",
               st->cache_hit ? "cache hit" : "full scan", (unsigned)st->tracks,
               (unsigned)st->albums, (unsigned)st->artists, (unsigned)st->files);
        printf("[media] scan time %lu ms (UART report included), parses %lu, "
               "art decodes %lu, cached covers %u (%u placeholders), cache %lu B\n",
               (unsigned long)scan_ms, (unsigned long)st->parse_calls,
               (unsigned long)st->art_decodes, (unsigned)st->art_albums,
               (unsigned)st->art_placeholders, (unsigned long)st->cache_bytes);
        return;
    }

    g_library = sim_library_get();
    g_media_active = false;
    if (st->files == 0u) {
        printf("[media] no media pack at 0x00100000; using the demo library\n");
        printf("[media] (flash dist/test_songs.uf2 first, then rerun)\n");
    } else {
        printf("[media] pack has no usable tracks (%u files, %u skipped); "
               "using the demo library\n",
               (unsigned)st->files, (unsigned)st->skipped);
    }
}

static void print_banner(void) {
    printf("\n=== PiPod: music player UI on the Purdue Proton (RP2350B) ===\n");
    printf("clk_sys: %lu Hz\n", (unsigned long)clock_get_hz(clk_sys));
    printf("core 0: input + simulated audio; core 1: display + UI\n");
    printf("uart0: GP0 TX / GP1 RX, 115200 8N1 (debug probe)\n");
    printf("buttons (active high, external pull-downs):\n");
    printf("  GP2 wheel-CW   GP3 wheel-CCW  GP4 play/pause  GP8 prev\n");
    printf("  GP9 next       GP21 select    GP26 menu\n");
    printf("display pins: MISO GP32 CS GP33 SCK GP34 MOSI GP35 "
           "DC GP36 RST GP37 BL GP38\n");
}

int main(void) {
    stdio_init_all();
    sleep_ms(100);

    print_banner();

    /* The board runs from the 12 MHz crystal at clk_sys = 150 MHz by default;
     * deliberately not overclocking here. */
    ili9341_config_t dcfg;
    ili9341_default_config(&dcfg);
    const bool display_ok = ili9341_init(&dcfg);
    const uint32_t id = ili9341_read_id();
    printf("[display] init %s  id 0x%06lX  SPI %lu Hz  %ux%u landscape\n",
           display_ok ? "PASS" : "FAIL", (unsigned long)id,
           (unsigned long)ili9341_get_spi_clock(), (unsigned)ili9341_width(),
           (unsigned)ili9341_height());

    critical_section_init(&g_perf_cs);

    input_buttons_init(input_buttons_default_map(), input_buttons_default_map_count(), 5);
    input_wheel_init(INPUT_WHEEL_DEFAULT_TICKS_PER_REV, INPUT_WHEEL_DEFAULT_HYSTERESIS_DEG10);
    input_uart_init();
    input_uart_set_perf_hook(perf_hook);
    printf("[input] buttons armed, wheel %u ticks/rev\n",
           (unsigned)INPUT_WHEEL_DEFAULT_TICKS_PER_REV);

    /* Open the pack, scan into the cache (flash writes!) and pick the UI
     * library. This must stay before core 1 exists: flash writes stall XIP. */
    init_media_library();
    sim_player_set_library(g_library, g_media_active ? &g_media_lib : NULL);

    /* Start the fake 1 ms audio tick only after the scan: the flash erase and
     * UART report would otherwise show up as audio jitter in the first
     * report window. */
    audio_sim_init(AUDIO_SIM_DEFAULT_BUSY_PCT);
    printf("[audio] simulated decoder/I2S: 1 ms tick, busy load %u%%\n",
           (unsigned)AUDIO_SIM_DEFAULT_BUSY_PCT);

    multicore_launch_core1(ui_core_entry);

    /* sim_player publishes state through ui_set_*(); wait for ui_init() so the
     * UI is already listening when the first values arrive. */
    uint32_t waited_ms = 0;
    while (!s_ui_ready) {
        sleep_ms(1);
        waited_ms++;
        if (waited_ms == 3000u) {
            printf("[ui] still waiting for core 1 to finish ui_init...\n");
        }
    }
    sim_player_init();
    printf("[ui] core 1 ready; library: %u artists, %u albums, %u songs, "
           "%u playlists (%s)\n",
           (unsigned)g_library->count(UI_LIST_ARTISTS, UI_ID_ALL, g_library->ctx),
           (unsigned)g_library->count(UI_LIST_ALBUMS, UI_ID_ALL, g_library->ctx),
           (unsigned)g_library->count(UI_LIST_SONGS, UI_ID_ALL, g_library->ctx),
           (unsigned)g_library->count(UI_LIST_PLAYLISTS, UI_ID_ALL, g_library->ctx),
           g_media_active ? "media pack" : "demo tables");
    printf("[input] keys: w/k/Up=-1  s/j/Down=+1  e/Enter=select  m/Esc=menu  "
           "space=pause  n/Right=next  p/Left=prev  h=home  P=perf\n\n");

    absolute_time_t next_poll = get_absolute_time();
    absolute_time_t last_report = get_absolute_time();

    for (;;) {
        input_buttons_poll();
        input_uart_poll();
        sim_player_update();
        audio_sim_busy_work();

        const absolute_time_t now = get_absolute_time();
        if (absolute_time_diff_us(last_report, now) >= (int64_t)REPORT_INTERVAL_US) {
            last_report = now;
            print_perf_report();
        }

        next_poll = delayed_by_us(next_poll, POLL_INTERVAL_US);
        if (time_reached(next_poll)) {
            next_poll = delayed_by_us(get_absolute_time(), POLL_INTERVAL_US);
        }
        sleep_until(next_poll);
    }
}
