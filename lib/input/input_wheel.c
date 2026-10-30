/**
 * @file input_wheel.c
 * @brief Angle accumulation, wrap handling and circular centroid for a
 *        capacitive touch wheel.
 */

#include "input_wheel.h"

#include <math.h>

#include "ui.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEG10_FULL   3600
#define DEG10_HALF   1800
#define DEG10_MAX_JUMP 900 /**< > 90 degrees between samples = slip/re-touch */
#define MAX_PADS     32

typedef struct {
    uint16_t ticks_per_rev;
    uint16_t hysteresis_deg10;
    int32_t  last_deg10;   /**< reference angle, valid while has_ref */
    int32_t  residue_deg10; /**< sub-tick movement already accumulated */
    bool     has_ref;
} input_wheel_state_t;

static input_wheel_state_t s_wheel;

static int32_t wrap_deg10(int32_t deg10) {
    deg10 %= DEG10_FULL;
    if (deg10 < 0) {
        deg10 += DEG10_FULL;
    }
    return deg10;
}

void input_wheel_init(uint16_t ticks_per_rev, uint16_t hysteresis_deg10) {
    s_wheel.ticks_per_rev = (ticks_per_rev == 0u) ? INPUT_WHEEL_DEFAULT_TICKS_PER_REV
                                                  : ticks_per_rev;
    s_wheel.hysteresis_deg10 = (hysteresis_deg10 == 0u) ? INPUT_WHEEL_DEFAULT_HYSTERESIS_DEG10
                                                        : hysteresis_deg10;
    s_wheel.last_deg10 = 0;
    s_wheel.residue_deg10 = 0;
    s_wheel.has_ref = false;
}

void input_wheel_feed_angle(int32_t angle_deg10, bool touching) {
    if (!touching) {
        /* Finger off: drop the reference so the next touch cannot produce a
         * tick from the angle the finger happened to leave at. */
        s_wheel.has_ref = false;
        s_wheel.residue_deg10 = 0;
        return;
    }

    int32_t angle = wrap_deg10(angle_deg10);
    if (!s_wheel.has_ref) {
        s_wheel.last_deg10 = angle;
        s_wheel.residue_deg10 = 0;
        s_wheel.has_ref = true;
        return;
    }

    int32_t delta = angle - s_wheel.last_deg10;
    if (delta > DEG10_HALF) {
        delta -= DEG10_FULL;
    } else if (delta <= -DEG10_HALF) {
        delta += DEG10_FULL;
    }
    s_wheel.last_deg10 = angle;

    if (delta > DEG10_MAX_JUMP || delta < -DEG10_MAX_JUMP) {
        /* Same touch but a huge step: treat as a slip, re-arm here. */
        s_wheel.residue_deg10 = 0;
        return;
    }

    int32_t step = (int32_t)(DEG10_FULL / s_wheel.ticks_per_rev);
    if (step < 1) {
        step = 1;
    }
    int32_t hyst = (int32_t)s_wheel.hysteresis_deg10;

    if (s_wheel.residue_deg10 > 0 && delta < 0) {
        s_wheel.residue_deg10 += delta;
        /* Small reversal inside the deadband: absorb it. */
        if (s_wheel.residue_deg10 < hyst) {
            s_wheel.residue_deg10 = 0;
        }
    } else if (s_wheel.residue_deg10 < 0 && delta > 0) {
        s_wheel.residue_deg10 += delta;
        if (s_wheel.residue_deg10 > -hyst) {
            s_wheel.residue_deg10 = 0;
        }
    } else {
        s_wheel.residue_deg10 += delta;
    }

    int32_t ticks = 0;
    while (s_wheel.residue_deg10 >= step) {
        s_wheel.residue_deg10 -= step;
        ticks++;
    }
    while (s_wheel.residue_deg10 <= -step) {
        s_wheel.residue_deg10 += step;
        ticks--;
    }

    if (ticks != 0) {
        if (ticks > INT16_MAX) {
            ticks = INT16_MAX;
        } else if (ticks < INT16_MIN) {
            ticks = INT16_MIN;
        }
        (void)ui_post_wheel((int16_t)ticks);
    }
}

static int32_t angle_from_pads(const uint16_t *pad_strength, uint8_t n_pads, uint16_t threshold) {
    if (pad_strength == NULL || n_pads < 2u || n_pads > MAX_PADS) {
        return -1;
    }

    uint32_t total = 0;
    for (uint8_t i = 0; i < n_pads; i++) {
        if (pad_strength[i] >= threshold) {
            total += pad_strength[i];
        }
    }
    if (total == 0u) {
        return -1;
    }

    /* Pad i sits at i * (360/n) degrees clockwise from 12 o'clock, which is
     * (sin, cos) = (east, north). Walk the unit vector around the ring with a
     * rotation instead of calling sin/cos per electrode. */
    const float step_rad = (float)(2.0 * M_PI / (double)n_pads);
    const float cs = cosf(step_rad);
    const float sn = sinf(step_rad);
    float sh = 0.0f; /* sin(i*step) */
    float ch = 1.0f; /* cos(i*step) */
    float east = 0.0f;
    float north = 0.0f;

    for (uint8_t i = 0; i < n_pads; i++) {
        uint16_t w = (pad_strength[i] >= threshold) ? pad_strength[i] : 0u;
        east += (float)w * sh;
        north += (float)w * ch;

        float next_sh = sh * cs + ch * sn;
        ch = ch * cs - sh * sn;
        sh = next_sh;
    }

    /* atan2(east, north) is already clockwise from 12 o'clock. */
    float angle = atan2f(east, north);
    if (angle < 0.0f) {
        angle += (float)(2.0 * M_PI);
    }
    long deg10 = lroundf(angle * (float)(1800.0 / M_PI));
    if (deg10 >= DEG10_FULL) {
        deg10 -= DEG10_FULL;
    }
    return (int32_t)deg10;
}

int32_t input_wheel_angle_from_pads(const uint16_t *pad_strength, uint8_t n_pads) {
    return angle_from_pads(pad_strength, n_pads, INPUT_WHEEL_PAD_TOUCH_THRESHOLD);
}

void input_wheel_feed_pads(const uint16_t *pad_strength, uint8_t n_pads, uint16_t threshold) {
    int32_t angle = angle_from_pads(pad_strength, n_pads, threshold);
    input_wheel_feed_angle(angle, angle >= 0);
}
