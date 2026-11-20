#include "input_clickwheel.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

static int32_t difference(int32_t a, int32_t b) {
    int32_t d = a - b;
    if (d > 1800) d -= 3600;
    if (d < -1800) d += 3600;
    return d;
}
void input_clickwheel_default_config(input_clickwheel_config_t *c) {
    memset(c, 0, sizeof(*c));
    for (unsigned i = 0; i < 11; ++i) c->ring[i] = (uint8_t)i;
    c->centre = 11;
    c->function[0] = CLICKWHEEL_MENU;
    c->function[3] = CLICKWHEEL_NEXT;
    c->function[5] = c->function[6] = CLICKWHEEL_PLAY_PAUSE;
    c->function[8] = CLICKWHEEL_PREV;
    c->function[11] = CLICKWHEEL_SELECT;
    c->threshold = 6;
    c->ticks_per_rev = 24;
    c->hysteresis_deg10 = 40;
    c->move_deg10 = 250;
    c->tap_ms = 350;
    c->hold_ms = 500;
}
void input_clickwheel_init(input_clickwheel_t *w,
    const input_clickwheel_config_t *c, input_clickwheel_wheel_cb wc,
    input_clickwheel_button_cb bc, void *user) {
    memset(w, 0, sizeof(*w));
    if (c) w->config = *c;
    else input_clickwheel_default_config(&w->config);
    if (!w->config.ticks_per_rev) w->config.ticks_per_rev = 24;
    w->quantizer.ticks_per_rev = w->config.ticks_per_rev;
    w->quantizer.hysteresis_deg10 = w->config.hysteresis_deg10;
    w->wheel_cb = wc;
    w->button_cb = bc;
    w->user = user;
    w->angle_deg10 = -1;
}
static void button(input_clickwheel_t *w, input_clickwheel_button_t b, bool down) {
    if (b != CLICKWHEEL_NONE && w->button_cb) w->button_cb(b, down, w->user);
}
void input_clickwheel_feed(input_clickwheel_t *w, const uint16_t deltas[12],
                           uint16_t mask, uint32_t now) {
    const input_clickwheel_config_t *c = &w->config;
    bool centre = c->centre < 12 && (mask & (1u << c->centre));
    if (centre != w->centre_down) {
        button(w, c->function[c->centre], centre);
        w->centre_down = centre;
    }
    if (centre) {
        if (w->held) button(w, w->candidate, false);
        w->active = w->held = false;
        (void)input_wheel_feed_angle_to_state(&w->quantizer, 0, false);
        w->angle_deg10 = -1;
        return;
    }
    float east = 0, north = 0;
    unsigned strongest = 12;
    uint16_t max_weight = 0;
    for (unsigned k = 0; k < 11; ++k) {
        unsigned e = c->ring[k];
        if (e >= 12 || !(mask & (1u << e))) continue;
        uint16_t weight = deltas ? deltas[e] : 1;
        if (deltas && weight < c->threshold) continue;
        float rad = (float)k * 6.28318530718f / 11.0f;
        east += weight * sinf(rad);
        north += weight * cosf(rad);
        if (weight > max_weight) { max_weight = weight; strongest = e; }
    }
    bool touching = strongest < 12 && east * east + north * north > 0.0001f;
    w->angle_deg10 = touching ? ((int32_t)lroundf(atan2f(east, north) *
                                      572.95779513f) + 3600) % 3600 : -1;
    if (!touching) {
        if (w->active) {
            if (w->held) button(w, w->candidate, false);
            else if (!w->scrolling && now - w->start_ms <= c->tap_ms) {
                button(w, w->candidate, true);
                button(w, w->candidate, false);
            }
        }
        w->active = w->held = false;
        (void)input_wheel_feed_angle_to_state(&w->quantizer, 0, false);
        return;
    }
    if (!w->active) {
        w->active = true;
        w->start_ms = now;
        w->start_angle = w->angle_deg10;
        (void)input_wheel_feed_angle_to_state(&w->quantizer, w->angle_deg10, true);
        w->candidate = c->function[strongest];
        w->scrolling = w->candidate == CLICKWHEEL_NONE;
        w->held = false;
        w->pending = 0;
        return;
    }
    if (w->held) return; /* committed hold cannot later turn into scroll */
    w->pending += input_wheel_feed_angle_to_state(&w->quantizer, w->angle_deg10, true);
    int32_t travel = difference(w->angle_deg10, w->start_angle);
    if (travel > c->move_deg10 || travel < -(int32_t)c->move_deg10)
        w->scrolling = true;
    if (w->scrolling) {
        if (w->pending && w->wheel_cb) w->wheel_cb((int16_t)w->pending, w->user);
        w->pending = 0;
    } else if (now - w->start_ms >= c->hold_ms) {
        w->held = true;
        w->pending = 0;
        button(w, w->candidate, true);
    }
}
