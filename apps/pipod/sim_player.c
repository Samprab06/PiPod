/**
 * @file sim_player.c
 * @brief Command-driven fake playback engine with a generated queue.
 */

#include "sim_player.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "pico/time.h"
#include "ui.h"

#include "sim_library.h"

#define QUEUE_MAX            192
#define POS_UPDATE_US        100000u  /**< report position to the UI at 10 Hz */
#define RESTART_THRESHOLD_MS 3000u    /**< PREV inside a track restarts it */
#define BATTERY_DRAIN_MS     15000u   /**< 1 % per 15 s of demo runtime */

static uint32_t s_queue[QUEUE_MAX];
static uint16_t s_queue_len;
static uint16_t s_queue_pos;

static bool            s_has_track;
static ui_play_state_t s_state;
static uint32_t        s_pos_ms;
static uint32_t        s_duration_ms;
static uint32_t        s_frac_us;
static uint64_t        s_last_update_us;
static uint64_t        s_last_pos_us;
static uint64_t        s_start_us;

static bool    s_shuffle;
static bool    s_repeat;
static uint8_t s_volume = SIM_DEMO_INITIAL_VOLUME;
static uint8_t s_battery = 100;
static uint32_t s_rng = 0x1BADF00Du;

/* --- helpers ----------------------------------------------------------- */

static uint32_t rnd(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static void shuffle_range(uint32_t *items, uint16_t n) {
    for (uint16_t i = (n > 1u) ? (uint16_t)(n - 1u) : 0u; i > 0u; i--) {
        uint16_t j = (uint16_t)(rnd() % (uint32_t)(i + 1u));
        uint32_t tmp = items[i];
        items[i] = items[j];
        items[j] = tmp;
    }
}

static void publish_modes(void) {
    ui_set_modes(s_shuffle, s_repeat);
}

static void play_index(uint16_t index) {
    if (index >= s_queue_len) {
        return;
    }
    ui_track_info_t track;
    if (!sim_song_info(s_queue[index], &track)) {
        return;
    }
    track.queue_index = (uint16_t)(index + 1u);
    track.queue_length = s_queue_len;

    s_queue_pos = index;
    s_has_track = true;
    s_pos_ms = 0;
    s_frac_us = 0;
    s_duration_ms = track.duration_ms;

    ui_set_now_playing(&track);
    s_state = UI_PLAY_PLAYING;
    ui_set_play_state(s_state);
    ui_set_position(0);
    s_last_pos_us = time_us_64();
}

static bool advance_relative(int step, bool wrap) {
    if (s_queue_len == 0u) {
        return false;
    }
    int next = (int)s_queue_pos + step;
    if (next >= (int)s_queue_len) {
        if (!wrap) {
            return false;
        }
        next = 0;
    } else if (next < 0) {
        if (!wrap) {
            return false;
        }
        next = (int)s_queue_len - 1;
    }
    play_index((uint16_t)next);
    return true;
}

static void stop_playback(void) {
    s_state = UI_PLAY_STOPPED;
    s_pos_ms = s_duration_ms;
    ui_set_position(s_pos_ms);
    ui_set_play_state(s_state);
}

/** Build the queue for a PLAY_LIST command. */
static void build_queue(ui_list_kind_t list, uint32_t parent_id, uint32_t selected_index,
                        uint32_t item_id) {
    ui_list_kind_t songs = UI_LIST_SONGS;
    uint32_t parent = parent_id;

    if (list == UI_LIST_PLAYLIST_SONGS) {
        songs = UI_LIST_PLAYLIST_SONGS;
    } else if (list != UI_LIST_SONGS) {
        /* Albums/artists do not map onto a PLAY_LIST in this demo; play all. */
        parent = UI_ID_ALL;
    }

    s_queue_len = 0;
    uint32_t count = sim_song_count(songs, parent);
    if (count > QUEUE_MAX) {
        count = QUEUE_MAX;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t id = sim_song_id_at(songs, parent, i);
        if (id != 0u) {
            s_queue[s_queue_len++] = id;
        }
    }

    /* Find the selected song; fall back to the row index. */
    uint16_t start = 0;
    if (item_id != 0u) {
        for (uint16_t i = 0; i < s_queue_len; i++) {
            if (s_queue[i] == item_id) {
                start = i;
                break;
            }
        }
    } else if (selected_index < s_queue_len) {
        start = (uint16_t)selected_index;
    }

    if (s_shuffle && s_queue_len > 1u) {
        /* Keep the chosen track, shuffle the rest. */
        uint32_t chosen = s_queue[start];
        s_queue[start] = s_queue[0];
        s_queue[0] = chosen;
        shuffle_range(&s_queue[1], (uint16_t)(s_queue_len - 1u));
        start = 0;
    }

    play_index(start);
}

static void shuffle_all(void) {
    s_queue_len = 0;
    for (uint32_t i = 0; i < SIM_SONG_COUNT && i < QUEUE_MAX; i++) {
        s_queue[s_queue_len++] = SIM_FIRST_SONG_ID + i;
    }
    shuffle_range(s_queue, s_queue_len);
    play_index(0);
}

static void restart_track(void) {
    if (!s_has_track) {
        return;
    }
    s_pos_ms = 0;
    s_frac_us = 0;
    ui_set_position(0);
}

static void handle_command(const ui_command_t *cmd) {
    switch (cmd->type) {
        case UI_CMD_PLAY_LIST:
            build_queue((ui_list_kind_t)cmd->list, cmd->parent_id, cmd->index, cmd->item_id);
            break;

        case UI_CMD_TOGGLE_PAUSE:
            if (!s_has_track) {
                break;
            }
            if (s_state == UI_PLAY_PLAYING) {
                s_state = UI_PLAY_PAUSED;
            } else {
                s_state = UI_PLAY_PLAYING;
            }
            ui_set_play_state(s_state);
            ui_set_position(s_pos_ms);
            break;

        case UI_CMD_NEXT:
            if (!advance_relative(1, true)) {
                stop_playback();
            }
            break;

        case UI_CMD_PREV:
            if (s_has_track && s_pos_ms > RESTART_THRESHOLD_MS) {
                restart_track();
            } else if (!advance_relative(-1, true)) {
                restart_track();
            }
            break;

        case UI_CMD_SEEK:
            if (s_has_track) {
                int32_t v = cmd->value;
                if (v < 0) {
                    v = 0;
                }
                if ((uint32_t)v > s_duration_ms) {
                    v = (int32_t)s_duration_ms;
                }
                s_pos_ms = (uint32_t)v;
                s_frac_us = 0;
                ui_set_position(s_pos_ms);
            }
            break;

        case UI_CMD_SET_VOLUME:
            if (cmd->value < 0) {
                s_volume = 0;
            } else if (cmd->value > 100) {
                s_volume = 100;
            } else {
                s_volume = (uint8_t)cmd->value;
            }
            ui_set_volume(s_volume);
            break;

        case UI_CMD_SET_SHUFFLE:
            s_shuffle = (cmd->value != 0);
            publish_modes();
            break;

        case UI_CMD_SET_REPEAT:
            s_repeat = (cmd->value != 0);
            publish_modes();
            break;

        case UI_CMD_SHUFFLE_ALL:
            shuffle_all();
            break;

        default:
            break;
    }
}

/* --- public API -------------------------------------------------------- */

void sim_player_init(void) {
    s_queue_len = 0;
    s_queue_pos = 0;
    s_has_track = false;
    s_state = UI_PLAY_STOPPED;
    s_pos_ms = 0;
    s_duration_ms = 0;
    s_frac_us = 0;
    s_shuffle = false;
    s_repeat = false;
    s_volume = SIM_DEMO_INITIAL_VOLUME;
    s_battery = 100;
    s_start_us = time_us_64();
    s_last_update_us = s_start_us;
    s_last_pos_us = s_start_us;

    ui_set_play_state(UI_PLAY_STOPPED);
    ui_set_volume(s_volume);
    ui_set_modes(false, false);
    ui_set_battery(s_battery, false);
}

void sim_player_print_status(void) {
    if (!s_has_track || s_queue_len == 0u) {
        printf("[player] stopped  vol %u\n", (unsigned)s_volume);
        return;
    }
    ui_track_info_t t;
    if (!sim_song_info(s_queue[s_queue_pos], &t)) {
        return;
    }
    const char *state = (s_state == UI_PLAY_PLAYING) ? "playing"
                        : (s_state == UI_PLAY_PAUSED) ? "paused"
                                                      : "stopped";
    printf("[player] %s %2u/%u  %s - %s  %lu:%02lu/%lu:%02lu  vol %u\n", state,
           (unsigned)(s_queue_pos + 1u), (unsigned)s_queue_len, t.artist, t.title,
           (unsigned long)(s_pos_ms / 60000u),
           (unsigned long)((s_pos_ms / 1000u) % 60u),
           (unsigned long)(t.duration_ms / 60000u),
           (unsigned long)((t.duration_ms / 1000u) % 60u), (unsigned)s_volume);
}

void sim_player_update(void) {
    ui_command_t cmd;
    while (ui_poll_command(&cmd)) {
        handle_command(&cmd);
    }

    uint64_t now = time_us_64();
    uint64_t delta = now - s_last_update_us;
    s_last_update_us = now;

    if (s_state == UI_PLAY_PLAYING && s_has_track) {
        s_frac_us += (uint32_t)delta;
        uint32_t adv_ms = s_frac_us / 1000u;
        s_frac_us %= 1000u;
        s_pos_ms += adv_ms;

        if (s_pos_ms >= s_duration_ms) {
            if (!advance_relative(1, s_repeat)) {
                stop_playback();
            }
        }
    } else {
        s_frac_us = 0;
    }

    if (now - s_last_pos_us >= POS_UPDATE_US) {
        s_last_pos_us = now;
        if (s_has_track) {
            ui_set_position(s_pos_ms);
        }
        uint32_t up_ms = (uint32_t)((now - s_start_us) / 1000u);
        uint8_t level = 100u;
        uint32_t drained = up_ms / BATTERY_DRAIN_MS;
        if (drained < 100u) {
            level = (uint8_t)(100u - drained);
        }
        if (level < 5u) {
            level = 5u;
        }
        if (level != s_battery) {
            s_battery = level;
            ui_set_battery(s_battery, false);
        }
    }
}
