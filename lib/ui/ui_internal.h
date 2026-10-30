/**
 * @file ui_internal.h
 * @brief Private declarations shared by the lib/ui translation units.
 *
 * Nothing here is part of the public contract; only lib/ui sources include it.
 * Geometry, palette and screen-stack definitions live here so that the
 * renderer, the widgets and the screens agree on one set of constants.
 */

#ifndef UI_INTERNAL_H
#define UI_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfx.h"
#include "gfx_font.h"
#include "gfx_image.h"
#include "gfx_render.h"
#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Panel geometry (landscape 320x240, docs/ui_spec.md)                       */
/* ------------------------------------------------------------------------- */

#define UI_SCREEN_W 320
#define UI_SCREEN_H 240
#define UI_STATUS_H 22
#define UI_CONTENT_Y 22
#define UI_CONTENT_H (UI_SCREEN_H - UI_CONTENT_Y)
#define UI_ROW_H 27
#define UI_ROWS_VISIBLE 8
#define UI_LIST_Y (UI_CONTENT_Y + 2)
#define UI_LIST_H (UI_ROWS_VISIBLE * UI_ROW_H)
#define UI_SCROLLBAR_X 314
#define UI_SCROLLBAR_W 4
#define UI_TITLE_COL_X 148
#define UI_TITLE_COL_W 160

/** Maximum number of stacked screens. */
#define UI_MAX_DEPTH 8

/** Size of the list row cache (docs/ui_spec.md: ring of 10). */
#define UI_CACHE_ROWS 10

/* ------------------------------------------------------------------------- */
/* Private input events                                                      */
/* ------------------------------------------------------------------------- */

/**
 * @brief Event types internal to the UI, posted through the public input queue.
 *
 * The values start above ui_input_type_t so an adapter can never produce one.
 */
enum {
    UI_EV_NAV_HOME = 0x10,       /**< pop to the main menu */
    UI_EV_NAV_BACK = 0x11,       /**< same as a MENU click */
    UI_EV_NAV_NOW_PLAYING = 0x12 /**< push the Now Playing screen */
};

/* ------------------------------------------------------------------------- */
/* Now-playing snapshot                                                      */
/* ------------------------------------------------------------------------- */

/** @brief One consistent copy of the cross-core now-playing state. */
typedef struct {
    ui_track_info_t track; /**< Metadata, valid when has_track is set. */
    bool has_track;        /**< A track is loaded. */
    ui_play_state_t play;  /**< Playback state. */
    uint32_t position_ms;  /**< Playback position. */
    uint8_t volume;        /**< Volume 0..100. */
    uint8_t battery;       /**< Battery 0..100. */
    bool charging;         /**< Charging flag. */
    bool shuffle;          /**< Shuffle mode. */
    bool repeat;           /**< Repeat mode. */
    uint32_t seq;          /**< Increments on every state write. */
    uint32_t track_seq;    /**< Increments when a track is set or cleared. */
} ui_np_t;

/* ------------------------------------------------------------------------- */
/* Screens                                                                   */
/* ------------------------------------------------------------------------- */

/** @brief Screen kinds held in the navigation stack. */
typedef enum {
    UI_SCR_MAIN = 0,   /**< Root "PiPod" menu. */
    UI_SCR_MUSIC,      /**< Music sub-menu. */
    UI_SCR_LIST,       /**< Virtualised library list. */
    UI_SCR_NOW_PLAYING,/**< Now Playing screen. */
    UI_SCR_SETTINGS,   /**< Settings screen. */
    UI_SCR_ABOUT,      /**< Static About screen. */
} ui_screen_kind_t;

/** @brief One navigation stack level (docs/ui_spec.md: kind, parent, selection, scroll). */
typedef struct {
    uint8_t kind;        /**< ui_screen_kind_t. */
    uint8_t list;        /**< ui_list_kind_t for UI_SCR_LIST. */
    uint8_t fetch_valid; /**< fetch_top holds a prefetched window. */
    uint16_t selected;   /**< Selected row index. */
    int16_t scroll;      /**< Target top row. */
    uint32_t parent_id;  /**< Library parent id (UI_ID_ALL when unused). */
    uint32_t count;      /**< Row count, refreshed when the screen is pushed. */
    float hl_y;          /**< Animated highlight top in row space (px). */
    float scroll_px;     /**< Animated scroll offset (px). */
    int16_t hl_drawn;    /**< Highlight y last drawn (screen space). */
    int16_t scroll_drawn;/**< Scroll offset last drawn (px). */
    int16_t fetch_top;   /**< Top row of the last prefetch. */
} ui_screen_t;

/** @brief One drawable menu row (pointers are valid for the duration of the call). */
typedef struct {
    const char *title;    /**< Primary label; never NULL. */
    const char *subtitle; /**< Optional right-aligned label; may be NULL. */
    uint8_t flags;        /**< ui_row_flag_t bits. */
} ui_row_t;

/** @brief ui_row_t::flags bits. */
enum {
    UI_ROW_CHEVRON = 1u << 0, /**< Draw a ">" marker (row opens a submenu). */
};

/* ------------------------------------------------------------------------- */
/* Status bar description                                                    */
/* ------------------------------------------------------------------------- */

/** @brief Everything the status bar needs for one draw. */
typedef struct {
    const char *title;    /**< Centred screen title. */
    ui_play_state_t play; /**< Play/pause glyph state. */
    bool has_track;       /**< False draws nothing at the left. */
    uint8_t battery;      /**< 0..100. */
    bool charging;        /**< Draw a charge bolt. */
    bool shuffle;         /**< Shuffle mode active. */
    bool repeat;          /**< Repeat mode active. */
    bool perf;            /**< Draw the FPS/CPU overlay. */
    uint16_t fps;         /**< Measured rendered frames per second. */
    uint8_t cpu;          /**< UI core load, percent. */
} ui_status_t;

/* ------------------------------------------------------------------------- */
/* ui_core.c - navigation and shared services                                */
/* ------------------------------------------------------------------------- */

/** @brief Library provider registered with ui_init(), or NULL before init. */
const ui_library_t *ui_library(void);

/** @brief ui_config_t.long_press_ms resolved to its default (ms). */
uint16_t ui_long_press_ms(void);

/** @brief Invalidate a rectangle on the panel (thin wrapper over gfx_invalidate). */
void ui_invalidate(gfx_rect_t r);

/** @brief Push a prepared screen, returning its level or NULL when the stack is full. */
ui_screen_t *ui_nav_push(ui_screen_kind_t kind);

/** @brief Push a library list screen. */
void ui_nav_push_list(ui_list_kind_t list, uint32_t parent_id);

/** @brief Push the Now Playing screen unless it is already on top. */
void ui_nav_push_now_playing(void);

/** @brief Pop one level (MENU click). No-op at the root. */
void ui_nav_back(void);

/** @brief Pop to the root screen (MENU long-press). */
void ui_nav_home(void);

/* ------------------------------------------------------------------------- */
/* ui_shared.c - cross-core state                                            */
/* ------------------------------------------------------------------------- */

/** @brief Initialise queues and now-playing state. Called once from ui_init(). */
void ui_shared_init(uint8_t initial_volume);

/** @brief Copy a consistent now-playing snapshot. UI core only. */
void ui_shared_snapshot(ui_np_t *out);

/** @brief Pop one input event (including private navigation events). */
bool ui_shared_pop_input(ui_input_event_t *ev);

/** @brief Post an event of any type, including the private UI_EV_* ones. */
bool ui_shared_post_event(const ui_input_event_t *ev);

/** @brief Post one command (coalescing SEEK/SET_VOLUME when the queue is full). */
bool ui_shared_post_cmd(const ui_command_t *cmd);

/** @brief Post a command with only type/value set (zeroes the rest). */
void ui_shared_post_simple(ui_cmd_type_t type, int32_t value);

/** @brief Number of input events dropped because the queue was full. */
uint32_t ui_shared_dropped_inputs(void);

/** @brief Clear the dropped-input counter. */
void ui_shared_reset_dropped_inputs(void);

/* ------------------------------------------------------------------------- */
/* ui_menu.c - generic virtualised list screens                              */
/* ------------------------------------------------------------------------- */

/** @brief Initialise a screen level for a menu or library list. */
void ui_menu_screen_init(ui_screen_t *s, ui_screen_kind_t kind, ui_list_kind_t list,
                         uint32_t parent_id);

/** @brief Number of rows currently in a menu screen. */
uint32_t ui_menu_count(const ui_np_t *np, const ui_screen_t *s);

/** @brief Reset per-screen animation state after a push. */
void ui_menu_screen_reset(ui_screen_t *s);

/** @brief Advance animations and invalidate the affected rectangles. */
void ui_menu_step(const ui_np_t *np, ui_screen_t *s, float dt_s);

/** @brief Draw the screen translated by x_off (transition slide). */
void ui_menu_draw(gfx_canvas_t *c, const ui_np_t *np, const ui_screen_t *s, int x_off);

/** @brief Apply @p ticks wheel ticks (raw, plus the acceleration step for lists). */
void ui_menu_wheel(const ui_np_t *np, ui_screen_t *s, int ticks, int step);

/** @brief Activate the selected row. */
void ui_menu_activate(const ui_np_t *np, ui_screen_t *s);

/** @brief Invalidate the rectangle of one row. */
void ui_menu_invalidate_row(const ui_screen_t *s, uint32_t index);

/** @brief Prefetch the visible window (also called by ui_menu_step). */
void ui_menu_prefetch(const ui_np_t *np, ui_screen_t *s);

/** @brief Status bar title for a screen. */
const char *ui_screen_title(const ui_np_t *np, const ui_screen_t *s);

/* ------------------------------------------------------------------------- */
/* ui_settings.c - settings and about                                        */
/* ------------------------------------------------------------------------- */

/** @brief Reset settings state to its defaults. Called once from ui_init(). */
void ui_settings_init(void);

/** @brief Adopt shuffle/repeat reported by the audio core. */
void ui_settings_sync(const ui_np_t *np);

/** @brief Row count of the settings screen. */
uint32_t ui_settings_count(void);

/** @brief Fill one settings row description. */
void ui_settings_row(uint32_t index, ui_row_t *out);

/** @brief Activate one settings row (SELECT). */
void ui_settings_activate(const ui_np_t *np, uint32_t index);

/** @brief Wheel handled in place (Backlight). @return true when consumed. */
bool ui_settings_wheel(uint32_t selected, int ticks);

/** @brief True when the FPS/CPU status overlay is enabled. */
bool ui_settings_perf_overlay(void);

/** @brief Draw the About screen translated by x_off. */
void ui_settings_about_draw(gfx_canvas_t *c, int x_off);

/* ------------------------------------------------------------------------- */
/* ui_nowplaying.c - Now Playing, volume overlay, scrub mode                 */
/* ------------------------------------------------------------------------- */

/** @brief Reset marquee/progress bookkeeping (track change or push). */
void ui_nowplaying_reset(void);

/** @brief Advance marquee, overlay and scrub state; invalidate what changed. */
void ui_nowplaying_step(const ui_np_t *np, ui_screen_t *s, float dt_s, uint32_t now_us);

/** @brief Draw the Now Playing screen translated by x_off. */
void ui_nowplaying_draw(gfx_canvas_t *c, const ui_np_t *np, const ui_screen_t *s, int x_off);

/** @brief Handle wheel ticks on Now Playing (volume or scrub seek). */
void ui_nowplaying_wheel(const ui_np_t *np, int ticks, uint32_t now_us);

/** @brief SELECT on Now Playing: toggle scrub mode. */
void ui_nowplaying_click(const ui_np_t *np, uint32_t now_us);

#ifdef __cplusplus
}
#endif

#endif /* UI_INTERNAL_H */
