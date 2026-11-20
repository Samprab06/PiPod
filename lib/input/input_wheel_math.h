#ifndef INPUT_WHEEL_MATH_H
#define INPUT_WHEEL_MATH_H
#include <stdbool.h>
#include <stdint.h>

/* Instance-local pure core of input_wheel_feed_angle. Both the legacy UI
 * adapter and clickwheel arbitration use exactly this quantizer. */
typedef struct {
    uint16_t ticks_per_rev, hysteresis_deg10;
    int32_t last_deg10, residue_deg10;
    bool has_ref;
} input_wheel_state_t;

static inline int16_t input_wheel_feed_angle_to_state(input_wheel_state_t *w,
                                                     int32_t angle, bool touching) {
    if (!touching) { w->has_ref = false; w->residue_deg10 = 0; return 0; }
    angle %= 3600;
    if (angle < 0) angle += 3600;
    if (!w->has_ref) {
        w->has_ref = true; w->last_deg10 = angle; w->residue_deg10 = 0;
        return 0;
    }
    int32_t delta = angle - w->last_deg10;
    if (delta > 1800) delta -= 3600;
    else if (delta <= -1800) delta += 3600;
    w->last_deg10 = angle;
    if (delta > 900 || delta < -900) { w->residue_deg10 = 0; return 0; }
    int32_t hyst = w->hysteresis_deg10;
    if (w->residue_deg10 > 0 && delta < 0) {
        w->residue_deg10 += delta;
        if (w->residue_deg10 < hyst) w->residue_deg10 = 0;
    } else if (w->residue_deg10 < 0 && delta > 0) {
        w->residue_deg10 += delta;
        if (w->residue_deg10 > -hyst) w->residue_deg10 = 0;
    } else w->residue_deg10 += delta;
    int32_t step = 3600 / (w->ticks_per_rev ? w->ticks_per_rev : 24);
    if (step < 1) step = 1;
    int32_t ticks = w->residue_deg10 / step;
    w->residue_deg10 -= ticks * step;
    return (int16_t)ticks;
}
#endif
