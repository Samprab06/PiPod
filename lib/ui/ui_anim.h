/**
 * @file ui_anim.h
 * @brief Time-based easing helpers used by the UI screens.
 *
 * Every animation in lib/ui steps from a microsecond delta, never from a frame
 * count, so the behaviour is identical whether a frame took 2 ms or 30 ms.
 */

#ifndef UI_ANIM_H
#define UI_ANIM_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Clamp @p v to [0, 1]. */
float ui_clamp01(float v);

/** @brief Ease-out cubic: fast start, gentle stop. @param t Progress in [0, 1]. */
float ui_ease_out_cubic(float t);

/**
 * @brief Frame-rate independent approach factor for exponential easing.
 *
 * Returns 1 - exp(-dt / tau), i.e. the fraction of the remaining distance to
 * travel this frame. @p dt_s is clamped to 100 ms so a long stall cannot make
 * an animation jump (and expf stays cheap).
 */
float ui_anim_factor(float dt_s, float tau_s);

/** @brief Move @p current toward @p target by the factor of a dt/tau window. */
float ui_anim_approach(float current, float target, float dt_s, float tau_s);

/** @brief True when two animated values are within @p eps pixels. */
bool ui_anim_close(float a, float b, float eps);

#ifdef __cplusplus
}
#endif

#endif /* UI_ANIM_H */
