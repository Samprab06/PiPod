/**
 * @file main.c
 * @brief touchpad_test: bring-up tool for three optional TTP223 slider pads.
 *
 * Prints the raw level and debounced state of every pad whenever a decoded
 * event happens (pad down/up, slider step, repeat, button down/up) and prints
 * one full state dump per event. No display and no audio: UART0 only
 * (GP0 TX / GP1 RX, 115200 8N1).
 *
 * The TTP223 modules self-calibrate at power-up: do not touch any pad while
 * the board boots. Pins come from pipod_pins.h.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/time.h"

#include "pipod_pins.h" /* must precede anything that uses the board map */

#include "input_touchpads.h"
#include "ui.h"

#ifndef INPUT_TOUCHPADS_GPIO_UNUSED
/* TODO: remove fallback when the input worker's optional-pad API lands. */
#define INPUT_TOUCHPADS_GPIO_UNUSED 0xFFu
#endif

#define POLL_INTERVAL_US 2000u

static volatile bool s_dump;
static uint32_t      s_events;

static const char *pad_name(uint8_t pad) {
    switch (pad) {
        case INPUT_TOUCHPAD_LEFT:
            return "LEFT";
        case INPUT_TOUCHPAD_MIDDLE:
            return "MIDDLE";
        case INPUT_TOUCHPAD_RIGHT:
            return "RIGHT";
        case INPUT_TOUCHPAD_SELECT:
            return "SELECT";
        case INPUT_TOUCHPAD_MENU:
            return "MENU";
        case INPUT_TOUCHPAD_PLAY_PAUSE:
            return "PLAY_PAUSE";
        default:
            return "?";
    }
}

static const char *event_name(uint8_t type) {
    switch (type) {
        case INPUT_TOUCHPAD_EV_PAD_DOWN:
            return "PAD_DOWN";
        case INPUT_TOUCHPAD_EV_PAD_UP:
            return "PAD_UP";
        case INPUT_TOUCHPAD_EV_SLIDE:
            return "SLIDE";
        case INPUT_TOUCHPAD_EV_REPEAT:
            return "REPEAT";
        case INPUT_TOUCHPAD_EV_BUTTON_DOWN:
            return "BTN_DOWN";
        case INPUT_TOUCHPAD_EV_BUTTON_UP:
            return "BTN_UP";
        default:
            return "?";
    }
}

static const char *button_name(uint8_t button) {
    switch (button) {
        case UI_BTN_SELECT:
            return "SELECT";
        case UI_BTN_MENU:
            return "MENU";
        case UI_BTN_PLAY_PAUSE:
            return "PLAY_PAUSE";
        case UI_BTN_NEXT:
            return "NEXT";
        case UI_BTN_PREV:
            return "PREV";
        default:
            return "-";
    }
}

static void on_event(const input_touchpad_event_t *ev, void *user) {
    (void)user;
    s_events++;

    printf("[ev] t=%7lums %-9s pad=%-10s", (unsigned long)ev->t_ms,
           event_name(ev->type), pad_name(ev->pad));

    if (ev->type == INPUT_TOUCHPAD_EV_SLIDE || ev->type == INPUT_TOUCHPAD_EV_REPEAT) {
        printf(" %s (%+d tick%s)\n", ev->ticks > 0 ? "clockwise" : "counter-clockwise",
               (int)ev->ticks, (ev->ticks == 1 || ev->ticks == -1) ? "" : "s");
    } else if (ev->type == INPUT_TOUCHPAD_EV_BUTTON_DOWN ||
               ev->type == INPUT_TOUCHPAD_EV_BUTTON_UP) {
        printf(" button=%s\n", button_name(ev->button));
    } else {
        printf("\n");
    }

    /* Caller prints the state dump after poll() returns. */
    s_dump = true;
}

int main(void) {
    stdio_init_all();
    sleep_ms(100);

    printf("\n=== PiPod touchpad_test (TTP223) ===\n");
    printf("[pins] slider LEFT GP%u MIDDLE GP%u RIGHT GP%u; button pads unused\n",
           (unsigned)PIPOD_PIN_TTP223_1, (unsigned)PIPOD_PIN_TTP223_2,
           (unsigned)PIPOD_PIN_TTP223_3);
    printf("[touch] do not touch any pad during power-up calibration\n");
    printf("[touch] VCC 3.3 V, GND, OUT -> GPIO; default jumpers = active high "
           "(PIPOD_TOUCH_ACTIVE_HIGH=%u)\n",
           (unsigned)PIPOD_TOUCH_ACTIVE_HIGH);

    input_touchpads_config_t cfg;
    input_touchpads_default_config(&cfg);
    cfg.slider_left_gpio = (uint8_t)PIPOD_PIN_TTP223_1;
    cfg.slider_middle_gpio = (uint8_t)PIPOD_PIN_TTP223_2;
    cfg.slider_right_gpio = (uint8_t)PIPOD_PIN_TTP223_3;
    cfg.button_select_gpio = INPUT_TOUCHPADS_GPIO_UNUSED;
    cfg.button_menu_gpio = INPUT_TOUCHPADS_GPIO_UNUSED;
    cfg.button_play_pause_gpio = INPUT_TOUCHPADS_GPIO_UNUSED;
    cfg.active_high = (PIPOD_TOUCH_ACTIVE_HIGH != 0);
    cfg.internal_pulls = false; /* TTP223 drives both levels; RP2350-E9 */
    cfg.event_cb = on_event;
    cfg.event_user = NULL;

    input_touchpads_init(&cfg);
    printf("[touch] armed: debounce %lu ms, slide window %lu ms, hold %lu ms, "
           "repeat %lu ms, %u tick/step\n",
           (unsigned long)cfg.debounce_ms, (unsigned long)cfg.slide_window_ms,
           (unsigned long)cfg.hold_before_repeat_ms,
           (unsigned long)cfg.repeat_rate_ms, (unsigned)cfg.ticks_per_step);
    printf("[touch] touch a pad: events and the state dump print here\n\n");

    input_touchpads_debug_print();

    absolute_time_t next_poll = get_absolute_time();

    for (;;) {
        input_touchpads_poll();

        if (s_dump) {
            s_dump = false;
            input_touchpads_debug_print();
            printf("[touch] events so far: %lu\n\n", (unsigned long)s_events);
        }

        next_poll = delayed_by_us(next_poll, POLL_INTERVAL_US);
        if (time_reached(next_poll)) {
            next_poll = delayed_by_us(get_absolute_time(), POLL_INTERVAL_US);
        }
        sleep_until(next_poll);
    }
}
