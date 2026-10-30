/**
 * @file input_wheel.h
 * @brief Continuous capacitive wheel -> abstract wheel ticks.
 *
 * On the breadboard the wheel is simulated by two push buttons (see
 * input_buttons.h). On the future PCB the wheel is a ring of capacitive
 * electrodes behind an MPR121 touch controller. This module is the glue that
 * turns *readings* (not I2C) into the same abstract +/-1 wheel ticks the UI
 * consumes. The touch teammate owns the MPR121 driver and calls
 * input_wheel_feed_pads() (or, when they already have an angle,
 * input_wheel_feed_angle()) at whatever rate their polling runs.
 *
 * Two tunables:
 *  - ticks_per_rev: wheel detents per full turn (24 by default, matching the
 *    iPod-style feel and CLICKWHEEL_CLICK_COUNT).
 *  - hysteresis_deg10: reversals smaller than this are absorbed so a resting
 *    finger does not oscillate between +/-1. Expressed in tenths of a degree.
 *
 * A movement larger than 90 degrees between two readings is treated as a
 * re-touch/slip and ignored (the reference angle is re-armed at the new
 * position).
 */

#ifndef INPUT_WHEEL_H
#define INPUT_WHEEL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INPUT_WHEEL_DEFAULT_TICKS_PER_REV    24u
#define INPUT_WHEEL_DEFAULT_HYSTERESIS_DEG10 40u /**< 4.0 degrees */

/** Default touch threshold for input_wheel_angle_from_pads(): any electrode
 *  at or above this counts as touched. Raw sensor units (e.g. MPR121 filtered
 *  counts); callers with their own noise floor should use
 *  input_wheel_feed_pads(..., threshold) instead. */
#define INPUT_WHEEL_PAD_TOUCH_THRESHOLD 200u

/** @brief Reset wheel state and configure the tick/hysteresis geometry.
 *  @param ticks_per_rev   Detents per revolution (0 = 24).
 *  @param hysteresis_deg10 Reversal deadband in 0.1 deg (0 = 4.0 deg). */
void input_wheel_init(uint16_t ticks_per_rev, uint16_t hysteresis_deg10);

/**
 * @brief Feed one angle sample and emit any completed ticks.
 *
 * @param angle_deg10 Angle in 0.1 degree, 0..3599 clockwise from 12 o'clock.
 * @param touching    false when the finger is off the wheel; the next true
 *                    sample re-arms the reference without producing ticks.
 */
void input_wheel_feed_angle(int32_t angle_deg10, bool touching);

/**
 * @brief Compute a wheel angle from electrode strengths.
 *
 * Weighted circular centroid of @p n_pads evenly spaced electrodes, pad 0 at
 * 12 o'clock, increasing clockwise. Electrodes below
 * INPUT_WHEEL_PAD_TOUCH_THRESHOLD are ignored.
 *
 * @param pad_strength n_pads raw strengths.
 * @param n_pads       Number of electrodes (2..32).
 * @return Angle in 0.1 degree (0..3599), or -1 if no pad is above threshold.
 */
int32_t input_wheel_angle_from_pads(const uint16_t *pad_strength, uint8_t n_pads);

/**
 * @brief Convenience: angles from pads then feed_angle().
 * @param pad_strength n_pads raw strengths.
 * @param n_pads       Number of electrodes.
 * @param threshold    Touch threshold in raw units; values below are ignored.
 */
void input_wheel_feed_pads(const uint16_t *pad_strength, uint8_t n_pads, uint16_t threshold);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_WHEEL_H */
