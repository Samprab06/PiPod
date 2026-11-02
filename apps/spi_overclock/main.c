/*
 * spi_overclock: ILI9341 SPI clock margin sweep for the Purdue Proton.
 *
 * clk_sys stays at 150 MHz. To clock SPI above the 75 MHz PL022 limit
 * (SPI = clk_peri / 2), clk_peri alone is overclocked: PLL_USB is programmed
 * to 2 * target SPI rate and clk_peri is switched to it with divider 1.
 * USB and the ADC are unused in this app; stdio stays on UART0 (clk_peri),
 * so the baud rate is reprogrammed after every clock change.
 *
 * Per step the app:
 *   - draws a deterministic test pattern through 320 x 16 strip buffers
 *   - optionally reads regions back over MISO at <= 6 MHz and compares them
 *     with the same scene regenerated in software
 *   - runs a 5 s full-frame animation plus a 320 x 120 partial-update
 *     measurement
 *   - holds the static pattern for visual inspection and waits for input
 *
 * Keys: n = next, p = previous, r = redo, s = safe 37.5 MHz (re-init),
 *       a = auto (advance every 8 s), t = summary table.
 * Buttons: GP21 = next, GP26 = previous (on-board 10k pull-downs).
 */

#include <stdio.h>
#include <string.h>

#include "fonts/fonts.h"
#include "gfx.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pll.h"
#include "hardware/spi.h"
#include "hardware/uart.h"
#include "ili9341.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#define SCR_W 320
#define SCR_H 240
#define BAND_H 16
#define BANNER_H 44

/* Partial-update window: 320 x 120 rows of the pattern, below the banner. */
#define PARTIAL_Y 48
#define PARTIAL_H 120
#define PARTIAL_BAND_H 15

#define BTN_NEXT_PIN 21
#define BTN_PREV_PIN 26
#define BTN_DEBOUNCE_MS 20u

#define COL_BLACK 0x0000u
#define COL_WHITE 0xFFFFu
#define COL_GREY 0xB5B6u  /* GFX_RGB(160, 160, 170) */
#define COL_CYAN 0x07FFu  /* GFX_RGB(0, 220, 255) */

#define SAFE_SPI_HZ 37500000u

/* SPI targets, in Hz. 66.67 is the 133.33 MHz clk_peri point. */
typedef struct {
    uint32_t target_spi_hz;
    const char *label;
} step_def_t;

static const step_def_t k_steps[] = {
    {37500000u, "37.5"},
    {62500000u, "62.5"},
    {66670000u, "66.67"},
    {70000000u, "70.0"},
    {75000000u, "75.0"},
    {80000000u, "80.0"},
    {85000000u, "85.0"},
    {90000000u, "90.0"},
    {100000000u, "100.0"},
};

#define STEP_COUNT ((size_t)(sizeof(k_steps) / sizeof(k_steps[0])))

/* Two strip buffers, alternated exactly like gfx_render: a blit waits for the
 * previous transfer, so the buffer used two blits ago is free to draw into. */
static uint16_t s_buf[2][SCR_W * BAND_H];
static uint16_t s_expect[SCR_W * BAND_H];
static uint16_t s_got[SCR_W * BAND_H];
static uint8_t s_buf_index;

typedef struct {
    const char *label; /* "80.0 MHz" */
    unsigned step;     /* 1-based for display */
    unsigned total;
    uint8_t phase;     /* 0 = normal, 1 = colour-shifted (inverted) */
    bool show_fps;
    uint32_t fps_x10;
} scene_t;

typedef struct {
    uint32_t target_spi_hz;
    uint32_t peri_hz;
    uint32_t vco_hz;
    uint32_t pd1;
    uint32_t pd2;
    bool exact;
} clock_plan_t;

typedef struct {
    uint32_t actual_spi_hz;
    uint32_t peri_hz;
    uint32_t vco_hz;
    uint32_t pd1;
    uint32_t pd2;
    uint32_t full_fps_x10;
    uint32_t partial_fps_x10;
    int32_t mismatches; /* -1 not run, -2 no MISO, -3 ignored */
    uint32_t checked_pixels;
    bool done;
} step_result_t;

static step_result_t s_results[STEP_COUNT];

typedef struct {
    uint16_t x, y, w, h;
} read_region_t;

static read_region_t s_regions[7];

static bool s_miso_alive;
static bool s_readback_trusted;
static bool s_auto_mode;

typedef struct {
    uint pin;
    bool raw;
    bool stable;
    uint32_t changed_ms;
} button_t;

static button_t s_btn_next;
static button_t s_btn_prev;

/* ------------------------------------------------------------------------- */
/* Test pattern                                                              */
/* ------------------------------------------------------------------------- */

static const uint16_t k_bars[8] = {
    COL_WHITE,
    GFX_RGB(255, 255, 0),
    GFX_RGB(0, 255, 255),
    GFX_RGB(0, 255, 0),
    GFX_RGB(255, 0, 255),
    GFX_RGB(255, 0, 0),
    GFX_RGB(0, 0, 255),
    COL_BLACK,
};

static uint16_t gradient_pixel(int x, int band) {
    uint32_t v = ((uint32_t)x * 255u) / (uint32_t)(SCR_W - 1);
    switch (band) {
        case 0:
            return GFX_RGB(v, 0u, 0u);
        case 1:
            return GFX_RGB(0u, v, 0u);
        case 2:
            return GFX_RGB(0u, 0u, v);
        default:
            return GFX_RGB(v, v, v);
    }
}

/* Pure function of (x, y, phase): the same source generates both the pixels
 * sent to the panel and the pixels expected back from MISO. */
static uint16_t pattern_pixel(int x, int y, uint8_t phase) {
    uint16_t c;

    if (y < BANNER_H) {
        c = COL_BLACK;
    } else if (y < 80 && x < 196) {
        /* 192 x 32, 1 px black/white checkerboard. */
        c = (((x + y) & 1) != 0) ? COL_WHITE : COL_BLACK;
    } else if (y >= 84 && y < 108) {
        /* 1 px vertical stripes. */
        c = ((x & 1) != 0) ? COL_WHITE : COL_BLACK;
    } else if (y >= 112 && y < 136) {
        /* 1 px horizontal stripes. */
        c = ((y & 1) != 0) ? COL_WHITE : COL_BLACK;
    } else if (y >= 140 && y < 164) {
        /* 8 colour bars, 40 px each (W,Y,C,G,M,R,B,K). */
        c = k_bars[x / 40];
    } else if (y >= 168 && y < 192) {
        /* R, G, B and grey horizontal gradients, 6 rows each. */
        c = gradient_pixel(x, (y - 168) / 6);
    } else {
        c = COL_BLACK;
    }

    return phase != 0u ? (uint16_t)~c : c;
}

static void scene_draw(gfx_canvas_t *c, void *user) {
    const scene_t *sc = (const scene_t *)user;
    int ax = c->area.x;
    int ay = c->area.y;
    int aw = c->area.w;
    int ah = c->area.h;

    for (int row = 0; row < ah; row++) {
        uint16_t *dst = c->buf + (size_t)row * (size_t)aw;
        for (int col = 0; col < aw; col++) {
            dst[col] = pattern_pixel(ax + col, ay + row, sc->phase);
        }
    }

    char tmp[32];

    /* Banner. */
    gfx_fill_rect(c, 0, 0, SCR_W, BANNER_H, COL_BLACK);
    gfx_draw_text(c, &font_inter_28b, 8, 34, sc->label, COL_WHITE);

    snprintf(tmp, sizeof tmp, "step %u/%u", sc->step, sc->total);
    int w = gfx_text_width(&font_inter_12, tmp);
    gfx_draw_text(c, &font_inter_12, SCR_W - 8 - w, 16, tmp, COL_GREY);

    if (sc->show_fps) {
        snprintf(tmp, sizeof tmp, "fps %u.%u", (unsigned)(sc->fps_x10 / 10u),
                 (unsigned)(sc->fps_x10 % 10u));
        w = gfx_text_width(&font_inter_12, tmp);
        gfx_draw_text(c, &font_inter_12, SCR_W - 8 - w, 36, tmp, COL_CYAN);
    }

    /* Small text sample on a black band so it stays readable. */
    gfx_fill_rect(c, 0, 194, SCR_W, 24, COL_BLACK);
    gfx_draw_text(c, &font_inter_12, 8, 212, "The quick brown fox jumps over 13 lazy dogs",
                  COL_WHITE);

    /* 1 px screen border. */
    gfx_fill_rect(c, 0, 0, SCR_W, 1, COL_WHITE);
    gfx_fill_rect(c, 0, SCR_H - 1, SCR_W, 1, COL_WHITE);
    gfx_fill_rect(c, 0, 0, 1, SCR_H, COL_WHITE);
    gfx_fill_rect(c, SCR_W - 1, 0, 1, SCR_H, COL_WHITE);
}

static void render_band(int x, int y, int w, int h, const scene_t *sc) {
    uint16_t *buf = s_buf[s_buf_index];
    gfx_canvas_t c = {
        .buf = buf,
        .area = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h},
        .clip = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h},
    };

    scene_draw(&c, (void *)sc);
    ili9341_blit_async((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, buf);
    s_buf_index ^= 1u;
}

static void render_full(const scene_t *sc) {
    for (int y = 0; y < SCR_H; y += BAND_H) {
        render_band(0, y, SCR_W, BAND_H, sc);
    }
}

static void render_partial(const scene_t *sc) {
    for (int i = 0; i < PARTIAL_H / PARTIAL_BAND_H; i++) {
        render_band(0, PARTIAL_Y + i * PARTIAL_BAND_H, SCR_W, PARTIAL_BAND_H, sc);
    }
}

static void render_expected(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const scene_t *sc,
                            uint16_t *buf) {
    gfx_canvas_t c = {
        .buf = buf,
        .area = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h},
        .clip = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h},
    };
    scene_draw(&c, (void *)sc);
}

/* ------------------------------------------------------------------------- */
/* Clock plan                                                                */
/* ------------------------------------------------------------------------- */

static void print_mhz(uint32_t hz) {
    printf("%u.%03u", (unsigned)(hz / 1000000u), (unsigned)((hz % 1000000u) / 1000u));
}

/* PL022 clock = clk_peri / (CPSDVSR * (1 + SCR)), CPSDVSR even >= 2, so the
 * highest possible SPI clock is clk_peri / 2. */
static bool compute_plan(uint32_t target_spi_hz, clock_plan_t *plan) {
    uint32_t wanted = target_spi_hz * 2u;
    uint vco = 0;
    uint pd1 = 0;
    uint pd2 = 0;

    memset(plan, 0, sizeof *plan);
    plan->target_spi_hz = target_spi_hz;

    if (wanted != 0u && check_sys_clock_hz(wanted, &vco, &pd1, &pd2)) {
        plan->vco_hz = vco;
        plan->pd1 = pd1;
        plan->pd2 = pd2;
        plan->peri_hz = vco / (pd1 * pd2);
        plan->exact = true;
        return true;
    }

    /* Nearest achievable output. check_sys_clock_hz() only accepts exact
     * integer results, but 66.67 MHz needs 133.333 MHz clk_peri, which exists
     * as a fractional PLL division. */
    uint32_t best_diff = UINT32_MAX;
    uint32_t best_out = 0;
    uint32_t best_vco = 0;
    uint32_t best_pd1 = 0;
    uint32_t best_pd2 = 0;
    uint32_t ref_hz = XOSC_HZ / PLL_SYS_REFDIV;

    for (uint fbdiv = 320; fbdiv >= 16; fbdiv--) {
        uint32_t vco_hz = (uint32_t)fbdiv * ref_hz;
        if (vco_hz < PICO_PLL_VCO_MIN_FREQ_HZ || vco_hz > PICO_PLL_VCO_MAX_FREQ_HZ) {
            continue;
        }
        for (uint a = 7; a >= 1; a--) {
            for (uint b = a; b >= 1; b--) {
                uint32_t div = a * b;
                uint32_t out = vco_hz / div;
                uint32_t diff = out > wanted ? out - wanted : wanted - out;
                if (diff < best_diff || (diff == best_diff && vco_hz > best_vco)) {
                    best_diff = diff;
                    best_out = out;
                    best_vco = vco_hz;
                    best_pd1 = a;
                    best_pd2 = b;
                }
            }
        }
    }

    if (best_diff == UINT32_MAX) {
        return false;
    }

    plan->vco_hz = best_vco;
    plan->pd1 = best_pd1;
    plan->pd2 = best_pd2;
    plan->peri_hz = best_out;
    plan->exact = false;
    return true;
}

static void print_plan_table(void) {
    printf("\n=== clock plan: SPI = clk_peri / 2 (CPSDVSR=2, SCR=0) ===\n");
    printf("step  target SPI MHz   clk_peri MHz   PLL_USB vco MHz  pd1  pd2   actual clk_peri MHz\n");

    for (size_t i = 0; i < STEP_COUNT; i++) {
        if (i == 0) {
            printf(" %u     %8s       150.000         -           -    -      150.000 (clk_sys)\n",
                   (unsigned)(i + 1), k_steps[i].label);
            continue;
        }
        clock_plan_t plan;
        if (!compute_plan(k_steps[i].target_spi_hz, &plan)) {
            printf(" %u     %8s       no valid PLL setting\n", (unsigned)(i + 1), k_steps[i].label);
            continue;
        }
        printf(" %u     %8s       ", (unsigned)(i + 1), k_steps[i].label);
        print_mhz(k_steps[i].target_spi_hz * 2u);
        printf("       ");
        print_mhz(plan.vco_hz);
        printf("       %u    %u      ", (unsigned)plan.pd1, (unsigned)plan.pd2);
        print_mhz(plan.peri_hz);
        printf("%s\n", plan.exact ? "" : " (nearest)");
    }
}

/* Park clk_peri on clk_sys, then optionally reprogram PLL_USB and switch
 * clk_peri to it. UART0 runs from clk_peri, so the baud rate is restored after
 * every change. */
static void apply_plan(const clock_plan_t *plan, bool safe) {
    uint32_t sys_hz = clock_get_hz(clk_sys);

    /* No DMA may be in flight while clk_peri (and therefore SPI) changes. */
    ili9341_wait_idle();
    uart_tx_wait_blocking(uart0);

    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, sys_hz, sys_hz);
    uart_set_baudrate(uart0, 115200u);

    if (!safe) {
        pll_init(pll_usb, 1u, plan->vco_hz, plan->pd1, plan->pd2);
        clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB, plan->peri_hz,
                        plan->peri_hz);
        uart_set_baudrate(uart0, 115200u);
    }

    uint32_t spi_hz = safe ? SAFE_SPI_HZ : clock_get_hz(clk_peri) / 2u;
    (void)ili9341_set_spi_clock(spi_hz);
}

static void print_step_clock(const clock_plan_t *plan, bool safe) {
    uint32_t peri_hz = clock_get_hz(clk_peri);
    uint32_t spi_hz = ili9341_get_spi_clock();
    spi_hw_t *hw = spi_get_hw(spi0);
    uint32_t cpsdvsr = hw->cpsr & 0xFFu;
    uint32_t scr = (hw->cr0 >> 8) & 0xFFu;

    if (safe) {
        printf("[clock] safe: clk_peri = clk_sys = ");
        print_mhz(peri_hz);
        printf(" MHz, SPI = ");
        print_mhz(spi_hz);
        printf(" MHz\n");
    } else {
        printf("[clock] PLL_USB vco=");
        print_mhz(plan->vco_hz);
        printf(" MHz pd1=%u pd2=%u -> clk_peri requested=", (unsigned)plan->pd1,
               (unsigned)plan->pd2);
        print_mhz(plan->target_spi_hz * 2u);
        printf(" actual=");
        print_mhz(peri_hz);
        printf(" MHz -> SPI actual=");
        print_mhz(spi_hz);
        printf(" MHz%s\n", plan->exact ? "" : " (nearest achievable)");
    }

    printf("[clock] SPI registers: CPSDVSR=%u SCR=%u (SPI = clk_peri/%u)\n", (unsigned)cpsdvsr,
           (unsigned)scr, (unsigned)(cpsdvsr * (scr + 1u)));

    uint32_t expected = peri_hz / 2u;
    if (!safe && spi_hz + 1u < expected) {
        printf("[clock] WARN SPI clock %u Hz is below the expected clk_peri/2 (%u Hz)\n",
               (unsigned)spi_hz, (unsigned)expected);
    }
}

/* ------------------------------------------------------------------------- */
/* Readback check                                                            */
/* ------------------------------------------------------------------------- */

static void make_regions(void) {
    s_regions[0] = (read_region_t){4, 48, 64, 16};   /* checkerboard */
    s_regions[1] = (read_region_t){0, 168, 320, 8};  /* gradient rows */
    s_regions[2] = (read_region_t){0, 140, 320, 8};  /* colour bars */

    /* Four deterministic 16x16 blocks inside the pattern area (below the
     * banner and above the text sample band). */
    uint32_t seed = 0x13579BDFu;
    for (size_t i = 0; i < 4; i++) {
        seed = seed * 1664525u + 1013904223u;
        uint16_t x = (uint16_t)(seed % (uint32_t)(SCR_W - 16));
        seed = seed * 1664525u + 1013904223u;
        uint16_t y = (uint16_t)(48u + seed % 131u); /* 48..178, block ends <= 193 */
        s_regions[3 + i] = (read_region_t){x, y, 16, 16};
    }
}

static int32_t readback_check(int step, const scene_t *sc) {
    if (!s_miso_alive) {
        return -2;
    }
    if (!s_readback_trusted) {
        return -3;
    }

    uint32_t total = 0;
    uint32_t bad = 0;
    uint32_t shown = 0;

    for (size_t r = 0; r < count_of(s_regions); r++) {
        const read_region_t *rg = &s_regions[r];
        uint32_t npx = (uint32_t)rg->w * (uint32_t)rg->h;

        render_expected(rg->x, rg->y, rg->w, rg->h, sc, s_expect);

        if (!ili9341_read_pixels(rg->x, rg->y, rg->w, rg->h, s_got)) {
            printf("[check] readback failed for (%u,%u) %ux%u: MISO looks dead\n", (unsigned)rg->x,
                   (unsigned)rg->y, (unsigned)rg->w, (unsigned)rg->h);
            return -2;
        }

        for (uint32_t i = 0; i < npx; i++) {
            uint16_t expect = s_expect[i];
            uint16_t got = s_got[i];
            total++;
            if (expect != got) {
                bad++;
                if (shown < 3u) {
                    printf("[check]   mismatch (%u,%u) expected=0x%04X got=0x%04X\n",
                           (unsigned)(rg->x + (i % rg->w)), (unsigned)(rg->y + (i / rg->w)),
                           (unsigned)expect, (unsigned)got);
                    shown++;
                }
            }
        }
    }

    printf("[check] mismatched pixels %u / %u\n", (unsigned)bad, (unsigned)total);

    if (step == 0 && bad != 0) {
        s_readback_trusted = false;
        printf("[check] baseline mismatch: the readback path itself is unreliable on this module.\n");
        printf("[check] ignoring readback results for the remaining steps.\n");
    }

    return (int32_t)bad;
}

/* ------------------------------------------------------------------------- */
/* Animation                                                                 */
/* ------------------------------------------------------------------------- */

static uint32_t measure_animation(scene_t *sc, uint32_t *partial_out) {
    const uint64_t duration_us = 5000000ull;
    uint64_t start = time_us_64();
    uint64_t window_start = start;
    uint32_t frames = 0;
    uint32_t window_frames = 0;
    uint32_t fps_x10 = 0;

    while (time_us_64() - start < duration_us) {
        sc->phase = (uint8_t)(frames & 1u);
        sc->show_fps = true;
        sc->fps_x10 = fps_x10;

        render_full(sc);
        ili9341_wait_idle();
        frames++;
        window_frames++;

        uint64_t now = time_us_64();
        if (now - window_start >= 1000000ull) {
            fps_x10 = (uint32_t)(((uint64_t)window_frames * 10000000ull) / (now - window_start));
            printf("[anim] full-frame fps=%u.%u\n", (unsigned)(fps_x10 / 10u),
                   (unsigned)(fps_x10 % 10u));
            window_frames = 0;
            window_start = now;
        }
    }

    uint64_t elapsed = time_us_64() - start;
    uint32_t full_fps_x10 = elapsed != 0u
                                ? (uint32_t)(((uint64_t)frames * 10000000ull) / elapsed)
                                : 0u;

    /* Partial update: 320 x 120 region in 8 bands of 15 rows. */
    uint64_t pstart = time_us_64();
    uint32_t updates = 0;
    while (time_us_64() - pstart < 1000000ull) {
        sc->phase = (uint8_t)(updates & 1u);
        sc->show_fps = true;
        sc->fps_x10 = full_fps_x10;
        render_partial(sc);
        ili9341_wait_idle();
        updates++;
    }
    uint64_t pelapsed = time_us_64() - pstart;
    uint32_t partial_fps_x10 =
        pelapsed != 0u ? (uint32_t)(((uint64_t)updates * 10000000ull) / pelapsed) : 0u;

    printf("[anim] step result: full-frame fps=%u.%u (%u frames), partial 320x120 fps=%u.%u "
           "(%u updates)\n",
           (unsigned)(full_fps_x10 / 10u), (unsigned)(full_fps_x10 % 10u), (unsigned)frames,
           (unsigned)(partial_fps_x10 / 10u), (unsigned)(partial_fps_x10 % 10u),
           (unsigned)updates);

    sc->phase = 0;
    sc->show_fps = true;
    sc->fps_x10 = full_fps_x10;
    *partial_out = partial_fps_x10;
    return full_fps_x10;
}

/* ------------------------------------------------------------------------- */
/* Input and summary                                                         */
/* ------------------------------------------------------------------------- */

typedef enum {
    ACT_NONE = 0,
    ACT_NEXT,
    ACT_PREV,
    ACT_REDO,
    ACT_SAFE,
    ACT_AUTO,
    ACT_TABLE,
} action_t;

static void button_init(button_t *b, uint pin) {
    b->pin = pin;
    b->raw = false;
    b->stable = false;
    b->changed_ms = 0;

    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    /* On-board 10k pull-downs are fitted; RP2350-E9 makes internal pulls
     * unreliable/floating, so leave them disabled like lib/input does. */
    gpio_disable_pulls(pin);
}

static bool button_pressed(button_t *b, uint32_t now_ms) {
    bool raw = gpio_get(b->pin);
    if (raw != b->raw) {
        b->raw = raw;
        b->changed_ms = now_ms;
    }
    if (raw != b->stable && (uint32_t)(now_ms - b->changed_ms) >= BTN_DEBOUNCE_MS) {
        b->stable = raw;
        return raw;
    }
    return false;
}

static action_t poll_input(void) {
    int c = getchar_timeout_us(0);
    if (c != PICO_ERROR_TIMEOUT) {
        switch (c) {
            case 'n':
            case 'N':
                return ACT_NEXT;
            case 'p':
            case 'P':
                return ACT_PREV;
            case 'r':
            case 'R':
                return ACT_REDO;
            case 's':
            case 'S':
                return ACT_SAFE;
            case 'a':
            case 'A':
                return ACT_AUTO;
            case 't':
            case 'T':
                return ACT_TABLE;
            default:
                break;
        }
    }

    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (button_pressed(&s_btn_next, now_ms)) {
        return ACT_NEXT;
    }
    if (button_pressed(&s_btn_prev, now_ms)) {
        return ACT_PREV;
    }
    return ACT_NONE;
}

static action_t hold_current_step(uint64_t step_start_us) {
    while (true) {
        action_t act = poll_input();
        if (act != ACT_NONE) {
            return act;
        }
        if (s_auto_mode && (time_us_64() - step_start_us) >= 8000000ull) {
            return ACT_NEXT;
        }
        sleep_ms(10);
    }
}

static void print_summary(void) {
    printf("\n=== summary ===\n");
    printf("step  target MHz  actual SPI MHz  full-frame fps  partial fps  readback mismatches\n");

    for (size_t i = 0; i < STEP_COUNT; i++) {
        const step_result_t *r = &s_results[i];
        if (!r->done) {
            printf(" %u/%u   %8s      not run\n", (unsigned)(i + 1), (unsigned)STEP_COUNT,
                   k_steps[i].label);
            continue;
        }

        printf(" %u/%u   %8s   %9u.%03u     %6u.%u       %6u.%u      ", (unsigned)(i + 1),
               (unsigned)STEP_COUNT, k_steps[i].label, (unsigned)(r->actual_spi_hz / 1000000u),
               (unsigned)((r->actual_spi_hz % 1000000u) / 1000u),
               (unsigned)(r->full_fps_x10 / 10u), (unsigned)(r->full_fps_x10 % 10u),
               (unsigned)(r->partial_fps_x10 / 10u), (unsigned)(r->partial_fps_x10 % 10u));

        if (r->mismatches >= 0) {
            printf("%d / %u\n", (int)r->mismatches, (unsigned)r->checked_pixels);
        } else if (r->mismatches == -2) {
            printf("no MISO\n");
        } else if (r->mismatches == -3) {
            printf("ignored\n");
        } else {
            printf("not checked\n");
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Main                                                                      */
/* ------------------------------------------------------------------------- */

int main(void) {
    stdio_init_all();
    sleep_ms(250);

    printf("\n=== PiPod SPI overclock sweep ===\n");
    printf("[boot] clk_sys=");
    print_mhz(clock_get_hz(clk_sys));
    printf(" MHz, clk_peri=");
    print_mhz(clock_get_hz(clk_peri));
    printf(" MHz\n");
    printf("[boot] PL022 limit: SPI = clk_peri/(CPSDVSR*(1+SCR)), CPSDVSR>=2 -> max SPI = "
           "clk_peri/2\n");
    printf("[boot] overclock method: clk_sys stays at 150 MHz, clk_peri is driven from PLL_USB "
           "(USB/ADC unused)\n");

    ili9341_config_t cfg;
    ili9341_default_config(&cfg);
    cfg.spi_hz = SAFE_SPI_HZ;

    bool init_ok = ili9341_init(&cfg);
    ili9341_selftest_t st;
    (void)ili9341_self_test(&st);

    printf("[init] ili9341_init=%u rddid=0x%06X miso_alive=%u dummy_bit=%u madctl_ok=%u "
           "pixfmt_ok=%u display_on=%u\n",
           (unsigned)init_ok, (unsigned)st.rddid, (unsigned)st.miso_alive,
           (unsigned)st.dummy_bit, (unsigned)st.madctl_ok, (unsigned)st.pixfmt_ok,
           (unsigned)st.display_on);

    if (st.miso_alive) {
        printf("[init] MISO readback available: pixel readback checks enabled\n");
    } else {
        printf("[init] MISO readback NOT available: write-only run, readback checks disabled\n");
    }

    ili9341_set_pin_drive(true);
    printf("[init] SCK/MOSI/CS/DC pads: 12 mA, fast slew\n");

    s_miso_alive = st.miso_alive;
    s_readback_trusted = st.miso_alive;

    button_init(&s_btn_next, BTN_NEXT_PIN);
    button_init(&s_btn_prev, BTN_PREV_PIN);
    make_regions();
    print_plan_table();

    printf("\n[input] n=next p=previous r=redo s=safe 37.5 MHz a=auto t=summary "
           "(GP21=next, GP26=previous)\n");

    int step = 0;
    bool need_reinit = false;
    bool safe_step_entered = false;

    while (true) {
        uint64_t step_start = time_us_64();
        bool safe = (step == 0);
        clock_plan_t plan;

        memset(&plan, 0, sizeof plan);

        if (!safe) {
            if (!compute_plan(k_steps[step].target_spi_hz, &plan)) {
                printf("FAIL no PLL setting for step %d, returning to the safe step\n", step + 1);
                step = 0;
                need_reinit = true;
                continue;
            }
            apply_plan(&plan, false);
        } else {
            apply_plan(&plan, true);
            /* Every later entry to the safe step (s, p or a failed plan)
             * re-runs the full panel init: the panel may have been left in an
             * unknown state by a clock it could not follow. The boot init
             * above already covers the first entry. */
            if (safe_step_entered || need_reinit) {
                bool ok = ili9341_init(&cfg);
                (void)ili9341_self_test(&st);
                ili9341_set_pin_drive(true);
                s_miso_alive = st.miso_alive;
                s_readback_trusted = st.miso_alive;
                printf("[recover] ili9341_init=%u miso_alive=%u\n", (unsigned)ok,
                       (unsigned)st.miso_alive);
            }
            safe_step_entered = true;
            need_reinit = false;
        }

        printf("\n--- step %d/%d: target SPI %s MHz ---\n", step + 1, (int)STEP_COUNT,
               k_steps[step].label);
        print_step_clock(&plan, safe);

        char label[16];
        snprintf(label, sizeof label, "%s MHz", k_steps[step].label);

        scene_t sc = {
            .label = label,
            .step = (unsigned)(step + 1),
            .total = (unsigned)STEP_COUNT,
            .phase = 0,
            .show_fps = false,
            .fps_x10 = 0,
        };

        /* Static pattern, then the optional MISO comparison. */
        render_full(&sc);
        ili9341_wait_idle();

        int32_t mismatches = -1;
        uint32_t checked = 0;
        if (s_miso_alive) {
            mismatches = readback_check(step, &sc);
            if (mismatches >= 0) {
                checked = 0;
                for (size_t r = 0; r < count_of(s_regions); r++) {
                    checked += (uint32_t)s_regions[r].w * (uint32_t)s_regions[r].h;
                }
            }
        } else {
            printf("[check] skipped: no MISO readback\n");
        }

        /* 5 s full-frame animation + partial-update measurement. */
        uint32_t partial_fps_x10 = 0;
        uint32_t full_fps_x10 = measure_animation(&sc, &partial_fps_x10);

        /* Hold the static pattern with the measured fps on the banner. */
        render_full(&sc);
        ili9341_wait_idle();

        step_result_t *res = &s_results[step];
        res->actual_spi_hz = ili9341_get_spi_clock();
        res->peri_hz = clock_get_hz(clk_peri);
        res->vco_hz = plan.vco_hz;
        res->pd1 = plan.pd1;
        res->pd2 = plan.pd2;
        res->full_fps_x10 = full_fps_x10;
        res->partial_fps_x10 = partial_fps_x10;
        res->mismatches = mismatches;
        res->checked_pixels = checked;
        res->done = true;

        printf("\n[hold] step %d/%d at %u.%03u MHz SPI.\n", step + 1, (int)STEP_COUNT,
               (unsigned)(res->actual_spi_hz / 1000000u),
               (unsigned)((res->actual_spi_hz % 1000000u) / 1000u));
        printf("[hold] look for: sparkles/noise in the 1px checkerboard, shifted rows, wrong\n");
        printf("[hold] colours, garbled text, a white or blank screen. Note the highest step that\n");
        printf("[hold] still looks clean. If the panel dies, press 's' to recover at 37.5 MHz.\n");
        printf("[hold] keys: n=next p=prev r=redo s=safe a=auto t=summary\n");

        bool restart = false;
        while (!restart) {
            action_t act = hold_current_step(step_start);
            switch (act) {
                case ACT_NEXT:
                    if (step == (int)STEP_COUNT - 1) {
                        print_summary();
                        s_auto_mode = false;
                    } else {
                        step++;
                        restart = true;
                    }
                    break;
                case ACT_PREV:
                    if (step > 0) {
                        step--;
                        restart = true;
                    }
                    break;
                case ACT_REDO:
                    restart = true;
                    break;
                case ACT_SAFE:
                    step = 0;
                    need_reinit = true;
                    restart = true;
                    break;
                case ACT_AUTO:
                    s_auto_mode = !s_auto_mode;
                    printf("[auto] %s (advance every 8 s)\n", s_auto_mode ? "on" : "off");
                    break;
                case ACT_TABLE:
                    print_summary();
                    break;
                default:
                    break;
            }
        }
    }
}
