/**
 * @file ui_anim.c
 * @brief Easing and exponential-approach helpers (time based, not frame based).
 */

#include "ui_anim.h"

#include <math.h>

static float clampf(float v, float lo, float hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

float ui_clamp01(float v) {
    return clampf(v, 0.0f, 1.0f);
}

float ui_ease_out_cubic(float t) {
    const float u = 1.0f - clampf(t, 0.0f, 1.0f);
    return 1.0f - u * u * u;
}

float ui_anim_factor(float dt_s, float tau_s) {
    if (tau_s <= 0.0f) {
        return 1.0f;
    }
    const float dt = clampf(dt_s, 0.0f, 0.1f);
    const float f = 1.0f - expf(-dt / tau_s);
    return clampf(f, 0.0f, 1.0f);
}

float ui_anim_approach(float current, float target, float dt_s, float tau_s) {
    const float f = ui_anim_factor(dt_s, tau_s);
    return current + (target - current) * f;
}

bool ui_anim_close(float a, float b, float eps) {
    const float d = a - b;
    return (d < 0.0f ? -d : d) <= eps;
}
