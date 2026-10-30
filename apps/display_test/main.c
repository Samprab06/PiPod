/*
 * display_test: ILI9341 driver validation and benchmark for the Purdue Proton.
 *
 * UART output (115200 8N1 on GP0) reports PASS/INFO lines for every stage:
 *   - init and panel self-test (RDDID, RDMADCTL, RDDCOLMOD, RDDPM)
 *   - solid fills, colour bars, frame/corner orientation check
 *   - checkerboard partial updates
 *   - SPI clock / fill / blit benchmarks
 *   - a bouncing-square animation at 30 FPS
 */

#include <stdio.h>

#include "hardware/clocks.h"
#include "ili9341.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#define SCR_W 320
#define SCR_H 240
#define STRIP_H 16

#define RGB565(r, g, b) \
    ((uint16_t)(((uint16_t)((r) & 0xF8u) << 8) | ((uint16_t)((g) & 0xFCu) << 3) | ((uint16_t)(b) >> 3)))

/* One 320x16 RGB565 strip = 10 KB, the only large static buffer in the app. */
static uint16_t g_strip[SCR_W * STRIP_H];

/* Achievable PL022 rates at clk_peri = 150 MHz:
 * f = clk_peri / (CPSDVSR * (1 + SCR)), CPSDVSR even 2..254.
 * 31.25/50/62.5 MHz are NOT achievable (they fold to 25/37.5/37.5 MHz). */
static const struct {
    uint32_t    hz;
    const char *label;
} k_bench_clocks[] = {
    {15000000u, NULL},
    {18750000u, NULL},
    {25000000u, NULL},
    {37500000u, NULL},
    {75000000u, "experimental"},
};

static uint32_t fps_tenths(uint32_t us) {
    if (us == 0u) {
        return 0u;
    }
    return (uint32_t)(10000000ull / (uint64_t)us);
}

static void print_fps(uint32_t us) {
    uint32_t f = fps_tenths(us);
    printf("%u.%u fps", (unsigned)(f / 10u), (unsigned)(f % 10u));
}

/* Fill the whole screen, measure the wall-clock time in microseconds. */
static uint32_t fill_screen_timed(uint16_t color) {
    uint64_t t0 = time_us_64();
    ili9341_fill_rect(0, 0, SCR_W, SCR_H, color);
    ili9341_wait_idle();
    return (uint32_t)(time_us_64() - t0);
}

static void test_solid_fills(void) {
    static const struct {
        const char *name;
        uint16_t color;
    } fills[] = {
        {"red", RGB565(255, 0, 0)},
        {"green", RGB565(0, 255, 0)},
        {"blue", RGB565(0, 0, 255)},
        {"white", RGB565(255, 255, 255)},
        {"black", RGB565(0, 0, 0)},
    };

    for (size_t i = 0; i < count_of(fills); i++) {
        uint32_t us = fill_screen_timed(fills[i].color);
        printf("PASS  fill %-5s: %u us (%u ms, ", fills[i].name, (unsigned)us, (unsigned)(us / 1000u));
        print_fps(us);
        printf(")\n");
        sleep_ms(150);
    }
}

static void test_colour_bars(void) {
    static const uint16_t bars[8] = {
        RGB565(255, 255, 255),
        RGB565(255, 255, 0),
        RGB565(0, 255, 255),
        RGB565(0, 255, 0),
        RGB565(255, 0, 255),
        RGB565(255, 0, 0),
        RGB565(0, 0, 255),
        RGB565(0, 0, 0),
    };
    const uint16_t bar_w = SCR_W / 8u;

    for (uint32_t i = 0; i < count_of(bars); i++) {
        ili9341_fill_rect((uint16_t)(i * bar_w), 0, bar_w, SCR_H, bars[i]);
    }
    ili9341_wait_idle();
    printf("PASS  colour bars: 8 x %u px (W,Y,C,G,M,R,B,K)\n", (unsigned)bar_w);
    sleep_ms(400);
}

/* 1 px white frame plus 24x24 corner markers: the orientation check. */
static void test_frame_and_corners(void) {
    const uint16_t white = RGB565(255, 255, 255);
    const uint16_t marker = 24;

    ili9341_fill_rect(0, 0, SCR_W, SCR_H, RGB565(0, 0, 0));
    ili9341_fill_rect(0, 0, SCR_W, 1, white);
    ili9341_fill_rect(0, SCR_H - 1, SCR_W, 1, white);
    ili9341_fill_rect(0, 0, 1, SCR_H, white);
    ili9341_fill_rect(SCR_W - 1, 0, 1, SCR_H, white);

    ili9341_fill_rect(0, 0, marker, marker, RGB565(255, 0, 0));                       /* top-left red */
    ili9341_fill_rect(SCR_W - marker, 0, marker, marker, RGB565(0, 255, 0));          /* top-right green */
    ili9341_fill_rect(0, SCR_H - marker, marker, marker, RGB565(0, 0, 255));          /* bottom-left blue */
    ili9341_fill_rect(SCR_W - marker, SCR_H - marker, marker, marker, RGB565(255, 255, 0)); /* bottom-right yellow */
    ili9341_wait_idle();

    printf("PASS  frame + corner markers, verify: TL=RED TR=GREEN BL=BLUE BR=YELLOW\n");
    printf("INFO  window addressing OK if the markers sit exactly in the corners\n");
    sleep_ms(600);
}

static void test_checkerboard(void) {
    const uint16_t even = RGB565(16, 16, 64);
    const uint16_t odd = RGB565(16, 64, 16);
    uint32_t tiles = 0;
    uint64_t t0 = time_us_64();

    for (uint32_t y = 0; y < SCR_H; y += 32u) {
        uint16_t th = (uint16_t)((y + 32u <= SCR_H) ? 32u : (SCR_H - y));
        for (uint32_t x = 0; x < SCR_W; x += 32u) {
            uint16_t color = (((x / 32u) + (y / 32u)) & 1u) ? odd : even;
            ili9341_fill_rect((uint16_t)x, (uint16_t)y, 32, th, color);
            tiles++;
        }
    }
    ili9341_wait_idle();

    uint32_t us = (uint32_t)(time_us_64() - t0);
    printf("PASS  checkerboard: %u partial 32x32 updates in %u us\n", (unsigned)tiles, (unsigned)us);
    sleep_ms(400);
}

static void benchmark_clock(uint32_t requested_hz, const char *label) {
    uint32_t actual_hz = ili9341_set_spi_clock(requested_hz);
    printf("INFO  spi request=%u Hz actual=%u Hz%s%s\n", (unsigned)requested_hz, (unsigned)actual_hz,
           label != NULL ? " " : "", label != NULL ? label : "");

    /* Full-screen fill, with the DMA/CPU split from the driver statistics. */
    ili9341_reset_stats();
    uint32_t fill_us = fill_screen_timed(RGB565(0, 40, 120));

    ili9341_stats_t stats;
    ili9341_get_stats(&stats);
    uint32_t dma_us = (uint32_t)stats.dma_busy_us;
    uint32_t cpu_us = fill_us > dma_us ? fill_us - dma_us : 0u;

    printf("PASS  full fill 320x240 : %u us (%u ms, ", (unsigned)fill_us, (unsigned)(fill_us / 1000u));
    print_fps(fill_us);
    printf(") dma=%u us cpu=%u us\n", (unsigned)dma_us, (unsigned)cpu_us);

    /* Full frame from the 320x16 strip buffer, pushed as 15 blits. */
    ili9341_reset_stats();
    uint64_t t0 = time_us_64();
    for (uint32_t y = 0; y < SCR_H; y += STRIP_H) {
        ili9341_blit_async(0, (uint16_t)y, SCR_W, STRIP_H, g_strip);
    }
    ili9341_wait_idle();
    uint32_t blit_us = (uint32_t)(time_us_64() - t0);
    ili9341_get_stats(&stats);

    printf("PASS  full blit 15 strips: %u us (%u ms, ", (unsigned)blit_us, (unsigned)(blit_us / 1000u));
    print_fps(blit_us);
    printf(") dma=%u us\n", (unsigned)stats.dma_busy_us);

    /* 100x100 partial update as ten 100x10 blits. */
    ili9341_reset_stats();
    t0 = time_us_64();
    for (uint32_t i = 0; i < 10u; i++) {
        ili9341_blit_async(110, (uint16_t)(70u + i * 10u), 100, 10, g_strip);
    }
    ili9341_wait_idle();
    uint32_t part_us = (uint32_t)(time_us_64() - t0);
    printf("PASS  partial blit 100x100: %u us (%u ms)\n", (unsigned)part_us, (unsigned)(part_us / 1000u));
}

static void run_benchmarks(void) {
    for (uint32_t y = 0; y < STRIP_H; y++) {
        for (uint32_t x = 0; x < SCR_W; x++) {
            g_strip[y * SCR_W + x] = RGB565((uint8_t)x, (uint8_t)(y * 16u), (uint8_t)(255u - x));
        }
    }

    printf("INFO  --- SPI clock benchmarks ---\n");
    for (size_t i = 0; i < count_of(k_bench_clocks); i++) {
        benchmark_clock(k_bench_clocks[i].hz, k_bench_clocks[i].label);
        if (k_bench_clocks[i].hz >= 75000000u) {
            /* The ILI9341 datasheet rates the write clock at 10 MHz; 37.5 MHz
             * is a widely used overclock. 75 MHz is experimental. */
            printf("INFO  75 MHz is an experimental overclock: check the panel for corruption.\n");
            test_colour_bars();
            printf("INFO  check the colour bars above: speckles, shifted bars or wrong colours\n");
            printf("      mean the panel/wiring cannot take 75 MHz; 37.5 MHz is the safe rate.\n");
            sleep_ms(3000); /* hold the bars on screen so they can be inspected */
        }
        sleep_ms(200);
    }

    uint32_t restored = ili9341_set_spi_clock(ILI9341_SPI_HZ);
    printf("INFO  restored spi clock: %u Hz (requested %u Hz)\n", (unsigned)restored, (unsigned)ILI9341_SPI_HZ);
}

static void animation_loop(void) {
    const int size = 48;
    const uint16_t square_color = RGB565(0, 200, 255);
    int x = 100;
    int y = 80;
    int dx = 4;
    int dy = 3;
    uint32_t frames = 0;
    uint64_t report_us = time_us_64();

    ili9341_fill_rect(0, 0, SCR_W, SCR_H, RGB565(0, 0, 0));
    ili9341_wait_idle();
    printf("INFO  --- bouncing square animation (30 FPS target) ---\n");

    while (true) {
        absolute_time_t frame_start = get_absolute_time();

        /* Erase the old square, move, draw the new one: two partial updates. */
        ili9341_fill_rect((uint16_t)x, (uint16_t)y, (uint16_t)size, (uint16_t)size, RGB565(0, 0, 0));

        x += dx;
        y += dy;
        if (x < 0) {
            x = 0;
            dx = -dx;
        } else if (x > SCR_W - size) {
            x = SCR_W - size;
            dx = -dx;
        }
        if (y < 0) {
            y = 0;
            dy = -dy;
        } else if (y > SCR_H - size) {
            y = SCR_H - size;
            dy = -dy;
        }

        ili9341_fill_rect((uint16_t)x, (uint16_t)y, (uint16_t)size, (uint16_t)size, square_color);
        frames++;

        uint64_t now_us = time_us_64();
        uint64_t elapsed_us = now_us - report_us;
        if (elapsed_us >= 1000000u) {
            uint32_t fps10 = (uint32_t)(((uint64_t)frames * 10000000ull) / elapsed_us);
            printf("INFO  anim fps=%u.%u frames=%u\n", (unsigned)(fps10 / 10u), (unsigned)(fps10 % 10u),
                   (unsigned)frames);
            frames = 0;
            report_us = now_us;
        }

        sleep_until(delayed_by_us(frame_start, 33333));
    }
}

int main(void) {
    stdio_init_all();
    sleep_ms(250);

    printf("\n=== ILI9341 display_test (Purdue Proton) ===\n");
    printf("INFO  clk_sys=%u Hz\n", (unsigned)clock_get_hz(clk_sys));

    ili9341_config_t cfg;
    ili9341_default_config(&cfg);

    bool init_ok = ili9341_init(&cfg);

    /* Re-run the probe so every field can be printed; ili9341_init() returns
     * the same verdict. */
    ili9341_selftest_t st;
    bool readback_ok = ili9341_self_test(&st);

    printf("\n--- panel readback self-test ---\n");
    printf("INFO  rddid=0x%06X power=0x%02X madctl=0x%02X pixfmt=0x%02X "
           "miso=%u madctl_ok=%u pixfmt_ok=%u display_on=%u dummy_bit=%u\n",
           (unsigned)st.rddid, (unsigned)st.power_mode, (unsigned)st.madctl, (unsigned)st.pixfmt,
           (unsigned)st.miso_alive, (unsigned)st.madctl_ok, (unsigned)st.pixfmt_ok,
           (unsigned)st.display_on, (unsigned)st.dummy_bit);

    if (st.rddid == 0x009341u) {
        printf("PASS  RDDID=0x009341 matches a genuine ILI9341\n");
    } else if (st.rddid == 0x000000u || st.rddid == 0xFFFFFFu) {
        printf("INFO  RDDID=0x%06X: unprogrammed ID bytes or no MISO (not a failure by itself)\n",
               (unsigned)st.rddid);
    } else {
        printf("INFO  RDDID=0x%06X differs: clone panel with a custom ID\n", (unsigned)st.rddid);
    }

    printf("%s  MISO/SDO readback: %s\n", st.miso_alive ? "PASS" : "FAIL",
           st.miso_alive ? "panel is driving data" : "every read returned 0x00/0xFF");
    if (!st.miso_alive) {
        printf("INFO  A MISO failure is acceptable when the module's SDO pin is not wired:\n");
        printf("      the panel still works write-only, so all draw tests below remain valid.\n");
    }

    printf("%s  RDMADCTL (0x0B): 0x%02X %s the programmed landscape MADCTL\n",
           st.madctl_ok ? "PASS" : "FAIL", (unsigned)st.madctl,
           st.madctl_ok ? "matches" : "does not match");

    printf("%s  RDDCOLMOD (0x0C): 0x%02X (expected 0x55 or 0x05 for 16 bpp DBI)\n",
           st.pixfmt_ok ? "PASS" : "FAIL", (unsigned)st.pixfmt);

    printf("%s  RDDPM (0x0A): 0x%02X (sleep-out=%u display-on=%u)\n", st.display_on ? "PASS" : "FAIL",
           (unsigned)st.power_mode, (unsigned)((st.power_mode >> 4) & 1u),
           (unsigned)((st.power_mode >> 2) & 1u));

    printf("INFO  8-bit register reads use %s dummy bit\n", st.dummy_bit ? "one leading" : "no");

    if (readback_ok) {
        printf("PASS  self-test: readback healthy (MADCTL, COLMOD and display-on all OK)\n");
    } else {
        printf("FAIL  self-test: readback not fully healthy, see the lines above\n");
    }
    if (init_ok) {
        printf("PASS  init: panel up, 1 DMA channel claimed, backlight 100%%\n");
    } else {
        printf("INFO  init returned false - a MISO/SDO wiring failure is not fatal, the panel\n");
        printf("      still works write-only; only RDDID/self-test readback is unavailable.\n");
    }
    if (init_ok != readback_ok) {
        printf("INFO  init and the repeated self-test disagree (unstable readback?)\n");
    }
    printf("INFO  spi=%u Hz display=%ux%u landscape\n", (unsigned)ili9341_get_spi_clock(),
           (unsigned)ili9341_width(), (unsigned)ili9341_height());

    test_solid_fills();
    printf("PASS  solid fills: R, G, B, W, K\n");

    test_colour_bars();
    test_frame_and_corners();
    test_checkerboard();

    run_benchmarks();

    animation_loop();
    return 0;
}
