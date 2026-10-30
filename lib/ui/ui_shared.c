/**
 * @file ui_shared.c
 * @brief Cross-core state for lib/ui: input queue, command queue, playback.
 *
 * The UI core drains both queues once per frame; every other core (including
 * interrupt context) only ever posts. The input queue is a pico/util/queue_t,
 * which is spin-locked and safe from an IRQ. The command queue adds an outer
 * critical section so that a full queue can be coalesced (drop the oldest
 * SEEK/SET_VOLUME) instead of losing a user action.
 *
 * Playback state lives behind a critical section and carries a sequence
 * counter, so the UI can copy one consistent snapshot per frame. All strings
 * are copied and force-terminated here; non-ASCII bytes are replaced at draw
 * time by ui_widgets.c.
 */

#include "ui_internal.h"

#include <string.h>

#include "pico/critical_section.h"
#include "pico/util/queue.h"

#define UI_INPUT_QUEUE_LEN 32u /**< ui.h: 32 entries. */
#define UI_CMD_QUEUE_LEN 16u   /**< ui.h: 16 commands. */

static queue_t s_in_q;
static queue_t s_cmd_q;
static critical_section_t s_cmd_cs;
static critical_section_t s_np_cs;

static ui_np_t s_np_state;
static volatile bool s_shared_ready;
static uint32_t s_dropped_inputs;
static uint32_t s_cmd_dropped;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static bool shared_ready(void) {
    return __atomic_load_n(&s_shared_ready, __ATOMIC_ACQUIRE);
}

static void copy_text(char *dst, size_t cap, const char *src) {
    if (cap == 0u) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    for (; src[i] != '\0' && i + 1u < cap; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* ------------------------------------------------------------------------- */
/* Initialisation                                                            */
/* ------------------------------------------------------------------------- */

void ui_shared_init(uint8_t initial_volume) {
    if (shared_ready()) {
        return;
    }
    (void)queue_init(&s_in_q, sizeof(ui_input_event_t), UI_INPUT_QUEUE_LEN);
    (void)queue_init(&s_cmd_q, sizeof(ui_command_t), UI_CMD_QUEUE_LEN);
    critical_section_init(&s_cmd_cs);
    critical_section_init(&s_np_cs);

    memset(&s_np_state, 0, sizeof s_np_state);
    if (initial_volume > 100u) {
        initial_volume = 100u;
    }
    s_np_state.volume = initial_volume;
    s_np_state.battery = 100u;
    s_np_state.play = UI_PLAY_STOPPED;
    s_np_state.seq = 1u;
    s_dropped_inputs = 0;
    s_cmd_dropped = 0;

    __atomic_store_n(&s_shared_ready, true, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------------- */
/* Input queue                                                               */
/* ------------------------------------------------------------------------- */

bool ui_shared_post_event(const ui_input_event_t *ev) {
    if (ev == NULL || !shared_ready()) {
        return false;
    }
    if (!queue_try_add(&s_in_q, ev)) {
        (void)__atomic_fetch_add(&s_dropped_inputs, 1u, __ATOMIC_RELAXED);
        return false;
    }
    return true;
}

bool ui_shared_pop_input(ui_input_event_t *ev) {
    if (ev == NULL || !shared_ready()) {
        return false;
    }
    return queue_try_remove(&s_in_q, ev);
}

uint32_t ui_shared_dropped_inputs(void) {
    return __atomic_load_n(&s_dropped_inputs, __ATOMIC_RELAXED);
}

void ui_shared_reset_dropped_inputs(void) {
    __atomic_store_n(&s_dropped_inputs, 0u, __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------------------- */
/* Command queue                                                             */
/* ------------------------------------------------------------------------- */

bool ui_shared_post_cmd(const ui_command_t *cmd) {
    if (cmd == NULL || !shared_ready()) {
        return false;
    }
    critical_section_enter_blocking(&s_cmd_cs);
    bool ok = queue_try_add(&s_cmd_q, cmd);
    if (!ok) {
        /* Full: drain, drop the oldest coalescable entry, refill, append. */
        ui_command_t tmp[UI_CMD_QUEUE_LEN];
        size_t n = 0;
        while (n < UI_CMD_QUEUE_LEN && queue_try_remove(&s_cmd_q, &tmp[n])) {
            n++;
        }
        size_t drop = n;
        for (size_t i = 0; i < n; i++) {
            if (tmp[i].type == (uint8_t)UI_CMD_SEEK || tmp[i].type == (uint8_t)UI_CMD_SET_VOLUME) {
                drop = i;
                break;
            }
        }
        size_t first = 0;
        if (drop < n) {
            for (size_t i = drop; i + 1u < n; i++) {
                tmp[i] = tmp[i + 1u];
            }
            n--;
        } else if (n == UI_CMD_QUEUE_LEN) {
            /* Nothing coalescable: drop the oldest command instead. */
            first = 1;
        }
        for (size_t i = first; i < n; i++) {
            (void)queue_try_add(&s_cmd_q, &tmp[i]);
        }
        ok = queue_try_add(&s_cmd_q, cmd);
        if (!ok) {
            s_cmd_dropped++;
        }
    }
    critical_section_exit(&s_cmd_cs);
    return ok;
}

void ui_shared_post_simple(ui_cmd_type_t type, int32_t value) {
    ui_command_t cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.type = (uint8_t)type;
    cmd.value = value;
    (void)ui_shared_post_cmd(&cmd);
}

bool ui_poll_command(ui_command_t *out) {
    if (out == NULL || !shared_ready()) {
        return false;
    }
    critical_section_enter_blocking(&s_cmd_cs);
    const bool ok = queue_try_remove(&s_cmd_q, out);
    critical_section_exit(&s_cmd_cs);
    return ok;
}

/* ------------------------------------------------------------------------- */
/* Now-playing state                                                         */
/* ------------------------------------------------------------------------- */

void ui_shared_snapshot(ui_np_t *out) {
    if (out == NULL) {
        return;
    }
    if (!shared_ready()) {
        memset(out, 0, sizeof *out);
        out->battery = 100u;
        return;
    }
    critical_section_enter_blocking(&s_np_cs);
    *out = s_np_state;
    critical_section_exit(&s_np_cs);
}

/**
 * @brief Replace the current track (copied).
 * @param track Track metadata; NULL is ignored.
 */
void ui_set_now_playing(const ui_track_info_t *track) {
    if (track == NULL || !shared_ready()) {
        return;
    }
    critical_section_enter_blocking(&s_np_cs);
    s_np_state.track = *track;
    copy_text(s_np_state.track.title, UI_TITLE_MAX, track->title);
    copy_text(s_np_state.track.artist, UI_ARTIST_MAX, track->artist);
    copy_text(s_np_state.track.album, UI_ALBUM_MAX, track->album);
    s_np_state.has_track = true;
    s_np_state.position_ms = 0;
    s_np_state.track_seq++;
    s_np_state.seq++;
    critical_section_exit(&s_np_cs);
}

/** @brief Clear the current track (the screen shows "Nothing playing"). */
void ui_clear_now_playing(void) {
    if (!shared_ready()) {
        return;
    }
    critical_section_enter_blocking(&s_np_cs);
    s_np_state.has_track = false;
    s_np_state.play = UI_PLAY_STOPPED;
    s_np_state.position_ms = 0;
    s_np_state.track_seq++;
    s_np_state.seq++;
    critical_section_exit(&s_np_cs);
}

/** @brief Report the playback state. */
void ui_set_play_state(ui_play_state_t state) {
    if (!shared_ready()) {
        return;
    }
    critical_section_enter_blocking(&s_np_cs);
    if (s_np_state.play != state) {
        s_np_state.play = state;
        s_np_state.seq++;
    }
    critical_section_exit(&s_np_cs);
}

/** @brief Report the playback position in milliseconds. */
void ui_set_position(uint32_t position_ms) {
    if (!shared_ready()) {
        return;
    }
    critical_section_enter_blocking(&s_np_cs);
    if (s_np_state.position_ms != position_ms) {
        s_np_state.position_ms = position_ms;
        s_np_state.seq++;
    }
    critical_section_exit(&s_np_cs);
}

/** @brief Report the current volume (0..100). */
void ui_set_volume(uint8_t volume) {
    if (!shared_ready()) {
        return;
    }
    if (volume > 100u) {
        volume = 100u;
    }
    critical_section_enter_blocking(&s_np_cs);
    if (s_np_state.volume != volume) {
        s_np_state.volume = volume;
        s_np_state.seq++;
    }
    critical_section_exit(&s_np_cs);
}

/** @brief Report the battery level and charging flag. */
void ui_set_battery(uint8_t percent, bool charging) {
    if (!shared_ready()) {
        return;
    }
    if (percent > 100u) {
        percent = 100u;
    }
    critical_section_enter_blocking(&s_np_cs);
    if (s_np_state.battery != percent || s_np_state.charging != charging) {
        s_np_state.battery = percent;
        s_np_state.charging = charging;
        s_np_state.seq++;
    }
    critical_section_exit(&s_np_cs);
}

/** @brief Report the shuffle and repeat modes shown in the status bar. */
void ui_set_modes(bool shuffle, bool repeat) {
    if (!shared_ready()) {
        return;
    }
    critical_section_enter_blocking(&s_np_cs);
    if (s_np_state.shuffle != shuffle || s_np_state.repeat != repeat) {
        s_np_state.shuffle = shuffle;
        s_np_state.repeat = repeat;
        s_np_state.seq++;
    }
    critical_section_exit(&s_np_cs);
}
