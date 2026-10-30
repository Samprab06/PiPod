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
        .library = sim_library_get(),
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

    audio_sim_init(AUDIO_SIM_DEFAULT_BUSY_PCT);
    printf("[audio] simulated decoder/I2S: 1 ms tick, busy load %u%%\n",
           (unsigned)AUDIO_SIM_DEFAULT_BUSY_PCT);

    input_buttons_init(input_buttons_default_map(), input_buttons_default_map_count(), 5);
    input_wheel_init(INPUT_WHEEL_DEFAULT_TICKS_PER_REV, INPUT_WHEEL_DEFAULT_HYSTERESIS_DEG10);
    input_uart_init();
    input_uart_set_perf_hook(perf_hook);
    printf("[input] buttons armed, wheel %u ticks/rev\n",
           (unsigned)INPUT_WHEEL_DEFAULT_TICKS_PER_REV);

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
           "%u playlists\n",
           (unsigned)SIM_ARTIST_COUNT, (unsigned)SIM_ALBUM_COUNT,
           (unsigned)SIM_SONG_COUNT, (unsigned)SIM_PLAYLIST_COUNT);
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
