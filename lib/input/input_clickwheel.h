#ifndef INPUT_CLICKWHEEL_H
#define INPUT_CLICKWHEEL_H
#include <stdbool.h>
#include <stdint.h>
#include "input_wheel_math.h"

/* Pure logic; button IDs are intentionally independent of the UI. */
typedef enum {
    CLICKWHEEL_NONE, CLICKWHEEL_MENU, CLICKWHEEL_NEXT, CLICKWHEEL_PLAY_PAUSE,
    CLICKWHEEL_PREV, CLICKWHEEL_SELECT
} input_clickwheel_button_t;
typedef struct {
    uint8_t ring[11];                 /* electrode IDs, clockwise from top */
    uint8_t centre;
    input_clickwheel_button_t function[12];
    uint16_t threshold, ticks_per_rev, hysteresis_deg10, move_deg10;
    uint32_t tap_ms, hold_ms;
} input_clickwheel_config_t;
typedef void (*input_clickwheel_wheel_cb)(int16_t ticks, void *user);
typedef void (*input_clickwheel_button_cb)(input_clickwheel_button_t button,
                                          bool down, void *user);
typedef struct {
    input_clickwheel_config_t config;
    input_clickwheel_wheel_cb wheel_cb;
    input_clickwheel_button_cb button_cb;
    void *user;
    int32_t angle_deg10, start_angle, pending;
    input_wheel_state_t quantizer;
    uint32_t start_ms;
    input_clickwheel_button_t candidate;
    bool active, scrolling, held, centre_down;
} input_clickwheel_t;
void input_clickwheel_default_config(input_clickwheel_config_t *config);
/* NULL config selects defaults. Ring IDs must be unique 0..11, centre 0..11
 * and not on the ring. threshold is in sensor counts; angular fields are
 * tenths of degrees. A committed hold is button-only until release.
 * Releases between tap_ms and hold_ms intentionally produce no event.
 * Movement during arbitration is accumulated and flushed when scroll wins. */
void input_clickwheel_init(input_clickwheel_t *wheel,
    const input_clickwheel_config_t *config, input_clickwheel_wheel_cb wheel_cb,
    input_clickwheel_button_cb button_cb, void *user);
/* deltas may be NULL (mask-only). With deltas, only mask-active ring pads
 * participate, preventing baseline noise from moving the centroid. To use
 * deltas alone, pass a mask derived from deltas >= threshold. Centre uses mask.
 * now_ms is unsigned monotonic time; wrap is supported. Single-core caller. */
void input_clickwheel_feed(input_clickwheel_t *wheel, const uint16_t deltas[12],
                           uint16_t touched_mask, uint32_t now_ms);
#endif
