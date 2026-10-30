/**
 * @file input_buttons.h
 * @brief Discrete GPIO buttons (breadboard demo) -> abstract UI button events.
 *
 * The adapter only ever produces abstract events through
 * ui_post_button() / ui_post_wheel(); it knows nothing about UI internals.
 *
 * Wiring (docs/design.md):
 *   GP2  wheel clockwise      GP3  wheel counter-clockwise
 *   GP4  play/pause           GP8  previous
 *   GP9  next                 GP21 select   (on-board)
 *   GP26 menu                 (on-board)
 *
 * All buttons are active high and rely on **external pull-downs**. The
 * RP2350-E9 erratum means internal pull-downs are unreliable (the pad can
 * float up), so this driver calls gpio_disable_pulls() and never enables an
 * internal pull-down. If you wire a button without an external pull-down the
 * input will read garbage.
 *
 * The two "wheel" entries in the map are pseudo-buttons: pressing one posts a
 * single wheel tick (+1 for CW, -1 for CCW) and then auto-repeats at
 * 250 ms delay / 80 ms rate while held.
 *
 * Threading: init and poll are meant to be called from one core (core 0 in
 * the demo) at a 1-5 ms cadence.
 */

#ifndef INPUT_BUTTONS_H
#define INPUT_BUTTONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Pseudo button ids for the wheel entries in an input_button_map_t.
 *  They are outside the ui_button_t range (0..UI_BTN_COUNT-1). */
#define INPUT_WHEEL_CW  0xF0u /**< each accepted press posts +1 wheel tick */
#define INPUT_WHEEL_CCW 0xF1u /**< each accepted press posts -1 wheel tick */

#define INPUT_BUTTONS_MAX 16 /**< map capacity */

/** One physical button -> logical UI action. */
typedef struct {
    uint8_t gpio;        /**< GPIO number of the input. */
    uint8_t ui_button;   /**< ui_button_t, or INPUT_WHEEL_CW / INPUT_WHEEL_CCW. */
    bool    active_high; /**< true if the pin reads high when pressed. */
} input_button_map_t;

/** Design-spec breadboard map (GP2/3/4/8/9/21/26, all active high).
 *  @return pointer to a static const table; do not free. */
const input_button_map_t *input_buttons_default_map(void);

/** Number of entries returned by input_buttons_default_map(). */
size_t input_buttons_default_map_count(void);

/**
 * @brief Configure the mapped GPIOs as inputs and arm the debouncer.
 *
 * @param map        Button map (copied). May be NULL to detach all buttons.
 * @param n          Number of entries, at most INPUT_BUTTONS_MAX.
 * @param debounce_ms Integrate time a level must be stable, in ms (0 = 5).
 */
void input_buttons_init(const input_button_map_t *map, size_t n, uint32_t debounce_ms);

/** @brief Poll the inputs; post events for accepted edges.
 *  Call every 1-5 ms. Uses an integrating (time-based) debounce, so it is
 *  tolerant of jitter in the call interval. */
void input_buttons_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_BUTTONS_H */
