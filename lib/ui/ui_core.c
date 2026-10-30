/**
 * @file ui_core.c
 * @brief UI lifecycle, frame loop, input derivation, screen stack, perf.
 *
 * ui_task() is one frame: apply the cross-core snapshot, drain and derive input
 * (click / long-press / hold-repeat), step time-based animations, then render
 * only the dirty rectangles through gfx_render(). The scene callback paints the
 * status bar and the current screen; during a push/pop it paints both screens
 * with a horizontal offset so the renderer's strip clip does the rest.
 *
 * The navigation stack is at most 8 levels deep and each level keeps its own
 * selection and scroll state, so popping back restores the previous view.
 */

#include "ui_internal.h"

#include <string.h>

#include "gfx_render.h"
#include "ili9341.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include "ui_anim.h"
#include "ui_widgets.h"

#define UI_FPS_DEFAULT 30u
#define UI_LONG_PRESS_DEFAULT_MS 600u
#define UI_TRANSITION_S 0.220f

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct {
    bool down;        /**< Button is currently held. */
    bool long_fired;  /**< Long press already emitted for this hold. */
    uint32_t down_us; /**< Time of the press. */
    uint32_t repeat_us;/**< Next hold-repeat deadline (NEXT/PREV). */
    int32_t seek_base;/**< Position the hold-repeat seeks from. */
} ui_btn_state_t;

static struct {
    const ui_library_t *lib;
    uint8_t fps;
    uint16_t long_press_ms;
    bool ready;
} s_cfg;

static ui_screen_t s_stack[UI_MAX_DEPTH];
static uint8_t s_depth;

static struct {
    bool active;
    uint8_t dir; /**< 1 = push, 0 = pop. */
    float t;     /**< 0..1 progress. */
    ui_screen_t from;
} s_tr;

static ui_btn_state_t s_btn[UI_BTN_COUNT];

static ui_np_t s_np;
static ui_np_t s_prev;

static uint32_t s_last_frame_us;
static bool s_last_frame_valid;
static float s_dt;

static uint32_t s_last_tick_us;
static bool s_tick_valid;

static struct {
    uint32_t frames;
    uint32_t idle;
    uint64_t busy_us;
    uint64_t render_busy_us;
    uint64_t wall_us;
    uint64_t pixels;
    uint64_t dma_us;
    uint32_t max_frame_us;
} s_perf;

/* 1 Hz status overlay. */
static uint32_t s_perf_tick_us;
static uint32_t s_fps_frames;
static uint64_t s_cpu_busy_us;
static uint16_t s_fps_show;
static uint8_t s_cpu_show;
static bool s_perf_was_on;

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static gfx_rect_t content_rect(void) {
    gfx_rect_t r = {0, UI_CONTENT_Y, UI_SCREEN_W, UI_CONTENT_H};
    return r;
}

const ui_library_t *ui_library(void) {
    return s_cfg.lib;
}

uint16_t ui_long_press_ms(void) {
    return s_cfg.long_press_ms;
}

void ui_invalidate(gfx_rect_t r) {
    gfx_invalidate(r);
}

static void transition_finish(void) {
    if (!s_tr.active) {
        return;
    }
    s_tr.active = false;
    s_tr.t = 1.0f;
    ui_invalidate(content_rect());
}

/* ------------------------------------------------------------------------- */
/* Cross-core snapshot                                                       */
/* ------------------------------------------------------------------------- */

static void apply_snapshot(void) {
    ui_shared_snapshot(&s_np);
    if (s_np.seq == s_prev.seq && s_np.track_seq == s_prev.track_seq) {
        return;
    }

    if (s_np.track_seq != s_prev.track_seq) {
        ui_nowplaying_reset();
        ui_invalidate(content_rect());
    }
    if (s_np.has_track != s_prev.has_track) {
        ui_invalidate(content_rect());
    }
    if (s_np.play != s_prev.play) {
        ui_invalidate(ui_rect_play_glyph());
    }
    if (s_np.battery != s_prev.battery || s_np.charging != s_prev.charging) {
        ui_invalidate(ui_rect_battery());
    }
    if (s_np.shuffle != s_prev.shuffle || s_np.repeat != s_prev.repeat) {
        ui_settings_sync(&s_np);
        ui_invalidate(ui_rect_mode_icons());
    }
    s_prev = s_np;
}

/* ------------------------------------------------------------------------- */
/* Input                                                                     */
/* ------------------------------------------------------------------------- */

static void on_click(ui_button_t button, uint32_t now);
static void on_long(ui_button_t button, uint32_t now);

static void seek_step(ui_button_t button) {
    ui_btn_state_t *st = &s_btn[button];
    const int32_t delta = (button == UI_BTN_NEXT) ? 3000 : -3000;
    int32_t next = st->seek_base + delta;
    if (next < 0) {
        next = 0;
    }
    if (s_np.track.duration_ms > 0u && (uint32_t)next > s_np.track.duration_ms) {
        next = (int32_t)s_np.track.duration_ms;
    }
    st->seek_base = next;
    ui_shared_post_simple(UI_CMD_SEEK, next);
}

static void on_down(ui_button_t button, uint32_t now) {
    if (button >= UI_BTN_COUNT) {
        return;
    }
    ui_btn_state_t *st = &s_btn[button];
    st->down = true;
    st->long_fired = false;
    st->down_us = now;
}

static void on_up(ui_button_t button, uint32_t now) {
    if (button >= UI_BTN_COUNT) {
        return;
    }
    ui_btn_state_t *st = &s_btn[button];
    if (!st->down) {
        return;
    }
    st->down = false;
    const uint32_t held = now - st->down_us;
    if (!st->long_fired) {
        if (held >= (uint32_t)ui_long_press_ms() * 1000u) {
            on_long(button, now);
        } else {
            on_click(button, now);
        }
    }
    st->long_fired = false;
}

static void on_long(ui_button_t button, uint32_t now) {
    switch (button) {
    case UI_BTN_MENU:
        ui_nav_home();
        break;
    case UI_BTN_NEXT:
    case UI_BTN_PREV: {
        ui_btn_state_t *st = &s_btn[button];
        st->seek_base = (int32_t)s_np.position_ms;
        st->repeat_us = now + 200000u;
        seek_step(button);
        break;
    }
    default:
        break;
    }
}

static void on_click(ui_button_t button, uint32_t now) {
    if (s_tr.active && (button == UI_BTN_SELECT || button == UI_BTN_MENU)) {
        return;
    }
    ui_screen_t *top = &s_stack[s_depth - 1u];
    switch (button) {
    case UI_BTN_SELECT:
        if (top->kind == UI_SCR_NOW_PLAYING) {
            ui_nowplaying_click(&s_np, now);
        } else {
            ui_menu_activate(&s_np, top);
        }
        break;
    case UI_BTN_MENU:
        ui_nav_back();
        break;
    case UI_BTN_PLAY_PAUSE:
        ui_shared_post_simple(UI_CMD_TOGGLE_PAUSE, 0);
        break;
    case UI_BTN_NEXT:
        ui_shared_post_simple(UI_CMD_NEXT, 0);
        break;
    case UI_BTN_PREV:
        ui_shared_post_simple(UI_CMD_PREV, 0);
        break;
    default:
        break;
    }
}

static void process_holds(uint32_t now) {
    const uint32_t hold_us = (uint32_t)ui_long_press_ms() * 1000u;
    for (int b = 0; b < UI_BTN_COUNT; b++) {
        ui_btn_state_t *st = &s_btn[b];
        if (!st->down) {
            continue;
        }
        if (!st->long_fired) {
            if ((uint32_t)(now - st->down_us) >= hold_us) {
                st->long_fired = true;
                on_long((ui_button_t)b, now);
            }
        } else if (b == UI_BTN_NEXT || b == UI_BTN_PREV) {
            if ((int32_t)(now - st->repeat_us) >= 0) {
                seek_step((ui_button_t)b);
                st->repeat_us += 200000u;
            }
        }
    }
}

static void on_wheel(int16_t delta, uint32_t now) {
    if (delta == 0 || s_tr.active) {
        return;
    }
    ui_screen_t *top = &s_stack[s_depth - 1u];
    const uint32_t tick_dt = now - s_last_tick_us;
    int step = 1;
    if (s_tick_valid) {
        const uint32_t count = ui_menu_count(&s_np, top);
        if (count >= 50u) {
            if (tick_dt < 20000u) {
                step = 4;
            } else if (tick_dt < 40000u) {
                step = 2;
            }
        }
    }
    s_last_tick_us = now;
    s_tick_valid = true;

    switch (top->kind) {
    case UI_SCR_NOW_PLAYING:
        ui_nowplaying_wheel(&s_np, delta, now);
        break;
    case UI_SCR_MAIN:
    case UI_SCR_MUSIC:
    case UI_SCR_LIST:
    case UI_SCR_SETTINGS:
        ui_menu_wheel(&s_np, top, (int)delta, step);
        break;
    default:
        break;
    }
}

static void drain_input(uint32_t now) {
    ui_input_event_t ev;
    while (ui_shared_pop_input(&ev)) {
        switch (ev.type) {
        case UI_INPUT_WHEEL:
            on_wheel(ev.delta, now);
            break;
        case UI_INPUT_BUTTON_DOWN:
            on_down((ui_button_t)ev.button, now);
            break;
        case UI_INPUT_BUTTON_UP:
            on_up((ui_button_t)ev.button, now);
            break;
        case UI_EV_NAV_HOME:
            ui_nav_home();
            break;
        case UI_EV_NAV_BACK:
            ui_nav_back();
            break;
        case UI_EV_NAV_NOW_PLAYING:
            ui_nav_push_now_playing();
            break;
        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Navigation                                                                */
/* ------------------------------------------------------------------------- */

ui_screen_t *ui_nav_push(ui_screen_kind_t kind) {
    if (s_depth >= UI_MAX_DEPTH) {
        return NULL;
    }
    transition_finish();
    s_tr.from = s_stack[s_depth - 1u];
    s_tr.dir = 1;
    s_tr.t = 0.0f;
    s_tr.active = true;

    ui_screen_t *s = &s_stack[s_depth++];
    memset(s, 0, sizeof *s);
    s->kind = (uint8_t)kind;
    s->parent_id = UI_ID_ALL;
    ui_menu_screen_reset(s);

    ui_invalidate(content_rect());
    ui_invalidate(ui_rect_title());
    return s;
}

void ui_nav_push_list(ui_list_kind_t list, uint32_t parent_id) {
    ui_screen_t *s = ui_nav_push(UI_SCR_LIST);
    if (s != NULL) {
        ui_menu_screen_init(s, UI_SCR_LIST, list, parent_id);
    }
}

void ui_nav_push_now_playing(void) {
    if (s_depth > 0u && s_stack[s_depth - 1u].kind == UI_SCR_NOW_PLAYING) {
        return;
    }
    ui_screen_t *s = ui_nav_push(UI_SCR_NOW_PLAYING);
    if (s != NULL) {
        ui_nowplaying_reset();
    }
}

void ui_nav_back(void) {
    if (s_depth <= 1u) {
        return;
    }
    transition_finish();
    s_tr.from = s_stack[s_depth - 1u];
    s_tr.dir = 0;
    s_tr.t = 0.0f;
    s_tr.active = true;
    s_depth--;

    ui_invalidate(content_rect());
    ui_invalidate(ui_rect_title());
}

void ui_nav_home(void) {
    if (s_depth <= 1u) {
        return;
    }
    transition_finish();
    s_tr.from = s_stack[s_depth - 1u];
    s_tr.dir = 0;
    s_tr.t = 0.0f;
    s_tr.active = true;
    s_depth = 1;

    ui_invalidate(content_rect());
    ui_invalidate(ui_rect_title());
}

/* ------------------------------------------------------------------------- */
/* Animation and rendering                                                   */
/* ------------------------------------------------------------------------- */

static void step_animations(uint32_t now) {
    ui_screen_t *top = &s_stack[s_depth - 1u];
    switch (top->kind) {
    case UI_SCR_NOW_PLAYING:
        ui_nowplaying_step(&s_np, top, s_dt, now);
        break;
    case UI_SCR_ABOUT:
        break;
    default:
        ui_menu_step(&s_np, top, s_dt);
        break;
    }

    if (s_tr.active) {
        const bool before = s_tr.t < 0.5f;
        s_tr.t += s_dt / UI_TRANSITION_S;
        if (s_tr.t >= 1.0f) {
            s_tr.t = 1.0f;
            s_tr.active = false;
        }
        if ((s_tr.t < 0.5f) != before) {
            ui_invalidate(ui_rect_title());
        }
        ui_invalidate(content_rect());
    }
}

static void screen_draw(gfx_canvas_t *c, const ui_screen_t *s, int x_off) {
    switch (s->kind) {
    case UI_SCR_NOW_PLAYING:
        ui_nowplaying_draw(c, &s_np, s, x_off);
        break;
    case UI_SCR_ABOUT:
        ui_settings_about_draw(c, x_off);
        break;
    default:
        ui_menu_draw(c, &s_np, s, x_off);
        break;
    }
}

static void draw_content(gfx_canvas_t *c) {
    const ui_screen_t *top = &s_stack[s_depth - 1u];
    if (!s_tr.active) {
        screen_draw(c, top, 0);
        return;
    }
    const int span = UI_SCREEN_W;
    const int off = (int)(ui_ease_out_cubic(s_tr.t) * (float)span + 0.5f);
    int off_from;
    int off_to;
    if (s_tr.dir != 0u) {
        off_from = -off;
        off_to = span - off;
    } else {
        off_from = off;
        off_to = -span + off;
    }
    screen_draw(c, &s_tr.from, off_from);
    screen_draw(c, top, off_to);
}

static const char *current_title(void) {
    if (s_tr.active && s_tr.t < 0.5f) {
        return ui_screen_title(&s_np, &s_tr.from);
    }
    return ui_screen_title(&s_np, &s_stack[s_depth - 1u]);
}

static void build_status(ui_status_t *st) {
    memset(st, 0, sizeof *st);
    st->title = current_title();
    st->play = s_np.play;
    st->has_track = s_np.has_track;
    st->battery = s_np.battery;
    st->charging = s_np.charging;
    st->shuffle = s_np.shuffle;
    st->repeat = s_np.repeat;
    st->perf = ui_settings_perf_overlay();
    st->fps = s_fps_show;
    st->cpu = s_cpu_show;
}

/**
 * @brief Scene callback handed to gfx_render(): paints one strip band.
 * @param c Band canvas (area == clip == the band being rendered).
 * @param user Unused.
 */
static void scene_draw(gfx_canvas_t *c, void *user) {
    (void)user;
    const gfx_rect_t saved = c->clip;

    /* Decide what to draw from the band alone. Testing ui_visible() after the
     * clip has been narrowed to the status bar would hide the content whenever
     * a 16-row band straddles y = 22 (status overlap only, content starts at
     * the band edge): the top rows of a menu would keep stale pixels, e.g. the
     * first row at boot or the first frame after a push. */
    const gfx_rect_t band = ui_band(c);
    const gfx_rect_t status = gfx_rect_intersect(band, ui_rect_status());
    const gfx_rect_t content = gfx_rect_intersect(band, content_rect());

    if (!gfx_rect_empty(status)) {
        ui_status_t st;
        build_status(&st);
        gfx_set_clip(c, gfx_rect_intersect(saved, ui_rect_status()));
        ui_draw_status_bar(c, &st);
    }

    if (!gfx_rect_empty(content)) {
        gfx_set_clip(c, gfx_rect_intersect(saved, content_rect()));
        draw_content(c);
    }

    gfx_set_clip(c, saved);
}

static void update_perf_overlay(uint32_t now) {
    if (!ui_settings_perf_overlay()) {
        if (s_perf_was_on) {
            s_perf_was_on = false;
            ui_invalidate(ui_rect_perf());
        }
        return;
    }
    s_perf_was_on = true;

    const uint32_t dt = now - s_perf_tick_us;
    if (dt < 1000000u) {
        return;
    }
    s_perf_tick_us = now;

    const uint16_t fps = (uint16_t)(((uint64_t)s_fps_frames * 1000000ull) / (uint64_t)dt);
    uint64_t wall = s_cpu_busy_us;
    uint8_t cpu = (wall > 0u) ? (uint8_t)((wall * 100ull) / (uint64_t)dt) : 0u;
    if (cpu > 100u) {
        cpu = 100u;
    }
    s_fps_frames = 0;
    s_cpu_busy_us = 0;

    if (fps != s_fps_show || cpu != s_cpu_show) {
        s_fps_show = fps;
        s_cpu_show = cpu;
        ui_invalidate(ui_rect_perf());
    }
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

bool ui_post_input(const ui_input_event_t *ev) {
    if (ev == NULL || ev->type > (uint8_t)UI_INPUT_BUTTON_UP) {
        return false;
    }
    return ui_shared_post_event(ev);
}

bool ui_post_wheel(int16_t ticks) {
    ui_input_event_t ev;
    ev.type = (uint8_t)UI_INPUT_WHEEL;
    ev.button = 0;
    ev.delta = ticks;
    return ui_post_input(&ev);
}

bool ui_post_button(ui_button_t button, bool down) {
    if (button >= UI_BTN_COUNT) {
        return false;
    }
    ui_input_event_t ev;
    ev.type = (uint8_t)(down ? UI_INPUT_BUTTON_DOWN : UI_INPUT_BUTTON_UP);
    ev.button = (uint8_t)button;
    ev.delta = 0;
    return ui_post_input(&ev);
}

static void post_nav(uint8_t type) {
    ui_input_event_t ev;
    ev.type = type;
    ev.button = 0;
    ev.delta = 0;
    (void)ui_shared_post_event(&ev);
}

void ui_go_home(void) {
    post_nav((uint8_t)UI_EV_NAV_HOME);
}

void ui_go_back(void) {
    post_nav((uint8_t)UI_EV_NAV_BACK);
}

void ui_show_now_playing(void) {
    post_nav((uint8_t)UI_EV_NAV_NOW_PLAYING);
}

void ui_init(const ui_config_t *cfg) {
    memset(&s_cfg, 0, sizeof s_cfg);
    s_cfg.fps = UI_FPS_DEFAULT;
    s_cfg.long_press_ms = UI_LONG_PRESS_DEFAULT_MS;
    uint8_t volume = 50u;
    if (cfg != NULL) {
        s_cfg.lib = cfg->library;
        if (cfg->target_fps != 0u) {
            s_cfg.fps = cfg->target_fps;
        }
        if (cfg->long_press_ms != 0u) {
            s_cfg.long_press_ms = cfg->long_press_ms;
        }
        volume = cfg->initial_volume;
    }
    if (s_cfg.fps < 1u) {
        s_cfg.fps = 1u;
    }
    if (s_cfg.fps > 100u) {
        s_cfg.fps = 100u;
    }
    if (volume > 100u) {
        volume = 100u;
    }

    gfx_render_init();
    ui_shared_init(volume);
    ui_settings_init();
    ui_nowplaying_reset();

    memset(s_btn, 0, sizeof s_btn);
    memset(&s_tr, 0, sizeof s_tr);
    memset(s_stack, 0, sizeof s_stack);
    s_depth = 1;
    s_stack[0].kind = (uint8_t)UI_SCR_MAIN;
    s_stack[0].parent_id = UI_ID_ALL;
    ui_menu_screen_reset(&s_stack[0]);

    memset(&s_perf, 0, sizeof s_perf);
    s_fps_frames = 0;
    s_cpu_busy_us = 0;
    s_fps_show = 0;
    s_cpu_show = 0;
    s_perf_was_on = false;
    s_last_frame_valid = false;
    s_dt = 0.0f;
    s_tick_valid = false;
    s_last_tick_us = 0;
    s_perf_tick_us = time_us_32();

    s_cfg.ready = true;

    /* Paint the initial screen so the panel is usable before ui_run(). */
    gfx_invalidate_all();
    ui_task();
}

void ui_task(void) {
    if (!s_cfg.ready) {
        return;
    }
    const uint32_t now = time_us_32();

    if (s_last_frame_valid) {
        const uint32_t wall = now - s_last_frame_us;
        s_perf.wall_us += wall;
        s_dt = (float)wall * 1.0e-6f;
        if (s_dt > 0.1f) {
            s_dt = 0.1f;
        }
    } else {
        s_dt = 0.0f;
    }
    s_last_frame_us = now;
    s_last_frame_valid = true;

    const uint32_t work_start = now;

    apply_snapshot();
    drain_input(now);
    process_holds(now);
    step_animations(now);
    update_perf_overlay(now);

    bool rendered = false;
    if (gfx_has_dirty()) {
        ili9341_stats_t disp0;
        ili9341_stats_t disp1;
        gfx_render_stats_t g0;
        gfx_render_stats_t g1;
        ili9341_get_stats(&disp0);
        gfx_render_get_stats(&g0);

        gfx_render(scene_draw, NULL);
        ili9341_wait_idle();

        ili9341_get_stats(&disp1);
        gfx_render_get_stats(&g1);
        s_perf.frames++;
        s_perf.pixels += (uint64_t)(g1.pixels - g0.pixels);
        s_perf.dma_us += (uint64_t)(disp1.dma_busy_us - disp0.dma_busy_us);
        rendered = true;
    } else {
        s_perf.idle++;
    }

    const uint32_t spent = time_us_32() - work_start;
    s_perf.busy_us += spent;
    s_cpu_busy_us += spent;
    if (rendered) {
        s_perf.render_busy_us += spent;
        s_fps_frames++;
        if (spent > s_perf.max_frame_us) {
            s_perf.max_frame_us = spent;
        }
    }
}

void ui_run(void) {
    if (!s_cfg.ready) {
        for (;;) {
            sleep_ms(1000);
        }
    }
    const uint32_t period_us = 1000000u / (uint32_t)s_cfg.fps;
    for (;;) {
        const absolute_time_t start = get_absolute_time();
        ui_task();
        sleep_until(delayed_by_us(start, period_us));
    }
}

void ui_get_perf(ui_perf_t *out, bool reset) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    out->frames = s_perf.frames;
    out->idle_frames = s_perf.idle;
    out->avg_frame_us =
        (s_perf.frames > 0u) ? (uint32_t)(s_perf.render_busy_us / (uint64_t)s_perf.frames) : 0u;
    out->max_frame_us = s_perf.max_frame_us;
    out->avg_pixels =
        (s_perf.frames > 0u) ? (uint32_t)(s_perf.pixels / (uint64_t)s_perf.frames) : 0u;
    out->dma_wait_us = (s_perf.dma_us > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)s_perf.dma_us;
    out->cpu_load_pct =
        (s_perf.wall_us > 0u) ? (uint32_t)((s_perf.busy_us * 100ull) / s_perf.wall_us) : 0u;
    if (out->cpu_load_pct > 100u) {
        out->cpu_load_pct = 100u;
    }
    out->elapsed_ms = (uint32_t)(s_perf.wall_us / 1000ull);
    out->dropped_inputs = ui_shared_dropped_inputs();

    if (reset) {
        memset(&s_perf, 0, sizeof s_perf);
        ui_shared_reset_dropped_inputs();
        s_last_frame_valid = false;
        s_dt = 0.0f;
        s_fps_frames = 0;
        s_cpu_busy_us = 0;
        s_perf_tick_us = time_us_32();
    }
}
