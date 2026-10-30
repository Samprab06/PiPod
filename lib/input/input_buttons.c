/**
 * @file input_buttons.c
 * @brief Discrete GPIO buttons with integrating debounce and wheel auto-repeat.
 */

#include "input_buttons.h"

#include <string.h>

#include "hardware/gpio.h"
#include "pico/time.h"
#include "ui.h"

#define DEFAULT_DEBOUNCE_MS 5u
#define REPEAT_DELAY_US     250000u
#define REPEAT_RATE_US      80000u
#define MAX_POLL_GAP_US     20000u /**< clamp the dt after a long stall */

typedef struct {
    uint8_t  gpio;
    uint8_t  ui_button;
    bool     active_high;

    int32_t  integrator_us; /**< time the raw level has disagreed with `pressed` */
    bool     pressed;       /**< debounced state */
    uint64_t next_repeat_us;
} input_button_state_t;

static input_button_state_t s_buttons[INPUT_BUTTONS_MAX];
static size_t   s_count;
static uint32_t s_debounce_us;
static uint64_t s_last_poll_us;
static bool     s_initialised;

static const input_button_map_t k_default_map[] = {
    /* gpio, ui_button,       active_high */
    {2,  INPUT_WHEEL_CW,  true}, /* wheel clockwise */
    {3,  INPUT_WHEEL_CCW, true}, /* wheel counter-clockwise */
    {4,  UI_BTN_PLAY_PAUSE, true},
    {8,  UI_BTN_PREV,       true},
    {9,  UI_BTN_NEXT,       true},
    {21, UI_BTN_SELECT,     true}, /* on-board button */
    {26, UI_BTN_MENU,       true}, /* on-board button */
};

const input_button_map_t *input_buttons_default_map(void) {
    return k_default_map;
}

size_t input_buttons_default_map_count(void) {
    return sizeof(k_default_map) / sizeof(k_default_map[0]);
}

static bool is_wheel(uint8_t ui_button) {
    return ui_button == INPUT_WHEEL_CW || ui_button == INPUT_WHEEL_CCW;
}

static void post_press(uint8_t ui_button) {
    (void)ui_post_button((ui_button_t)ui_button, true);
}

static void post_release(uint8_t ui_button) {
    (void)ui_post_button((ui_button_t)ui_button, false);
}

static void post_wheel(uint8_t ui_button) {
    (void)ui_post_wheel(ui_button == INPUT_WHEEL_CW ? 1 : -1);
}

void input_buttons_init(const input_button_map_t *map, size_t n, uint32_t debounce_ms) {
    if (n > INPUT_BUTTONS_MAX) {
        n = INPUT_BUTTONS_MAX;
    }
    memset(s_buttons, 0, sizeof(s_buttons));
    s_count = 0;

    if (map != NULL) {
        for (size_t i = 0; i < n; i++) {
            if (map[i].ui_button >= UI_BTN_COUNT && !is_wheel(map[i].ui_button)) {
                continue; /* not a UI button we can post */
            }
            input_button_state_t *b = &s_buttons[s_count];

            b->gpio = map[i].gpio;
            b->ui_button = map[i].ui_button;
            b->active_high = map[i].active_high;

            gpio_init(b->gpio);
            gpio_set_dir(b->gpio, GPIO_IN);
            gpio_set_input_enabled(b->gpio, true);
            /* RP2350-E9: never rely on an internal pull-down. The breadboard
             * map requires external 10k pull-downs and drives the pin high
             * when the button is pressed. */
            gpio_disable_pulls(b->gpio);

            b->pressed = (gpio_get(b->gpio) != 0) == b->active_high;
            b->integrator_us = 0;
            b->next_repeat_us = 0;
            s_count++;
        }
    }

    s_debounce_us = (debounce_ms == 0u ? DEFAULT_DEBOUNCE_MS : debounce_ms) * 1000u;
    s_last_poll_us = time_us_64();
    s_initialised = true;
}

void input_buttons_poll(void) {
    if (!s_initialised) {
        return;
    }

    uint64_t now = time_us_64();
    uint64_t gap = now - s_last_poll_us;
    s_last_poll_us = now;
    if (gap > MAX_POLL_GAP_US) {
        gap = MAX_POLL_GAP_US;
    }
    int32_t dt = (gap == 0u) ? 1 : (int32_t)gap;

    for (size_t i = 0; i < s_count; i++) {
        input_button_state_t *b = &s_buttons[i];
        bool raw = (gpio_get(b->gpio) != 0) == b->active_high;

        if (raw != b->pressed) {
            /* Integrating debounce: an opposite level has to persist for the
             * whole debounce window, no matter how the polls are spaced. */
            b->integrator_us += dt;
            if (b->integrator_us >= (int32_t)s_debounce_us) {
                b->integrator_us = 0;
                b->pressed = raw;

                if (raw) {
                    if (is_wheel(b->ui_button)) {
                        post_wheel(b->ui_button);
                        b->next_repeat_us = now + REPEAT_DELAY_US;
                    } else {
                        post_press(b->ui_button);
                    }
                } else {
                    if (!is_wheel(b->ui_button)) {
                        post_release(b->ui_button);
                    }
                    b->next_repeat_us = 0;
                }
            }
        } else {
            b->integrator_us -= dt;
            if (b->integrator_us < 0) {
                b->integrator_us = 0;
            }
        }

        /* Wheel pseudo-buttons auto-repeat while held. */
        if (b->pressed && is_wheel(b->ui_button) && b->next_repeat_us != 0u &&
            now >= b->next_repeat_us) {
            post_wheel(b->ui_button);
            b->next_repeat_us = now + REPEAT_RATE_US;
        }
    }
}
