/*
 * motion_test: animation tests for the ILI9341 at 75 MHz SPI.
 *
 * 75 MHz is clk_peri / 2 at the stock 150 MHz clk_peri, so no clock changes
 * are needed: the panel is initialised at the default rate and then switched
 * to 75 MHz at runtime. lib/display defaults are left untouched.
 *
 * Scenes (all motion is time based, so speed does not change with frame rate):
 *   1. scroll  - full-screen colour bars scrolling sideways, full redraw
 *   2. balls   - anti-aliased balls over a gradient, partial updates only
 *   3. list    - iPod-style text rows scrolling vertically under a fixed bar
 *   4. sweep   - white bar sweeping over black, the easiest place to see tearing
 *
 * Controls (UART 115200 on the debug probe, or the on-board buttons):
 *   n / GP21  next scene        p  previous scene
 *   f / GP26  frame cap 60 -> 30 -> uncapped
 *   s         print stats now
 *
 * Every 2 s a stats line is printed: fps, frame time, render CPU time and
 * pixels pushed per frame.
 */

#include <math.h>
#include <stdio.h>

#include "fonts.h"
#include "gfx.h"
#include "gfx_render.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "ili9341.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#define SCR_W 320
#define SCR_H 240

#define TEST_SPI_HZ 75000000u

#define BTN_NEXT 21u /* on-board upper button, 10k pull-down fitted */
#define BTN_CAP 26u  /* on-board lower button, 10k pull-down fitted */
#define BTN_DEBOUNCE_US 30000u

#define REPORT_INTERVAL_US 2000000u
#define HUD_INTERVAL_US 250000u

#define HUD_RECT ((gfx_rect_t){0, 0, 200, 20})

typedef enum { SCENE_SCROLL = 0, SCENE_BALLS, SCENE_LIST, SCENE_SWEEP, SCENE_COUNT } scene_t;

static const char *const k_scene_names[SCENE_COUNT] = {"scroll", "balls", "list", "sweep"};

static const uint32_t k_caps[] = {60u, 30u, 0u}; /* 0 = uncapped */

#define BALL_COUNT 4
#define BALL_R 18

typedef struct {
    float x, y;   /* centre */
    float vx, vy; /* px/s */
    gfx_color_t col;
} ball_t;

typedef struct {
    scene_t scene;
    float t;            /* seconds since the scene started */
    char hud[48];
    ball_t balls[BALL_COUNT];
    gfx_rect_t prev_rects[BALL_COUNT + 1];
    uint8_t prev_count;
} motion_state_t;

static motion_state_t s;

/* ------------------------------------------------------------------------- */
/* Scene drawing                                                             */
/* ------------------------------------------------------------------------- */

static int wrap(int v, int m) {
    v %= m;
    return v < 0 ? v + m : v;
}

static void draw_scroll(gfx_canvas_t *c) {
    static const gfx_color_t bars[8] = {
        GFX_RGB(255, 255, 255), GFX_RGB(255, 220, 0), GFX_RGB(0, 220, 255), GFX_RGB(0, 200, 60),
        GFX_RGB(230, 0, 200),   GFX_RGB(230, 30, 30), GFX_RGB(30, 60, 230), GFX_RGB(20, 20, 20),
    };
    const int bar_w = SCR_W / 8;
    const int off = (int)(s.t * 160.0f); /* 160 px/s to the right */

    for (int i = 0; i < 8; i++) {
        int x = wrap(i * bar_w + off, SCR_W);
        gfx_fill_rect(c, x, 0, bar_w, 168, bars[i]);
        if (x + bar_w > SCR_W) {
            gfx_fill_rect(c, x - SCR_W, 0, bar_w, 168, bars[i]);
        }
    }

    /* Fine 16 px checkerboard scrolling the other way: shows judder. */
    const int coff = wrap(-(int)(s.t * 96.0f), 32);
    gfx_fill_rect(c, 0, 168, SCR_W, 72, GFX_RGB(0, 0, 0));
    for (int y = 168; y < SCR_H; y += 16) {
        int row = (y - 168) / 16;
        for (int x = -32 + coff; x < SCR_W; x += 32) {
            int sx = x + ((row & 1) ? 16 : 0);
            gfx_fill_rect(c, sx, y, 16, 16, GFX_RGB(235, 235, 235));
        }
    }
}

static void draw_balls_bg(gfx_canvas_t *c) {
    gfx_fill_gradient_v(c, 0, 0, SCR_W, SCR_H, GFX_RGB(20, 30, 70), GFX_RGB(70, 20, 60));
    /* Static grid lines so motion is judged against fixed references. */
    for (int x = 0; x < SCR_W; x += 40) {
        gfx_vline(c, x, 0, SCR_H, GFX_RGB(60, 70, 110));
    }
    for (int y = 0; y < SCR_H; y += 40) {
        gfx_hline(c, 0, y, SCR_W, GFX_RGB(60, 70, 110));
    }
}

static void draw_balls(gfx_canvas_t *c) {
    draw_balls_bg(c);
    for (int i = 0; i < BALL_COUNT; i++) {
        gfx_fill_circle(c, (int)lroundf(s.balls[i].x), (int)lroundf(s.balls[i].y), BALL_R, s.balls[i].col);
    }
}

#define LIST_TOP 22
#define LIST_ROW_H 27

static void draw_list(gfx_canvas_t *c) {
    /* Fixed status bar. */
    gfx_fill_gradient_v(c, 0, 0, SCR_W, LIST_TOP - 1, GFX_RGB(250, 250, 252), GFX_RGB(214, 216, 222));
    gfx_hline(c, 0, LIST_TOP - 1, SCR_W, GFX_RGB(160, 162, 170));
    const char *title = "Songs";
    gfx_draw_text(c, &font_inter_14, (SCR_W - gfx_text_width(&font_inter_14, title)) / 2, 15, title,
                  GFX_RGB(20, 20, 24));

    /* Rows scroll up at 90 px/s; only rows touching this band are drawn. */
    const int scroll = (int)(s.t * 90.0f);
    const int band_top = c->area.y < LIST_TOP ? LIST_TOP : c->area.y;
    const int band_bot = c->area.y + c->area.h;
    if (band_bot <= LIST_TOP) {
        return;
    }
    int first = (band_top - LIST_TOP + scroll) / LIST_ROW_H;
    for (int row = first;; row++) {
        int y = LIST_TOP + row * LIST_ROW_H - scroll;
        if (y >= band_bot) {
            break;
        }
        bool hl = (row % 7) == 3;
        if (hl) {
            gfx_fill_gradient_v(c, 0, y, SCR_W, LIST_ROW_H, GFX_RGB(74, 144, 226), GFX_RGB(32, 98, 196));
        } else {
            gfx_fill_rect(c, 0, y, SCR_W, LIST_ROW_H, GFX_RGB(255, 255, 255));
        }
        char label[32];
        snprintf(label, sizeof label, "Track %03d", row % 1000);
        gfx_draw_text(c, &font_inter_16sb, 12, y + 19, label, hl ? GFX_RGB(255, 255, 255) : GFX_RGB(20, 20, 24));
        const char *sub = "Aurelia Vane";
        gfx_draw_text(c, &font_inter_12, 300 - gfx_text_width(&font_inter_12, sub), y + 18, sub,
                      hl ? GFX_RGB(225, 235, 255) : GFX_RGB(110, 110, 118));
    }
}

#define SWEEP_W 12

static int sweep_x(void) {
    /* 400 px/s, bouncing between the edges. */
    int span = SCR_W - SWEEP_W;
    int p = wrap((int)(s.t * 400.0f), span * 2);
    return p < span ? p : span * 2 - p;
}

static void draw_sweep(gfx_canvas_t *c) {
    gfx_fill(c, GFX_RGB(0, 0, 0));
    gfx_fill_rect(c, sweep_x(), 0, SWEEP_W, SCR_H, GFX_RGB(255, 255, 255));
}

static void draw_hud(gfx_canvas_t *c) {
    gfx_rect_t r = HUD_RECT;
    if (gfx_rect_empty(gfx_rect_intersect(r, c->area))) {
        return;
    }
    gfx_fill_rect_alpha(c, r.x, r.y, r.w, r.h, GFX_RGB(0, 0, 0), 170);
    gfx_draw_text(c, &font_inter_12, 6, 14, s.hud, GFX_RGB(255, 255, 255));
}

static void scene_draw(gfx_canvas_t *c, void *user) {
    (void)user;
    switch (s.scene) {
        case SCENE_SCROLL:
            draw_scroll(c);
            break;
        case SCENE_BALLS:
            draw_balls(c);
            break;
        case SCENE_LIST:
            draw_list(c);
            break;
        case SCENE_SWEEP:
            draw_sweep(c);
            break;
        default:
            break;
    }
    draw_hud(c);
}

/* ------------------------------------------------------------------------- */
/* Scene update: move things and mark what changed                           */
/* ------------------------------------------------------------------------- */

static gfx_rect_t ball_rect(const ball_t *b) {
    /* +2 px margin for the anti-aliased edge and rounding. */
    return (gfx_rect_t){(int16_t)(lroundf(b->x) - BALL_R - 2), (int16_t)(lroundf(b->y) - BALL_R - 2),
                        (int16_t)(BALL_R * 2 + 5), (int16_t)(BALL_R * 2 + 5)};
}

static void reset_balls(void) {
    static const gfx_color_t cols[BALL_COUNT] = {
        GFX_RGB(255, 90, 80), GFX_RGB(90, 220, 120), GFX_RGB(90, 170, 255), GFX_RGB(255, 210, 70),
    };
    static const float vel[BALL_COUNT][2] = {{180, 130}, {-150, 170}, {220, -90}, {-120, -200}};
    for (int i = 0; i < BALL_COUNT; i++) {
        s.balls[i].x = 60.0f + 60.0f * (float)i;
        s.balls[i].y = 60.0f + 30.0f * (float)i;
        s.balls[i].vx = vel[i][0];
        s.balls[i].vy = vel[i][1];
        s.balls[i].col = cols[i];
    }
}

static void update_balls(float dt) {
    for (int i = 0; i < BALL_COUNT; i++) {
        ball_t *b = &s.balls[i];
        gfx_invalidate(ball_rect(b)); /* where it was */
        b->x += b->vx * dt;
        b->y += b->vy * dt;
        if (b->x < BALL_R) {
            b->x = BALL_R;
            b->vx = -b->vx;
        } else if (b->x > SCR_W - BALL_R) {
            b->x = SCR_W - BALL_R;
            b->vx = -b->vx;
        }
        if (b->y < BALL_R) {
            b->y = BALL_R;
            b->vy = -b->vy;
        } else if (b->y > SCR_H - BALL_R) {
            b->y = SCR_H - BALL_R;
            b->vy = -b->vy;
        }
        gfx_invalidate(ball_rect(b)); /* where it is now */
    }
}

static void start_scene(scene_t scene) {
    s.scene = scene;
    s.t = 0.0f;
    if (scene == SCENE_BALLS) {
        reset_balls();
    }
    gfx_invalidate_all();
    printf("INFO  scene %d/%d: %s\n", (int)scene + 1, (int)SCENE_COUNT, k_scene_names[scene]);
}

static void update_scene(float dt) {
    int old_sweep = sweep_x();
    s.t += dt;
    switch (s.scene) {
        case SCENE_SCROLL:
            gfx_invalidate_all();
            break;
        case SCENE_BALLS:
            update_balls(dt);
            break;
        case SCENE_LIST:
            gfx_invalidate((gfx_rect_t){0, LIST_TOP, SCR_W, SCR_H - LIST_TOP});
            break;
        case SCENE_SWEEP: {
            int nx = sweep_x();
            int x0 = old_sweep < nx ? old_sweep : nx;
            int x1 = (old_sweep > nx ? old_sweep : nx) + SWEEP_W;
            gfx_invalidate((gfx_rect_t){(int16_t)x0, 0, (int16_t)(x1 - x0), SCR_H});
            break;
        }
        default:
            break;
    }
}

/* ------------------------------------------------------------------------- */
/* Input                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint pin;
    bool stable;
    bool last_raw;
    uint64_t changed_us;
} button_t;

static button_t g_btn[2] = {{BTN_NEXT, false, false, 0}, {BTN_CAP, false, false, 0}};

static void buttons_init(void) {
    for (int i = 0; i < 2; i++) {
        gpio_init(g_btn[i].pin);
        gpio_set_dir(g_btn[i].pin, GPIO_IN);
        gpio_disable_pulls(g_btn[i].pin); /* external pull-downs, RP2350-E9 */
    }
}

/* Returns true on a debounced press (rising edge). */
static bool button_pressed(button_t *b, uint64_t now) {
    bool raw = gpio_get(b->pin);
    if (raw != b->last_raw) {
        b->last_raw = raw;
        b->changed_us = now;
    }
    if (raw != b->stable && now - b->changed_us >= BTN_DEBOUNCE_US) {
        b->stable = raw;
        return raw;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* Main loop                                                                 */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint32_t frames;
    uint32_t over_budget;
    uint64_t frame_us_sum;
    uint32_t frame_us_max;
    uint64_t render_us_sum;
    uint64_t pixels;
    uint64_t start_us;
} window_stats_t;

static void print_stats(window_stats_t *w, uint32_t cap, uint64_t now) {
    uint64_t elapsed = now - w->start_us;
    if (w->frames == 0u || elapsed == 0u) {
        return;
    }
    uint32_t fps10 = (uint32_t)((uint64_t)w->frames * 10000000ull / elapsed);
    printf("STAT  %-6s cap=%-3s fps=%lu.%lu frame avg=%lu us max=%lu us render avg=%lu us "
           "px/frame=%lu over_budget=%lu\n",
           k_scene_names[s.scene], cap ? (cap == 60u ? "60" : "30") : "off", (unsigned long)(fps10 / 10u),
           (unsigned long)(fps10 % 10u), (unsigned long)(w->frame_us_sum / w->frames),
           (unsigned long)w->frame_us_max, (unsigned long)(w->render_us_sum / w->frames),
           (unsigned long)(w->pixels / w->frames), (unsigned long)w->over_budget);
}

int main(void) {
    stdio_init_all();
    sleep_ms(250);

    printf("\n=== PiPod motion_test (ILI9341 @ 75 MHz) ===\n");
    printf("INFO  clk_sys=%lu Hz clk_peri=%lu Hz\n", (unsigned long)clock_get_hz(clk_sys),
           (unsigned long)clock_get_hz(clk_peri));

    ili9341_config_t cfg;
    ili9341_default_config(&cfg);
    ili9341_init(&cfg);
    ili9341_set_pin_drive(true);
    uint32_t spi_hz = ili9341_set_spi_clock(TEST_SPI_HZ);
    printf("INFO  spi requested=%lu Hz actual=%lu Hz, fast pin drive on\n", (unsigned long)TEST_SPI_HZ,
           (unsigned long)spi_hz);
    printf("INFO  full frame at this clock: %lu us (max ~%lu fps)\n",
           (unsigned long)((uint64_t)SCR_W * SCR_H * 16u * 1000000u / spi_hz),
           (unsigned long)(spi_hz / ((uint32_t)SCR_W * SCR_H * 16u)));
    printf("INFO  keys: n/p scene, f frame cap (60/30/off), s stats. buttons: GP21 next, GP26 cap\n");
    printf("INFO  look for: tearing (a horizontal break in moving edges), stutter, sparkles\n");

    buttons_init();
    gfx_render_init();

    uint32_t cap_idx = 0;
    start_scene(SCENE_SCROLL);

    window_stats_t w = {0};
    w.start_us = time_us_64();
    uint64_t last_frame_us = time_us_64();
    uint64_t hud_us = 0;
    absolute_time_t next_frame = get_absolute_time();

    for (;;) {
        uint64_t frame_start = time_us_64();
        uint32_t cap = k_caps[cap_idx];

        /* Input. */
        int ch = getchar_timeout_us(0);
        bool next = ch == 'n' || button_pressed(&g_btn[0], frame_start);
        bool cycle_cap = ch == 'f' || button_pressed(&g_btn[1], frame_start);
        if (next) {
            start_scene((scene_t)((s.scene + 1) % SCENE_COUNT));
        } else if (ch == 'p') {
            start_scene((scene_t)((s.scene + SCENE_COUNT - 1) % SCENE_COUNT));
        }
        if (cycle_cap) {
            cap_idx = (cap_idx + 1u) % count_of(k_caps);
            cap = k_caps[cap_idx];
            printf("INFO  frame cap: %s\n", cap ? (cap == 60u ? "60 fps" : "30 fps") : "off");
            next_frame = get_absolute_time();
        }
        if (next || ch == 'p' || cycle_cap || ch == 's') {
            print_stats(&w, cap, frame_start);
            w = (window_stats_t){0};
            w.start_us = frame_start;
        }

        /* Move things. */
        float dt = (float)(frame_start - last_frame_us) * 1e-6f;
        if (dt > 0.1f) {
            dt = 0.1f;
        }
        last_frame_us = frame_start;
        update_scene(dt);

        /* HUD text, refreshed a few times per second. */
        if (frame_start - hud_us >= HUD_INTERVAL_US) {
            uint64_t el = frame_start - w.start_us;
            uint32_t fps10 = el ? (uint32_t)((uint64_t)w.frames * 10000000ull / el) : 0u;
            snprintf(s.hud, sizeof s.hud, "%s  %lu.%lu fps  cap %s  75MHz", k_scene_names[s.scene],
                     (unsigned long)(fps10 / 10u), (unsigned long)(fps10 % 10u),
                     cap ? (cap == 60u ? "60" : "30") : "off");
            gfx_invalidate(HUD_RECT);
            hud_us = frame_start;
        }

        /* Render and wait for the last band so frame time includes the DMA. */
        gfx_render_stats_t g0, g1;
        gfx_render_get_stats(&g0);
        gfx_render(scene_draw, NULL);
        ili9341_wait_idle();
        gfx_render_get_stats(&g1);

        uint64_t done = time_us_64();
        uint32_t frame_us = (uint32_t)(done - frame_start);
        w.frames++;
        w.frame_us_sum += frame_us;
        if (frame_us > w.frame_us_max) {
            w.frame_us_max = frame_us;
        }
        w.render_us_sum += g1.last_render_us;
        w.pixels += g1.pixels - g0.pixels;
        if (cap && frame_us > 1000000u / cap) {
            w.over_budget++;
        }

        if (done - w.start_us >= REPORT_INTERVAL_US) {
            print_stats(&w, cap, done);
            w = (window_stats_t){0};
            w.start_us = time_us_64();
        }

        if (cap) {
            next_frame = delayed_by_us(next_frame, 1000000u / cap);
            if (time_reached(next_frame)) {
                next_frame = get_absolute_time(); /* fell behind: re-phase */
            }
            sleep_until(next_frame);
        }
    }
}
