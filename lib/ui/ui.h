/**
 * @file ui.h
 * @brief Public API of the iPod-style user interface.
 *
 * Threading model
 * ---------------
 * The UI owns the display and runs on its own core (core 1 by convention):
 * call ui_init() and then ui_run() there, or call ui_task() from your own
 * loop. Every other function in this header is **safe to call from any core
 * and from interrupt context**, unless its comment says otherwise. Each one
 * copies its data or posts it to a lock-free/spin-locked queue and returns
 * without blocking on the display.
 *
 * Data flow
 *   input adapters   --ui_post_input()------>  UI   (wheel ticks, buttons)
 *   audio/app core   --ui_set_*()----------->  UI   (now-playing state)
 *   UI               --ui_poll_command()---->  audio/app core (play, seek, volume...)
 *   UI               --ui_library_t calls--->  library provider (runs on the UI core)
 */
#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stdint.h>

#include "gfx_image.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Input events (hardware-independent)                                       */
/* ------------------------------------------------------------------------- */

/** Logical buttons. Adapters map their physical inputs onto these. */
typedef enum {
    UI_BTN_SELECT = 0,  /**< centre button */
    UI_BTN_MENU,        /**< back / up one level (top of wheel) */
    UI_BTN_PLAY_PAUSE,  /**< bottom of wheel */
    UI_BTN_NEXT,        /**< right of wheel (hold = fast-forward) */
    UI_BTN_PREV,        /**< left of wheel (hold = rewind) */
    UI_BTN_COUNT
} ui_button_t;

/** Input event types. Adapters only ever post WHEEL, BUTTON_DOWN and
 *  BUTTON_UP; the UI itself derives clicks and long-presses from timing. */
typedef enum {
    UI_INPUT_WHEEL = 0,     /**< delta = signed ticks, +ve = clockwise */
    UI_INPUT_BUTTON_DOWN,   /**< button = ui_button_t */
    UI_INPUT_BUTTON_UP,     /**< button = ui_button_t */
} ui_input_type_t;

/** One input event. One wheel tick moves the menu selection by one row. */
typedef struct {
    uint8_t  type;     /**< ui_input_type_t */
    uint8_t  button;   /**< ui_button_t (button events) */
    int16_t  delta;    /**< wheel ticks (wheel events) */
} ui_input_event_t;

/** Post an input event. Non-blocking; IRQ/any-core safe.
 *  @return false if the input queue (32 entries) is full and the event was dropped. */
bool ui_post_input(const ui_input_event_t *ev);

/** Convenience: post a wheel movement of @p ticks (clockwise positive). */
bool ui_post_wheel(int16_t ticks);

/** Convenience: post a button press (@p down = true) or release. */
bool ui_post_button(ui_button_t button, bool down);

/* ------------------------------------------------------------------------- */
/* Now-playing state (audio core -> UI)                                      */
/* ------------------------------------------------------------------------- */

#define UI_TITLE_MAX  64   /**< bytes incl. NUL, UTF-8 truncated to ASCII range 0x20-0x7E for display */
#define UI_ARTIST_MAX 48
#define UI_ALBUM_MAX  48

typedef enum { UI_PLAY_STOPPED = 0, UI_PLAY_PLAYING, UI_PLAY_PAUSED } ui_play_state_t;

/** Track metadata shown on the Now Playing screen. */
typedef struct {
    uint32_t track_id;            /**< library id, passed back in commands */
    uint32_t album_id;            /**< used to fetch album art through ui_library_t */
    char     title[UI_TITLE_MAX];
    char     artist[UI_ARTIST_MAX];
    char     album[UI_ALBUM_MAX];
    uint32_t duration_ms;
    uint16_t queue_index;         /**< 1-based position in the play queue ("3 of 12") */
    uint16_t queue_length;
} ui_track_info_t;

/** Replace the current track (copied). Call when a new track starts. */
void ui_set_now_playing(const ui_track_info_t *track);
/** Clear the current track (nothing playing). */
void ui_clear_now_playing(void);
/** Report playback state. */
void ui_set_play_state(ui_play_state_t state);
/** Report playback position in ms. Cheap; call as often as you like (the UI redraws at most at its frame rate). */
void ui_set_position(uint32_t position_ms);
/** Report the current volume 0..100 (for example after the audio side clamps it). */
void ui_set_volume(uint8_t volume);
/** Report battery level 0..100 and charging flag (status bar icon). */
void ui_set_battery(uint8_t percent, bool charging);
/** Report shuffle/repeat modes shown in the status bar. */
void ui_set_modes(bool shuffle, bool repeat);

/* ------------------------------------------------------------------------- */
/* Commands (UI -> audio/app core)                                           */
/* ------------------------------------------------------------------------- */

typedef enum {
    UI_CMD_PLAY_LIST = 0,   /**< start playing @c list/@c parent_id at @c index (user selected a song) */
    UI_CMD_TOGGLE_PAUSE,
    UI_CMD_NEXT,
    UI_CMD_PREV,            /**< audio side decides "restart track" vs "previous track" */
    UI_CMD_SEEK,            /**< value = absolute position in ms */
    UI_CMD_SET_VOLUME,      /**< value = 0..100 */
    UI_CMD_SET_SHUFFLE,     /**< value = 0/1 */
    UI_CMD_SET_REPEAT,      /**< value = 0/1 */
    UI_CMD_SHUFFLE_ALL,     /**< "Shuffle Songs" menu item */
} ui_cmd_type_t;

/** Library list kinds (also used by ui_library_t). */
typedef enum {
    UI_LIST_ARTISTS = 0,    /**< parent_id ignored */
    UI_LIST_ALBUMS,         /**< parent_id = artist id, or UI_ID_ALL */
    UI_LIST_SONGS,          /**< parent_id = album id, or UI_ID_ALL */
    UI_LIST_PLAYLISTS,      /**< parent_id ignored */
    UI_LIST_PLAYLIST_SONGS, /**< parent_id = playlist id */
} ui_list_kind_t;

#define UI_ID_ALL 0xFFFFFFFFu

typedef struct {
    uint8_t  type;       /**< ui_cmd_type_t */
    uint8_t  list;       /**< ui_list_kind_t (PLAY_LIST) */
    uint16_t index;      /**< item index within the list (PLAY_LIST) */
    uint32_t parent_id;  /**< list parent (PLAY_LIST) */
    uint32_t item_id;    /**< id of the selected item (PLAY_LIST) */
    int32_t  value;      /**< SEEK / SET_VOLUME / SET_SHUFFLE / SET_REPEAT */
} ui_command_t;

/** Fetch the next command produced by the UI. Non-blocking; any core.
 *  @return true if @p out was filled. Queue holds 16 commands; when full the
 *          oldest SEEK/SET_VOLUME is coalesced so user actions are never lost. */
bool ui_poll_command(ui_command_t *out);

/* ------------------------------------------------------------------------- */
/* Music library provider (implemented by the library/SD teammate)           */
/* ------------------------------------------------------------------------- */

/** One row of a list, filled in by the provider (strings are copied by the UI). */
typedef struct {
    uint32_t id;
    char     title[UI_TITLE_MAX];
    char     subtitle[UI_ARTIST_MAX];   /**< optional second line / right label, may be "" */
} ui_list_item_t;

/**
 * Library provider callbacks. They are called **on the UI core** and only for
 * rows that are about to become visible (about 10 at a time), so they must be fast
 * (serve from an in-RAM index; do not block on SD I/O from the UI core).
 */
typedef struct {
    /** Number of items in a list. */
    uint32_t (*count)(ui_list_kind_t list, uint32_t parent_id, void *ctx);
    /** Fill @p out for item @p index. @return false if the index is invalid. */
    bool (*get_item)(ui_list_kind_t list, uint32_t parent_id, uint32_t index, ui_list_item_t *out, void *ctx);
    /** Album art (RGB565, ideally 120x120; other sizes are scaled) or NULL for the placeholder.
     *  The returned image must stay valid until the next call with a different album_id. */
    const gfx_image_t *(*get_album_art)(uint32_t album_id, void *ctx);
    void *ctx;
} ui_library_t;

/* ------------------------------------------------------------------------- */
/* Navigation (any core)                                                     */
/* ------------------------------------------------------------------------- */

void ui_go_home(void);          /**< pop to the main menu */
void ui_go_back(void);          /**< same as the MENU button */
void ui_show_now_playing(void); /**< push the Now Playing screen */

/* ------------------------------------------------------------------------- */
/* Lifecycle and diagnostics (UI core only)                                  */
/* ------------------------------------------------------------------------- */

typedef struct {
    const ui_library_t *library;  /**< required */
    uint8_t  target_fps;          /**< 0 = default 30 */
    uint8_t  initial_volume;      /**< 0..100 */
    uint16_t long_press_ms;       /**< 0 = default 600 */
} ui_config_t;

/** Initialise UI state and the renderer and draw the main menu. The display
 *  must already be initialised (ili9341_init). UI core only. */
void ui_init(const ui_config_t *cfg);

/** Run one UI iteration: drain input, step animations, render dirty regions.
 *  Returns without sleeping. UI core only. */
void ui_task(void);

/** Run ui_task() forever, paced to target_fps with sleep between frames.
 *  Never returns. Suitable as a multicore_launch_core1 entry after ui_init(). */
void ui_run(void) __attribute__((noreturn));

/** Performance counters, accumulated since the last ui_get_perf(..., true). */
typedef struct {
    uint32_t frames;            /**< frames that rendered something */
    uint32_t idle_frames;       /**< frame slots with nothing to draw */
    uint32_t avg_frame_us;      /**< mean CPU time per rendered frame (input+anim+render) */
    uint32_t max_frame_us;
    uint32_t avg_pixels;        /**< pixels pushed per rendered frame */
    uint32_t dma_wait_us;       /**< total time the CPU spent waiting on display DMA */
    uint32_t cpu_load_pct;      /**< UI core busy time / wall time */
    uint32_t elapsed_ms;
    uint32_t dropped_inputs;
} ui_perf_t;

/** Read (and optionally reset) performance counters. UI core only. */
void ui_get_perf(ui_perf_t *out, bool reset);

#ifdef __cplusplus
}
#endif

#endif /* UI_H */
