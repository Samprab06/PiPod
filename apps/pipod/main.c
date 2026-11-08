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
 *
 * Two firmware images share this source: `pipod` (ILI9341, 320x240) and
 * `pipod_st7796` (ST7796S, 480x320). display.h resolves every display_* call to
 * the selected driver at compile time; all board pins come from
 * pipod_pins.h.
 *
 * Library source at boot (core 0, before core 1): the microSD card first, then
 * the flash media pack, then the built-in demo tables. Only metadata is read
 * from the card; no audio bytes are ever decoded by this demo.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/clocks.h"
#include "pico/critical_section.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include "pipod_pins.h" /* must precede the driver headers: board pin map */

#include "display.h"
#include "gfx_render.h"
#include "media_library.h"
#include "storage.h"
#include "ui.h"

#include "audio_load_sim.h"
#include "input_buttons.h"
#include "input_touchpads.h"
#include "input_uart.h"
#include "input_wheel.h"
#include "sim_library.h"
#include "sim_player.h"

#define UI_TARGET_FPS   30u
#define POLL_INTERVAL_US 2000u /**< button + touchpad poll cadence */
#define REPORT_INTERVAL_US 5000000u
#define FRAME_INTERVAL_US (1000000u / UI_TARGET_FPS)

/** Snapshot of UI-core-only counters, copied to core 0 under a critical
 *  section. */
typedef struct {
    bool               valid;
    ui_perf_t          ui;
    gfx_render_stats_t gfx;
    display_stats_t    panel;
} demo_perf_t;

static demo_perf_t      g_perf;
static critical_section_t g_perf_cs;
static volatile bool    s_ui_ready;

/* The media library lives either on the SD card, in the flash pack or in the
 * demo tables. Whichever provider is ready is set before core 1 starts. */
static media_library_t       g_media_lib;
static const ui_library_t   *g_library;
static bool                  g_media_active;

/* --- board input maps --------------------------------------------------- */

/* The old lib/input default map occupies GP2/GP3/GP4, which are the TTP223
 * slider pads on this breadboard, so the app owns its button map: only PREV
 * and NEXT are physical buttons; SELECT and MENU stay on the on-board Proton
 * buttons (GP21 / GP26). */
static const input_button_map_t k_breadboard_buttons[] = {
    { .gpio = PIPOD_PIN_BTN_PREV, .ui_button = UI_BTN_PREV, .active_high = true },
    { .gpio = PIPOD_PIN_BTN_NEXT, .ui_button = UI_BTN_NEXT, .active_high = true },
    { .gpio = PIPOD_PIN_BTN_ONBOARD_SELECT, .ui_button = UI_BTN_SELECT, .active_high = true },
    { .gpio = PIPOD_PIN_BTN_ONBOARD_MENU, .ui_button = UI_BTN_MENU, .active_high = true },
};

/* --- core 1: display + UI ---------------------------------------------- */

static void collect_perf(void) {
    ui_perf_t p;
    ui_get_perf(&p, true);

    gfx_render_stats_t gs;
    gfx_render_get_stats(&gs);
    gfx_render_reset_stats();

    display_stats_t ds;
    display_get_stats(&ds);
    display_reset_stats();

    critical_section_enter_blocking(&g_perf_cs);
    g_perf.ui = p;
    g_perf.gfx = gs;
    g_perf.panel = ds;
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
        printf("[perf] %s: %lu bytes  %lu xfers  dma busy %lu us\n", display_name(),
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

/** Called once per file during a full scan (never on a cache hit). */
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

/** Print the counts of whichever source just built the index. */
static void report_library(const char *source, uint32_t scan_ms) {
    const media_scan_stats_t *st = media_library_stats(&g_media_lib);
    printf("[media] %s%s: %u tracks, %u albums, %u artists from %u files\n", source,
           st->cache_hit ? " (cache hit)" : "", (unsigned)st->tracks,
           (unsigned)st->albums, (unsigned)st->artists, (unsigned)st->files);
    printf("[media] scan time %lu ms (UART report included), parses %lu, "
           "art decodes %lu, cached covers %u (%u placeholders), cache %lu B\n",
           (unsigned long)scan_ms, (unsigned long)st->parse_calls,
           (unsigned long)st->art_decodes, (unsigned)st->art_albums,
           (unsigned)st->art_placeholders, (unsigned long)st->cache_bytes);
}

/**
 * Pick the library source. Runs on core 0 before core 1 exists: flash cache
 * writes (SD and pack scans alike) stall XIP and must not overlap the UI.
 *
 *   1. microSD card (metadata only, never audio bytes), if mounted and it has
 *      at least one playable track;
 *   2. the flash media pack at 0x00100000;
 *   3. the built-in demo tables.
 */
static void init_media_library(void) {
    if (storage_mount()) {
        const storage_info_t *info = storage_info();
        const unsigned long mb = (unsigned long)(info->capacity_bytes / (1024ull * 1024ull));
        printf("[sd] %s card, %lu MB, SPI %lu Hz, product \"%s\"\n",
               storage_card_type_str(info->card_type), mb,
               (unsigned long)info->spi_hz, info->product);

        const absolute_time_t t0 = get_absolute_time();
        const bool scanned = media_library_scan_sd(&g_media_lib, STORAGE_ROOT, scan_report);
        const uint32_t scan_ms =
            (uint32_t)(absolute_time_diff_us(t0, get_absolute_time()) / 1000);
        const media_scan_stats_t *st = media_library_stats(&g_media_lib);

        if (scanned && st->valid && media_library_ready(&g_media_lib)) {
            if (st->cache_hit) {
                print_cached_tracks();
            }
            g_library = media_library_ui(&g_media_lib);
            g_media_active = true;
            report_library("SD card", scan_ms);
            return;
        }
        printf("[media] SD card: no usable tracks (%u files, %u skipped), "
               "trying the flash media pack\n",
               (unsigned)st->files, (unsigned)st->skipped);
    } else {
        printf("[sd] no card / mount failed (SPI1 MISO GP28 CS GP29 SCK GP30 "
               "MOSI GP31), trying the flash media pack\n");
    }

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
        report_library("flash media pack", scan_ms);
        return;
    }

    g_library = sim_library_get();
    g_media_active = false;
    if (st->files == 0u) {
        printf("[media] no media pack at 0x00100000; using the demo library\n");
        printf("[media] (flash dist/test_songs.uf2 or insert a card, then rerun)\n");
    } else {
        printf("[media] pack has no usable tracks (%u files, %u skipped); "
               "using the demo library\n",
               (unsigned)st->files, (unsigned)st->skipped);
    }
}

/* --- boot banner -------------------------------------------------------- */

static void print_banner(void) {
    printf("\n=== PiPod: music player UI on the Purdue Proton (RP2350B) ===\n");
    printf("firmware: %s  %ux%u landscape\n", display_name(), (unsigned)display_width(),
           (unsigned)display_height());
    printf("clk_sys: %lu Hz\n", (unsigned long)clock_get_hz(clk_sys));
    printf("core 0: input + simulated audio; core 1: display + UI\n");
    printf("uart0: GP0 TX / GP1 RX, 115200 8N1 (debug probe)\n");
    printf("buttons (active high): GP8 prev, GP9 next, GP21 select, GP26 menu\n");
    printf("TTP223: slider GP2/GP3/GP4, select GP5, menu GP6, play/pause GP7\n");
    printf("display: CS GP33 SCK GP34 MOSI GP35 DC GP36 RST GP37");
#if PIPOD_DISPLAY_ST7796
    printf(" (BL tied to 3V3, no MISO)\n");
#else
    printf(", MISO GP32, BL GP38\n");
#endif
    printf("microSD: SPI1 MISO GP28 CS GP29 SCK GP30 MOSI GP31\n");
}

/* --- input setup -------------------------------------------------------- */

static void init_buttons(void) {
    input_buttons_init(k_breadboard_buttons,
                       sizeof(k_breadboard_buttons) / sizeof(k_breadboard_buttons[0]), 5);
}

static void init_touchpads(void) {
    input_touchpads_config_t tcfg;
    input_touchpads_default_config(&tcfg);

    tcfg.slider_left_gpio = (uint8_t)PIPOD_PIN_TOUCH_LEFT;
    tcfg.slider_middle_gpio = (uint8_t)PIPOD_PIN_TOUCH_MIDDLE;
    tcfg.slider_right_gpio = (uint8_t)PIPOD_PIN_TOUCH_RIGHT;
    tcfg.button_select_gpio = (uint8_t)PIPOD_PIN_TOUCH_SELECT;
    tcfg.button_menu_gpio = (uint8_t)PIPOD_PIN_TOUCH_MENU;
    tcfg.button_play_pause_gpio = (uint8_t)PIPOD_PIN_TOUCH_PLAY_PAUSE;

    /* TTP223 modules drive both levels (momentary, active high with the
     * default jumpers). The RP2350-E9 erratum makes internal pull-downs
     * unreliable, so leave the pads floating as the modules intend. */
    tcfg.active_high = (PIPOD_TOUCH_ACTIVE_HIGH != 0);
    tcfg.internal_pulls = false;

    input_touchpads_init(&tcfg);
    printf("[touch] TTP223 pads on GP2..GP7 armed (active_high=%u, internal_pulls=0)\n",
           PIPOD_TOUCH_ACTIVE_HIGH ? 1u : 0u);
}

/* --- main --------------------------------------------------------------- */

int main(void) {
    stdio_init_all();
    sleep_ms(100);

    print_banner();

    /* The TTP223 modules self-calibrate at power-up; remind the operator
     * before any pad can be sampled. */
    printf("[touch] do not touch the TTP223 pads during power-up calibration "
           "(GP2..GP7)\n");

    /* Display: pins come from the board header and override the driver's
     * compile-time defaults through the runtime config. */
    display_config_t dcfg;
    display_default_config(&dcfg);
    dcfg.pin_cs = (uint)PIPOD_PIN_DISPLAY_CS;
    dcfg.pin_sck = (uint)PIPOD_PIN_DISPLAY_SCK;
    dcfg.pin_mosi = (uint)PIPOD_PIN_DISPLAY_MOSI;
    dcfg.pin_dc = (uint)PIPOD_PIN_DISPLAY_DC;
    dcfg.pin_rst = (uint)PIPOD_PIN_DISPLAY_RST;
#if !PIPOD_DISPLAY_ST7796
    dcfg.pin_miso = (uint)PIPOD_PIN_DISPLAY_MISO;
    dcfg.pin_bl = (uint)PIPOD_PIN_DISPLAY_BL;
#endif

    const bool display_ok = display_init(&dcfg);
    display_selftest_t st;
    const display_selftest_result_t selftest = display_self_test(&st);

    printf("[display] %s init %s  SPI %lu Hz  %ux%u landscape\n", display_name(),
           display_ok ? "PASS" : "FAIL", (unsigned long)display_get_spi_clock(),
           (unsigned)display_width(), (unsigned)display_height());
    if (selftest == DISPLAY_SELFTEST_UNSUPPORTED) {
        printf("[display] no readback: the module has no MISO, write-only run\n");
    } else {
        printf("[display] self-test %s  id 0x%06lX  miso_alive=%u madctl_ok=%u "
               "pixfmt_ok=%u display_on=%u\n",
               selftest == DISPLAY_SELFTEST_OK ? "PASS" : "FAIL",
               (unsigned long)st.rddid, (unsigned)st.miso_alive,
               (unsigned)st.madctl_ok, (unsigned)st.pixfmt_ok,
               (unsigned)st.display_on);
    }

    critical_section_init(&g_perf_cs);

    init_buttons();
    init_touchpads();
    input_wheel_init(INPUT_WHEEL_DEFAULT_TICKS_PER_REV, INPUT_WHEEL_DEFAULT_HYSTERESIS_DEG10);
    input_uart_init();
    input_uart_set_perf_hook(perf_hook);
    printf("[input] buttons armed, wheel %u ticks/rev\n",
           (unsigned)INPUT_WHEEL_DEFAULT_TICKS_PER_REV);

    /* Mount/scan the card, then the pack, then the demo tables. Must stay
     * before core 1 exists: flash writes stall XIP. */
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
           g_media_active ? "media library" : "demo tables");
    printf("[input] keys: w/k/Up=-1  s/j/Down=+1  e/Enter=select  m/Esc=menu  "
           "space=pause  n/Right=next  p/Left=prev  h=home  P=perf\n\n");

    absolute_time_t next_poll = get_absolute_time();
    absolute_time_t last_report = get_absolute_time();

    for (;;) {
        input_buttons_poll();
        input_touchpads_poll();
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
