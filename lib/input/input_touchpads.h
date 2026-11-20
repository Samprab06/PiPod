/**
 * @file input_touchpads.h
 * @brief Six TTP223 capacitive pads (three-pad slider + three buttons) ->
 *        abstract UI wheel / button events.
 *
 * The six modules are plain on/off capacitive switches: a pad is either
 * touched or not, there is no position or pressure. Each module's output is
 * normally push-pull and active high (high while touched), but that is not
 * guaranteed by every module / jumper combination, so the active level is a
 * config value.
 *
 * Slider
 * ------
 * The order in which LEFT, MIDDLE and RIGHT become active is decoded into
 * wheel ticks:
 *   - an adjacent activation (LEFT->MIDDLE, MIDDLE->RIGHT) within
 *     slide_window_ms is one step clockwise;
 *   - the reverse adjacent activation is one step counter-clockwise;
 *   - LEFT->RIGHT (or RIGHT->LEFT) counts as two steps only when the skipped
 *     MIDDLE pad fired inside the same window, otherwise it is ignored;
 *   - a pad touched and released without moving emits nothing;
 *   - after a slide, holding the end pad for hold_before_repeat_ms repeats a
 *     step every repeat_rate_ms in the last direction (like the wheel
 *     pseudo-buttons in input_buttons.c, but with the longer TTP223 times).
 *
 * Every emitted step is posted as a UI_INPUT_WHEEL event with
 * ticks_per_step ticks, so the UI's smooth scroll animation runs unchanged.
 *
 * Buttons
 * -------
 * SELECT, MENU and PLAY/PAUSE produce exactly the same events as
 * input_buttons.c: BUTTON_DOWN when the debounced press is accepted and
 * BUTTON_UP on release. The UI derives click versus long press from those
 * edges (see ui.h), so this driver must not invent its own long-press event.
 *
 * Power-up / calibration
 * ----------------------
 * TTP223 modules self-calibrate at power-up. Do not touch any pad while the
 * board powers up, and call input_touchpads_init() with all pads untouched.
 * The response time is up to ~220 ms in low-power mode and ~60 ms in fast
 * mode; the default slide_window_ms (400 ms) covers a normal finger slide
 * plus that latency. Poll every 1-5 ms (about 200 Hz-1 kHz); slower polling
 * is fine because the debounce integrates elapsed time, but keep well above
 * the 400 ms slide window.
 *
 * Threading: init, poll and debug_print are meant to be called from one core.
 * Posting to the UI is safe from any core, but the adapter state is not
 * re-entrant.
 */

#ifndef INPUT_TOUCHPADS_H
#define INPUT_TOUCHPADS_H

/** Any pad GPIO may be unused; it is never initialised or sampled. */
#define INPUT_TOUCHPADS_GPIO_UNUSED 0xFFu

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default breadboard pins (approved map; the integration worker passes the
 *  board's pins explicitly instead of relying on these). */
#define INPUT_TOUCHPADS_DEFAULT_GPIO_LEFT        2u /**< slider LEFT  GP2 */
#define INPUT_TOUCHPADS_DEFAULT_GPIO_MIDDLE      3u /**< slider MID   GP3 */
#define INPUT_TOUCHPADS_DEFAULT_GPIO_RIGHT       4u /**< slider RIGHT GP4 */
#define INPUT_TOUCHPADS_DEFAULT_GPIO_SELECT      5u /**< SELECT   GP5 */
#define INPUT_TOUCHPADS_DEFAULT_GPIO_MENU        6u /**< MENU     GP6 */
#define INPUT_TOUCHPADS_DEFAULT_GPIO_PLAY_PAUSE  7u /**< PLAY/PAUSE GP7 */

#define INPUT_TOUCHPADS_DEFAULT_DEBOUNCE_MS      20u
#define INPUT_TOUCHPADS_DEFAULT_SLIDE_WINDOW_MS  400u
#define INPUT_TOUCHPADS_DEFAULT_HOLD_MS          500u
#define INPUT_TOUCHPADS_DEFAULT_REPEAT_MS        150u
#define INPUT_TOUCHPADS_DEFAULT_TICKS_PER_STEP   1u

/** Number of decoded events kept for input_touchpads_debug_print(). */
#define INPUT_TOUCHPADS_EVENT_LOG 16u

/** Pad ids, in poll order. The first three are the slider, the last three the
 *  button pads. */
typedef enum {
    INPUT_TOUCHPAD_LEFT = 0,
    INPUT_TOUCHPAD_MIDDLE,
    INPUT_TOUCHPAD_RIGHT,
    INPUT_TOUCHPAD_SELECT,
    INPUT_TOUCHPAD_MENU,
    INPUT_TOUCHPAD_PLAY_PAUSE,
    INPUT_TOUCHPAD_COUNT
} input_touchpad_id_t;

/** Decoded event types, for the optional diagnostics callback and the event
 *  log printed by input_touchpads_debug_print(). */
typedef enum {
    INPUT_TOUCHPAD_EV_PAD_DOWN = 0, /**< debounced raw edge, pad valid */
    INPUT_TOUCHPAD_EV_PAD_UP,       /**< debounced raw edge, pad valid */
    INPUT_TOUCHPAD_EV_SLIDE,        /**< slider step, ticks = +/- ticks_per_step */
    INPUT_TOUCHPAD_EV_REPEAT,       /**< auto-repeat step, ticks = +/- ticks_per_step */
    INPUT_TOUCHPAD_EV_BUTTON_DOWN,  /**< same edge the UI receives, button valid */
    INPUT_TOUCHPAD_EV_BUTTON_UP     /**< same edge the UI receives, button valid */
} input_touchpad_event_type_t;

/** One decoded event. */
typedef struct {
    uint8_t  type;   /**< input_touchpad_event_type_t */
    uint8_t  pad;    /**< input_touchpad_id_t */
    uint8_t  button; /**< ui_button_t for button events; 0xFF otherwise */
    int16_t  ticks;  /**< wheel ticks for SLIDE / REPEAT; 0 otherwise */
    uint32_t t_ms;   /**< driver time of the event in ms since init */
} input_touchpad_event_t;

/** Optional diagnostics callback; called for every decoded event. */
typedef void (*input_touchpad_event_cb_t)(const input_touchpad_event_t *ev,
                                          void *user);

/** Driver configuration. Fill with input_touchpads_default_config() and
 *  override the fields the board needs. */
typedef struct {
    uint8_t  slider_left_gpio;       /**< TTP223 OUT for slider LEFT */
    uint8_t  slider_middle_gpio;     /**< TTP223 OUT for slider MIDDLE */
    uint8_t  slider_right_gpio;      /**< TTP223 OUT for slider RIGHT */
    uint8_t  button_select_gpio;     /**< TTP223 OUT for SELECT */
    uint8_t  button_menu_gpio;       /**< TTP223 OUT for MENU */
    uint8_t  button_play_pause_gpio; /**< TTP223 OUT for PLAY/PAUSE */

    /** true: an output reads high while touched (the usual TTP223 setting).
     *  false: active low. */
    bool     active_high;

    /** true: enable the internal pull that matches active_high (pull-down for
     *  active-high, pull-up for active-low). The modules normally drive both
     *  levels themselves, so this is only a fallback; it can be turned off.
     *  Note the RP2350-E9 erratum makes internal pull-downs weak/unreliable. */
    bool     internal_pulls;

    uint32_t debounce_ms;         /**< stable time before an edge is accepted (0 = 20) */
    uint32_t slide_window_ms;     /**< max time between two slider activations (0 = 400) */
    uint32_t hold_before_repeat_ms; /**< end-pad hold before auto-repeat (0 = 500) */
    uint32_t repeat_rate_ms;      /**< auto-repeat period (0 = 150) */
    uint16_t ticks_per_step;      /**< wheel ticks per slider step (0 = 1) */

    input_touchpad_event_cb_t event_cb; /**< optional diagnostics callback */
    void *event_user;                   /**< opaque pointer passed to event_cb */
} input_touchpads_config_t;

/** @brief Fill @p cfg with the breadboard defaults (GP2..GP7, active high,
 *  internal pulls on, 20 ms debounce, 400 ms slide window, 500 ms hold,
 *  150 ms repeat, 1 tick per step).
 *  @param cfg Destination; NULL is ignored. */
void input_touchpads_default_config(input_touchpads_config_t *cfg);

/**
 * @brief Configure the six pads and reset the debouncer / slider state.
 *
 * Call once after the TTP223 modules have powered up and self-calibrated,
 * with no pad touched. The config is copied; zero-valued timing fields are
 * replaced by the defaults.
 *
 * @param cfg Configuration; NULL uses the default configuration.
 */
void input_touchpads_init(const input_touchpads_config_t *cfg);

/**
 * @brief Sample the pads, debounce and post decoded UI events.
 *
 * Call every 1-5 ms (about 1 kHz or less). Elapsed time is integrated, so
 * short jitter in the call interval does not change the debounce result.
 */
void input_touchpads_poll(void);

/**
 * @brief Print raw pad levels, debounced state and the last decoded events
 *        over printf (for wiring / active-level bring-up).
 *
 * Shows the level sampled by the most recent poll(), so call poll() first
 * (or just call this from the main loop). Only prints; it never posts events.
 */
void input_touchpads_debug_print(void);

#ifdef INPUT_TOUCHPADS_HOST
/* ---------------------------------------------------------------------- */
/* Host-test hooks. Compiled only when the driver is built with            */
/* -DINPUT_TOUCHPADS_HOST (the hardware GPIO / timer calls are omitted).    */
/* ---------------------------------------------------------------------- */

/** @brief Set the simulated raw pin level of one pad (pre active-level
 *  inversion). Starts low. */
void input_touchpads_host_set_raw(input_touchpad_id_t pad, bool high);

/** @brief Advance the simulated driver clock by @p us microseconds. */
void input_touchpads_host_advance_us(uint64_t us);

/** @brief Current simulated driver time in microseconds. */
uint64_t input_touchpads_host_now_us(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* INPUT_TOUCHPADS_H */
