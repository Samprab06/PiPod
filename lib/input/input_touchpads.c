/**
 * @file input_touchpads.c
 * @brief TTP223 six-pad decoder: per-pad debounce, slider gesture, button
 *        edges, and an optional event log for bring-up.
 *
 * Click versus long press is deliberately NOT decoded here: like
 * input_buttons.c this adapter only posts BUTTON_DOWN on the accepted press
 * and BUTTON_UP on release. ui.h's input contract says the UI derives clicks
 * and long presses from those edges, so both adapters behave identically.
 */

#include "input_touchpads.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "ui.h"

#ifndef INPUT_TOUCHPADS_HOST
#include "hardware/gpio.h"
#include "pico/time.h"
#endif

#define MAX_POLL_GAP_US 20000u /**< clamp dt after a long stall */
#define NO_PAD          0xFFu
#define NO_BUTTON       0xFFu

typedef struct {
    uint8_t gpio;
    bool    raw_high;       /**< level sampled by the last poll (pre inversion) */
    bool    active;         /**< debounced, logical "touched" */
    int32_t integrator_us;  /**< time the raw level has disagreed with `active` */
} pad_state_t;

static input_touchpads_config_t s_cfg;
static pad_state_t s_pads[INPUT_TOUCHPAD_COUNT];
static uint32_t    s_debounce_us;
static uint64_t    s_last_poll_us;
static bool        s_initialised;

/* Slider gesture state. */
static uint64_t s_last_down_us[3]; /**< debounced down time of each slider pad */
static bool     s_seen_down[3];    /**< s_last_down_us[] is valid */
static uint64_t s_prev_down_us;    /**< down time of the previous slider pad */
static uint8_t  s_last_pad;        /**< most recent slider pad, NO_PAD at rest */
static int8_t   s_direction;       /**< +1 CW, -1 CCW, 0 = no active slide */
static uint64_t s_repeat_at_us;    /**< next auto-repeat due, 0 = none */

/* Decoded-event log for input_touchpads_debug_print(). */
static input_touchpad_event_t s_log[INPUT_TOUCHPADS_EVENT_LOG];
static uint8_t s_log_head;
static uint8_t s_log_count;

static const uint8_t k_button_for_pad[3] = {
    (uint8_t)UI_BTN_SELECT,
    (uint8_t)UI_BTN_MENU,
    (uint8_t)UI_BTN_PLAY_PAUSE,
};

static const char *const k_pad_names[INPUT_TOUCHPAD_COUNT] = {
    "LEFT", "MIDDLE", "RIGHT", "SELECT", "MENU", "PLAY_PAUSE",
};

static const char *const k_event_names[] = {
    "PAD_DOWN", "PAD_UP", "SLIDE", "REPEAT", "BUTTON_DOWN", "BUTTON_UP",
};

#ifdef INPUT_TOUCHPADS_HOST
static bool     s_host_raw[INPUT_TOUCHPAD_COUNT];
static uint64_t s_host_now_us;

void input_touchpads_host_set_raw(input_touchpad_id_t pad, bool high) {
    if ((unsigned)pad < (unsigned)INPUT_TOUCHPAD_COUNT) {
        s_host_raw[pad] = high;
    }
}

void input_touchpads_host_advance_us(uint64_t us) {
    s_host_now_us += us;
}

uint64_t input_touchpads_host_now_us(void) {
    return s_host_now_us;
}
#endif

static uint64_t now_us(void) {
#ifdef INPUT_TOUCHPADS_HOST
    return s_host_now_us;
#else
    return time_us_64();
#endif
}

static bool read_raw_high(uint8_t pad) {
#ifdef INPUT_TOUCHPADS_HOST
    return s_host_raw[pad];
#else
    return gpio_get(s_pads[pad].gpio) != 0;
#endif
}

static void log_event(uint8_t type, uint8_t pad, uint8_t button, int16_t ticks,
                      uint64_t now) {
    input_touchpad_event_t *ev = &s_log[s_log_head];

    ev->type = type;
    ev->pad = pad;
    ev->button = button;
    ev->ticks = ticks;
    ev->t_ms = (uint32_t)(now / 1000u);

    s_log_head = (uint8_t)((s_log_head + 1u) % INPUT_TOUCHPADS_EVENT_LOG);
    if (s_log_count < INPUT_TOUCHPADS_EVENT_LOG) {
        s_log_count++;
    }
    if (s_cfg.event_cb != NULL) {
        s_cfg.event_cb(ev, s_cfg.event_user);
    }
}

/** Post one slider step (1 or 2 sub-steps) as wheel ticks and log it. */
static void post_step(uint8_t pad, int dir, int steps, bool repeat,
                      uint64_t now) {
    int32_t ticks = dir * steps * (int32_t)s_cfg.ticks_per_step;

    if (ticks > INT16_MAX) {
        ticks = INT16_MAX;
    } else if (ticks < INT16_MIN) {
        ticks = INT16_MIN;
    }
    (void)ui_post_wheel((int16_t)ticks);
    log_event(repeat ? (uint8_t)INPUT_TOUCHPAD_EV_REPEAT
                     : (uint8_t)INPUT_TOUCHPAD_EV_SLIDE,
              pad, NO_BUTTON, (int16_t)ticks, now);
}

static void slider_pad_down(uint8_t pad, uint64_t now) {
    const uint64_t window_us = (uint64_t)s_cfg.slide_window_ms * 1000u;
    int steps = 0;
    int dir = 0;

    if (s_last_pad != NO_PAD) {
        int diff = (int)pad - (int)s_last_pad;
        int adiff = diff < 0 ? -diff : diff;

        if (adiff == 1) {
            /* Adjacent activation inside the window: one step. */
            if (now - s_prev_down_us <= window_us) {
                dir = diff > 0 ? 1 : -1;
                steps = 1;
            }
        } else if (adiff == 2) {
            /* Skipped pad: two steps only if the MIDDLE pad really fired
             * inside the window, otherwise the jump is ignored. */
            uint8_t mid = (uint8_t)(((int)pad + (int)s_last_pad) / 2);

            if (s_seen_down[mid] && now - s_last_down_us[mid] <= window_us) {
                dir = diff > 0 ? 1 : -1;
                steps = 2;
            }
        }
    }

    s_last_down_us[pad] = now;
    s_seen_down[pad] = true;
    s_prev_down_us = now;
    s_last_pad = pad;

    if (steps > 0) {
        s_direction = (int8_t)dir;
        post_step(pad, dir, steps, false, now);
        s_repeat_at_us =
            now + (uint64_t)s_cfg.hold_before_repeat_ms * 1000u;
    } else {
        /* First pad of a gesture, or a jump/slow transition that does not
         * count: start over without emitting anything. */
        s_direction = 0;
        s_repeat_at_us = 0;
    }
}

static void pad_down(uint8_t pad, uint64_t now) {
    log_event((uint8_t)INPUT_TOUCHPAD_EV_PAD_DOWN, pad, NO_BUTTON, 0, now);

    if (pad < 3u) {
        slider_pad_down(pad, now);
        return;
    }

    uint8_t button = k_button_for_pad[pad - 3u];

    (void)ui_post_button((ui_button_t)button, true);
    log_event((uint8_t)INPUT_TOUCHPAD_EV_BUTTON_DOWN, pad, button, 0, now);
}

static void pad_up(uint8_t pad, uint64_t now) {
    log_event((uint8_t)INPUT_TOUCHPAD_EV_PAD_UP, pad, NO_BUTTON, 0, now);

    if (pad >= 3u) {
        uint8_t button = k_button_for_pad[pad - 3u];

        (void)ui_post_button((ui_button_t)button, false);
        log_event((uint8_t)INPUT_TOUCHPAD_EV_BUTTON_UP, pad, button, 0, now);
    }
}

void input_touchpads_default_config(input_touchpads_config_t *cfg) {
    if (cfg == NULL) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->slider_left_gpio = INPUT_TOUCHPADS_DEFAULT_GPIO_LEFT;
    cfg->slider_middle_gpio = INPUT_TOUCHPADS_DEFAULT_GPIO_MIDDLE;
    cfg->slider_right_gpio = INPUT_TOUCHPADS_DEFAULT_GPIO_RIGHT;
    cfg->button_select_gpio = INPUT_TOUCHPADS_DEFAULT_GPIO_SELECT;
    cfg->button_menu_gpio = INPUT_TOUCHPADS_DEFAULT_GPIO_MENU;
    cfg->button_play_pause_gpio = INPUT_TOUCHPADS_DEFAULT_GPIO_PLAY_PAUSE;
    cfg->active_high = true;
    cfg->internal_pulls = true;
    cfg->debounce_ms = INPUT_TOUCHPADS_DEFAULT_DEBOUNCE_MS;
    cfg->slide_window_ms = INPUT_TOUCHPADS_DEFAULT_SLIDE_WINDOW_MS;
    cfg->hold_before_repeat_ms = INPUT_TOUCHPADS_DEFAULT_HOLD_MS;
    cfg->repeat_rate_ms = INPUT_TOUCHPADS_DEFAULT_REPEAT_MS;
    cfg->ticks_per_step = INPUT_TOUCHPADS_DEFAULT_TICKS_PER_STEP;
    cfg->event_cb = NULL;
    cfg->event_user = NULL;
}

void input_touchpads_init(const input_touchpads_config_t *cfg) {
    input_touchpads_config_t defaults;

    if (cfg == NULL) {
        input_touchpads_default_config(&defaults);
        cfg = &defaults;
    }

    s_cfg = *cfg;
    if (s_cfg.debounce_ms == 0u) {
        s_cfg.debounce_ms = INPUT_TOUCHPADS_DEFAULT_DEBOUNCE_MS;
    }
    if (s_cfg.slide_window_ms == 0u) {
        s_cfg.slide_window_ms = INPUT_TOUCHPADS_DEFAULT_SLIDE_WINDOW_MS;
    }
    if (s_cfg.hold_before_repeat_ms == 0u) {
        s_cfg.hold_before_repeat_ms = INPUT_TOUCHPADS_DEFAULT_HOLD_MS;
    }
    if (s_cfg.repeat_rate_ms == 0u) {
        s_cfg.repeat_rate_ms = INPUT_TOUCHPADS_DEFAULT_REPEAT_MS;
    }
    if (s_cfg.ticks_per_step == 0u) {
        s_cfg.ticks_per_step = INPUT_TOUCHPADS_DEFAULT_TICKS_PER_STEP;
    }
    s_debounce_us = s_cfg.debounce_ms * 1000u;

    const uint8_t pins[INPUT_TOUCHPAD_COUNT] = {
        s_cfg.slider_left_gpio,    s_cfg.slider_middle_gpio,
        s_cfg.slider_right_gpio,   s_cfg.button_select_gpio,
        s_cfg.button_menu_gpio,    s_cfg.button_play_pause_gpio,
    };

    memset(s_pads, 0, sizeof(s_pads));
    for (uint8_t i = 0; i < (uint8_t)INPUT_TOUCHPAD_COUNT; i++) {
        s_pads[i].gpio = pins[i];
        if (pins[i] == INPUT_TOUCHPADS_GPIO_UNUSED) {
            continue;
        }

#ifndef INPUT_TOUCHPADS_HOST
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_set_input_enabled(pins[i], true);
        if (s_cfg.internal_pulls) {
            /* A TTP223 push-pull output drives both levels, so the pull is
             * only a defined-level fallback for an unpopulated pad. */
            if (s_cfg.active_high) {
                gpio_pull_down(pins[i]);
            } else {
                gpio_pull_up(pins[i]);
            }
        } else {
            gpio_disable_pulls(pins[i]);
        }
#endif

        /* Snapshot the level: init is documented to run with pads untouched,
         * and a pad already touched then must not post a phantom press. */
        s_pads[i].raw_high = read_raw_high(i);
        s_pads[i].active = (s_pads[i].raw_high == s_cfg.active_high);
    }

    memset(s_last_down_us, 0, sizeof(s_last_down_us));
    memset(s_seen_down, 0, sizeof(s_seen_down));
    s_prev_down_us = 0;
    s_last_pad = NO_PAD;
    s_direction = 0;
    s_repeat_at_us = 0;

    memset(s_log, 0, sizeof(s_log));
    s_log_head = 0;
    s_log_count = 0;

    s_last_poll_us = now_us();
    s_initialised = true;
}

void input_touchpads_poll(void) {
    if (!s_initialised) {
        return;
    }

    uint64_t now = now_us();
    uint64_t gap = now - s_last_poll_us;
    s_last_poll_us = now;
    if (gap > MAX_POLL_GAP_US) {
        gap = MAX_POLL_GAP_US;
    }
    int32_t dt = (gap == 0u) ? 1 : (int32_t)gap;

    for (uint8_t i = 0; i < (uint8_t)INPUT_TOUCHPAD_COUNT; i++) {
        pad_state_t *p = &s_pads[i];
        if (p->gpio == INPUT_TOUCHPADS_GPIO_UNUSED) {
            continue;
        }
        bool raw = read_raw_high(i);
        bool touched = (raw == s_cfg.active_high);

        p->raw_high = raw;

        if (touched != p->active) {
            /* Integrating debounce, same idea as input_buttons.c: the new
             * level has to survive the whole window regardless of poll
             * spacing. */
            p->integrator_us += dt;
            if (p->integrator_us >= (int32_t)s_debounce_us) {
                p->integrator_us = 0;
                p->active = touched;
                if (touched) {
                    pad_down(i, now);
                } else {
                    pad_up(i, now);
                }
            }
        } else {
            p->integrator_us -= dt;
            if (p->integrator_us < 0) {
                p->integrator_us = 0;
            }
        }
    }

    /* Releasing every slider pad ends the gesture. The last_down timestamps
     * deliberately survive so a fast LEFT->RIGHT skip can still see that the
     * MIDDLE pad fired inside the window. */
    if (!s_pads[INPUT_TOUCHPAD_LEFT].active &&
        !s_pads[INPUT_TOUCHPAD_MIDDLE].active &&
        !s_pads[INPUT_TOUCHPAD_RIGHT].active) {
        s_last_pad = NO_PAD;
        s_direction = 0;
        s_repeat_at_us = 0;
    }

    /* Holding the end pad of a slide auto-repeats in the last direction.
     * Post at most one repeat per poll so a stalled loop cannot dump a burst
     * of ticks into the UI queue. */
    if (s_direction != 0 && s_last_pad < 3u && s_repeat_at_us != 0u &&
        s_pads[s_last_pad].active && now >= s_repeat_at_us) {
        post_step(s_last_pad, s_direction, 1, true, now);
        s_repeat_at_us = now + (uint64_t)s_cfg.repeat_rate_ms * 1000u;
    }
}

static const char *button_name(uint8_t button) {
    switch (button) {
    case (uint8_t)UI_BTN_SELECT:
        return "SELECT";
    case (uint8_t)UI_BTN_MENU:
        return "MENU";
    case (uint8_t)UI_BTN_PLAY_PAUSE:
        return "PLAY_PAUSE";
    case (uint8_t)UI_BTN_PREV:
        return "PREV";
    case (uint8_t)UI_BTN_NEXT:
        return "NEXT";
    default:
        return "-";
    }
}

void input_touchpads_debug_print(void) {
    if (!s_initialised) {
        printf("[touchpads] not initialised\n");
        return;
    }

    printf("[touchpads] active_high=%u internal_pulls=%u debounce=%lums "
           "window=%lums hold=%lums repeat=%lums ticks/step=%u\n",
           s_cfg.active_high ? 1u : 0u, s_cfg.internal_pulls ? 1u : 0u,
           (unsigned long)s_cfg.debounce_ms,
           (unsigned long)s_cfg.slide_window_ms,
           (unsigned long)s_cfg.hold_before_repeat_ms,
           (unsigned long)s_cfg.repeat_rate_ms,
           (unsigned)s_cfg.ticks_per_step);

    printf("  pad         gpio  raw  deb  last_down_ms\n");
    for (uint8_t i = 0; i < (uint8_t)INPUT_TOUCHPAD_COUNT; i++) {
        const pad_state_t *p = &s_pads[i];
        int64_t down_ms = -1;

        if (i < 3u && s_seen_down[i]) {
            down_ms = (int64_t)(s_last_down_us[i] / 1000u);
        }
        printf("  %-10s %5u  %3u  %3u  %7ld\n", k_pad_names[i],
               (unsigned)p->gpio, p->raw_high ? 1u : 0u,
               p->active ? 1u : 0u, (long)down_ms);
    }

    printf("  slider: last=%s dir=%+d repeat_due_ms=%ld event_log=%u\n",
           (s_last_pad < 3u) ? k_pad_names[s_last_pad] : "-",
           (int)s_direction,
           (s_repeat_at_us != 0u) ? (long)(s_repeat_at_us / 1000u) : -1L,
           (unsigned)s_log_count);

    printf("  events (oldest first):\n");
    if (s_log_count == 0u) {
        printf("    none\n");
        return;
    }

    uint8_t idx = (uint8_t)((s_log_head + INPUT_TOUCHPADS_EVENT_LOG -
                             s_log_count) %
                            INPUT_TOUCHPADS_EVENT_LOG);
    for (uint8_t n = 0; n < s_log_count; n++) {
        const input_touchpad_event_t *ev = &s_log[idx];

        printf("    t=%7lums %-11s pad=%-10s ticks=%+d button=%s\n",
               (unsigned long)ev->t_ms,
               (ev->type < sizeof(k_event_names) / sizeof(k_event_names[0]))
                   ? k_event_names[ev->type]
                   : "?",
               (ev->pad < INPUT_TOUCHPAD_COUNT) ? k_pad_names[ev->pad] : "?",
               (int)ev->ticks, button_name(ev->button));
        idx = (uint8_t)((idx + 1u) % INPUT_TOUCHPADS_EVENT_LOG);
    }
}
