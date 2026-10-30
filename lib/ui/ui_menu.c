/**
 * @file ui_menu.c
 * @brief Generic menu/list screen for the static menus and the library lists.
 *
 * One implementation drives the root "PiPod" menu, the Music sub-menu, the
 * Settings screen and every virtualised library list (Artists, Albums, Songs,
 * Playlists). Rows come from a static table or from the ui_library_t provider
 * through a small ring cache keyed by (list, parent, index). Only the visible
 * window is ever requested, per docs/ui_spec.md.
 *
 * Selection and scrolling are pixel animations driven by time deltas; the only
 * rectangles invalidated are the union of the old and new highlight and, while
 * the list moves, the content area.
 */

#include "ui_internal.h"

#include <math.h>
#include <string.h>

#include "fonts.h"
#include "ui_anim.h"
#include "ui_widgets.h"

/* ------------------------------------------------------------------------- */
/* Static menu tables                                                        */
/* ------------------------------------------------------------------------- */

static const char *const s_music_rows[] = {"Playlists", "Artists", "Albums", "Songs"};

/* ------------------------------------------------------------------------- */
/* Row cache                                                                 */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint32_t parent;
    uint32_t index;
    uint8_t list;
    uint8_t used;
    ui_list_item_t item;
} cache_entry_t;

static cache_entry_t s_cache[UI_CACHE_ROWS];
static uint8_t s_cache_next;

static const ui_list_item_t *cache_lookup(ui_list_kind_t list, uint32_t parent, uint32_t index) {
    for (size_t i = 0; i < UI_CACHE_ROWS; i++) {
        if (s_cache[i].used != 0u && s_cache[i].list == (uint8_t)list &&
            s_cache[i].parent == parent && s_cache[i].index == index) {
            return &s_cache[i].item;
        }
    }
    return NULL;
}

static const ui_list_item_t *cache_fetch(ui_list_kind_t list, uint32_t parent, uint32_t index) {
    const ui_list_item_t *hit = cache_lookup(list, parent, index);
    if (hit != NULL) {
        return hit;
    }
    cache_entry_t *e = &s_cache[s_cache_next];
    s_cache_next = (uint8_t)((s_cache_next + 1u) % UI_CACHE_ROWS);
    memset(e, 0, sizeof *e);
    e->list = (uint8_t)list;
    e->parent = parent;
    e->index = index;
    e->used = 1;

    const ui_library_t *lib = ui_library();
    bool ok = false;
    if (lib != NULL && lib->get_item != NULL) {
        ok = lib->get_item(list, parent, index, &e->item, lib->ctx);
    }
    if (!ok) {
        e->item.id = 0;
        e->item.title[0] = '.';
        e->item.title[1] = '.';
        e->item.title[2] = '.';
        e->item.title[3] = '\0';
        e->item.subtitle[0] = '\0';
    }
    /* The provider is not required to terminate; force it. */
    e->item.title[UI_TITLE_MAX - 1] = '\0';
    e->item.subtitle[UI_ARTIST_MAX - 1] = '\0';
    return &e->item;
}

/* ------------------------------------------------------------------------- */
/* Row model                                                                 */
/* ------------------------------------------------------------------------- */

static gfx_rect_t content_rect(void) {
    gfx_rect_t r = {0, UI_CONTENT_Y, UI_SCREEN_W, UI_CONTENT_H};
    return r;
}

static uint32_t menu_row_count(const ui_np_t *np, const ui_screen_t *s) {
    switch (s->kind) {
    case UI_SCR_MAIN:
        return np->has_track ? 4u : 3u;
    case UI_SCR_MUSIC:
        return (uint32_t)(sizeof s_music_rows / sizeof s_music_rows[0]);
    case UI_SCR_SETTINGS:
        return ui_settings_count();
    case UI_SCR_LIST:
        return s->count;
    default:
        return 0u;
    }
}

uint32_t ui_menu_count(const ui_np_t *np, const ui_screen_t *s) {
    if (np == NULL || s == NULL) {
        return 0u;
    }
    return menu_row_count(np, s);
}

static void menu_row_view(const ui_np_t *np, const ui_screen_t *s, uint32_t idx, ui_row_t *out) {
    out->title = "";
    out->subtitle = NULL;
    out->flags = 0;

    switch (s->kind) {
    case UI_SCR_MAIN:
        if (idx == 0u) {
            out->title = "Music";
            out->flags = UI_ROW_CHEVRON;
        } else if (np->has_track && idx == 1u) {
            out->title = "Now Playing";
            out->flags = UI_ROW_CHEVRON;
        } else if ((np->has_track && idx == 2u) || (!np->has_track && idx == 1u)) {
            out->title = "Shuffle Songs";
        } else {
            out->title = "Settings";
            out->flags = UI_ROW_CHEVRON;
        }
        break;

    case UI_SCR_MUSIC:
        if (idx < (uint32_t)(sizeof s_music_rows / sizeof s_music_rows[0])) {
            out->title = s_music_rows[idx];
            out->flags = UI_ROW_CHEVRON;
        }
        break;

    case UI_SCR_SETTINGS:
        ui_settings_row(idx, out);
        break;

    case UI_SCR_LIST: {
        const ui_list_item_t *it =
            cache_fetch((ui_list_kind_t)s->list, s->parent_id, idx);
        out->title = it->title;
        if (it->subtitle[0] != '\0') {
            out->subtitle = it->subtitle;
        }
        break;
    }

    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Screen lifecycle                                                          */
/* ------------------------------------------------------------------------- */

void ui_menu_screen_reset(ui_screen_t *s) {
    s->hl_y = 0.0f;
    s->scroll_px = 0.0f;
    s->hl_drawn = UI_LIST_Y;
    s->scroll_drawn = 0;
    s->fetch_valid = 0;
    s->fetch_top = (int16_t)-1;
}

void ui_menu_screen_init(ui_screen_t *s, ui_screen_kind_t kind, ui_list_kind_t list,
                         uint32_t parent_id) {
    memset(s, 0, sizeof *s);
    s->kind = (uint8_t)kind;
    s->list = (uint8_t)list;
    s->parent_id = parent_id;
    ui_menu_screen_reset(s);
    if (kind == UI_SCR_LIST) {
        const ui_library_t *lib = ui_library();
        if (lib != NULL && lib->count != NULL) {
            s->count = lib->count(list, parent_id, lib->ctx);
        }
    }
}

void ui_menu_prefetch(const ui_np_t *np, ui_screen_t *s) {
    (void)np;
    if (s->kind != UI_SCR_LIST || s->count == 0u) {
        return;
    }
    int top = (int)(s->scroll_px / (float)UI_ROW_H);
    if (top < 0) {
        top = 0;
    }
    if (s->fetch_valid != 0u && top == (int)s->fetch_top) {
        return;
    }
    s->fetch_valid = 1;
    s->fetch_top = (int16_t)top;

    const int first = top - 1;
    const int last = top + UI_ROWS_VISIBLE;
    for (int i = first; i <= last; i++) {
        if (i < 0 || (uint32_t)i >= s->count) {
            continue;
        }
        (void)cache_fetch((ui_list_kind_t)s->list, s->parent_id, (uint32_t)i);
    }
}

/* ------------------------------------------------------------------------- */
/* Animation                                                                 */
/* ------------------------------------------------------------------------- */

void ui_menu_step(const ui_np_t *np, ui_screen_t *s, float dt_s) {
    if (s->kind == UI_SCR_NOW_PLAYING || s->kind == UI_SCR_ABOUT) {
        return;
    }

    const uint32_t count = menu_row_count(np, s);
    if (count == 0u) {
        s->selected = 0;
        s->scroll = 0;
    } else {
        if ((uint32_t)s->selected >= count) {
            s->selected = (uint16_t)(count - 1u);
        }
        if (count <= (uint32_t)UI_ROWS_VISIBLE) {
            s->scroll = 0;
        } else {
            const int max_top = (int)count - UI_ROWS_VISIBLE;
            const int sel = (int)s->selected;
            if (sel < (int)s->scroll) {
                s->scroll = (int16_t)sel;
            } else if (sel >= (int)s->scroll + UI_ROWS_VISIBLE) {
                s->scroll = (int16_t)(sel - UI_ROWS_VISIBLE + 1);
            }
            if ((int)s->scroll > max_top) {
                s->scroll = (int16_t)max_top;
            }
            if (s->scroll < 0) {
                s->scroll = 0;
            }
        }
    }

    const float k = ui_anim_factor(dt_s, 0.030f);
    const float scroll_target = (float)((int)s->scroll * UI_ROW_H);
    const float hl_target = (float)s->selected * (float)UI_ROW_H;
    s->scroll_px += (scroll_target - s->scroll_px) * k;
    s->hl_y += (hl_target - s->hl_y) * k;
    if (ui_anim_close(s->scroll_px, scroll_target, 0.15f)) {
        s->scroll_px = scroll_target;
    }
    if (ui_anim_close(s->hl_y, hl_target, 0.15f)) {
        s->hl_y = hl_target;
    }

    ui_menu_prefetch(np, s);

    const int scroll_i = (int)(s->scroll_px + 0.5f);
    if (scroll_i != (int)s->scroll_drawn) {
        ui_invalidate(content_rect());
        s->scroll_drawn = (int16_t)scroll_i;
    }

    const int hy = UI_LIST_Y + (int)(s->hl_y + 0.5f) - scroll_i;
    if (hy != (int)s->hl_drawn) {
        gfx_rect_t a = {0, s->hl_drawn, UI_SCREEN_W, UI_ROW_H};
        gfx_rect_t b = {0, (int16_t)hy, UI_SCREEN_W, UI_ROW_H};
        ui_invalidate(gfx_rect_union(a, b));
        s->hl_drawn = (int16_t)hy;
    }
}

/* ------------------------------------------------------------------------- */
/* Input                                                                     */
/* ------------------------------------------------------------------------- */

void ui_menu_wheel(const ui_np_t *np, ui_screen_t *s, int ticks, int step) {
    if (ticks == 0 || s == NULL) {
        return;
    }
    if (s->kind == UI_SCR_SETTINGS && ui_settings_wheel(s->selected, ticks)) {
        ui_menu_invalidate_row(s, s->selected);
        return;
    }

    const uint32_t count = menu_row_count(np, s);
    if (count == 0u) {
        return;
    }
    int move = ticks * step;
    int sel = (int)s->selected + move;
    if (sel < 0) {
        sel = 0;
    }
    if ((uint32_t)sel >= count) {
        sel = (int)count - 1;
    }
    s->selected = (uint16_t)sel;
}

void ui_menu_activate(const ui_np_t *np, ui_screen_t *s) {
    if (np == NULL || s == NULL) {
        return;
    }
    const uint32_t idx = s->selected;

    switch (s->kind) {
    case UI_SCR_MAIN:
        if (np->has_track) {
            if (idx == 0u) {
                (void)ui_nav_push(UI_SCR_MUSIC);
            } else if (idx == 1u) {
                ui_nav_push_now_playing();
            } else if (idx == 2u) {
                ui_shared_post_simple(UI_CMD_SHUFFLE_ALL, 0);
            } else {
                (void)ui_nav_push(UI_SCR_SETTINGS);
            }
        } else {
            if (idx == 0u) {
                (void)ui_nav_push(UI_SCR_MUSIC);
            } else if (idx == 1u) {
                ui_shared_post_simple(UI_CMD_SHUFFLE_ALL, 0);
            } else {
                (void)ui_nav_push(UI_SCR_SETTINGS);
            }
        }
        break;

    case UI_SCR_MUSIC:
        switch (idx) {
        case 0u:
            ui_nav_push_list(UI_LIST_PLAYLISTS, UI_ID_ALL);
            break;
        case 1u:
            ui_nav_push_list(UI_LIST_ARTISTS, UI_ID_ALL);
            break;
        case 2u:
            ui_nav_push_list(UI_LIST_ALBUMS, UI_ID_ALL);
            break;
        case 3u:
            ui_nav_push_list(UI_LIST_SONGS, UI_ID_ALL);
            break;
        default:
            break;
        }
        break;

    case UI_SCR_SETTINGS:
        ui_settings_activate(np, idx);
        ui_menu_invalidate_row(s, idx);
        break;

    case UI_SCR_LIST: {
        if (idx >= menu_row_count(np, s)) {
            break;
        }
        const ui_list_item_t *it = cache_fetch((ui_list_kind_t)s->list, s->parent_id, idx);
        if (it == NULL) {
            break;
        }
        switch ((ui_list_kind_t)s->list) {
        case UI_LIST_ARTISTS:
            ui_nav_push_list(UI_LIST_ALBUMS, it->id);
            break;
        case UI_LIST_ALBUMS:
            ui_nav_push_list(UI_LIST_SONGS, it->id);
            break;
        case UI_LIST_PLAYLISTS:
            ui_nav_push_list(UI_LIST_PLAYLIST_SONGS, it->id);
            break;
        case UI_LIST_SONGS:
        case UI_LIST_PLAYLIST_SONGS: {
            ui_command_t cmd;
            memset(&cmd, 0, sizeof cmd);
            cmd.type = (uint8_t)UI_CMD_PLAY_LIST;
            cmd.list = s->list;
            cmd.index = (uint16_t)idx;
            cmd.parent_id = s->parent_id;
            cmd.item_id = it->id;
            (void)ui_shared_post_cmd(&cmd);
            ui_nav_push_now_playing();
            break;
        }
        default:
            break;
        }
        break;
    }

    default:
        break;
    }
}

void ui_menu_invalidate_row(const ui_screen_t *s, uint32_t index) {
    if (s == NULL) {
        return;
    }
    const int y = UI_LIST_Y + (int)index * UI_ROW_H - (int)(s->scroll_px + 0.5f);
    gfx_rect_t r = {0, (int16_t)y, UI_SCREEN_W, UI_ROW_H};
    ui_invalidate(r);
}

/* ------------------------------------------------------------------------- */
/* Drawing                                                                   */
/* ------------------------------------------------------------------------- */

const char *ui_screen_title(const ui_np_t *np, const ui_screen_t *s) {
    (void)np;
    if (s == NULL) {
        return "";
    }
    switch (s->kind) {
    case UI_SCR_MAIN:
        return "PiPod";
    case UI_SCR_MUSIC:
        return "Music";
    case UI_SCR_SETTINGS:
        return "Settings";
    case UI_SCR_ABOUT:
        return "About";
    case UI_SCR_NOW_PLAYING:
        return "Now Playing";
    case UI_SCR_LIST:
        switch ((ui_list_kind_t)s->list) {
        case UI_LIST_ARTISTS:
            return "Artists";
        case UI_LIST_ALBUMS:
            return "Albums";
        case UI_LIST_SONGS:
            return "Songs";
        case UI_LIST_PLAYLISTS:
            return "Playlists";
        case UI_LIST_PLAYLIST_SONGS:
            return "Playlist";
        default:
            return "List";
        }
    default:
        return "";
    }
}

void ui_menu_draw(gfx_canvas_t *c, const ui_np_t *np, const ui_screen_t *s, int x_off) {
    if (c == NULL || np == NULL || s == NULL) {
        return;
    }
    if (s->kind == UI_SCR_NOW_PLAYING || s->kind == UI_SCR_ABOUT) {
        return;
    }
    if (!ui_visible(c, x_off, UI_CONTENT_Y, UI_SCREEN_W, UI_CONTENT_H)) {
        return;
    }

    const uint32_t count = menu_row_count(np, s);
    const int scroll = (int)(s->scroll_px + 0.5f);

    gfx_fill_rect(c, x_off, UI_CONTENT_Y, UI_SCREEN_W, UI_CONTENT_H, UI_COL_BG);

    const int hl_y = UI_LIST_Y + (int)(s->hl_y + 0.5f) - scroll;
    if ((uint32_t)s->selected < count) {
        ui_draw_selection(c, x_off, hl_y, UI_SCREEN_W, UI_ROW_H);
    }

    const int first = scroll / UI_ROW_H;
    for (int i = first - 1; i <= first + UI_ROWS_VISIBLE + 1; i++) {
        if (i < 0 || (uint32_t)i >= count) {
            continue;
        }
        const int y = UI_LIST_Y + i * UI_ROW_H - scroll;
        if (!ui_visible(c, x_off, y, UI_SCREEN_W, UI_ROW_H)) {
            continue;
        }

        ui_row_t row;
        menu_row_view(np, s, (uint32_t)i, &row);
        const bool sel = ((uint32_t)i == s->selected);
        const gfx_color_t title_col = sel ? UI_COL_SEL_TEXT : UI_COL_TEXT;

        int text_right = (row.flags & UI_ROW_CHEVRON) ? 292 : 306;

        if (row.subtitle != NULL) {
            char sub_buf[UI_TITLE_MAX];
            ui_ascii(row.subtitle, sub_buf, sizeof sub_buf);
            const int sub_w = gfx_text_width(&font_inter_12, sub_buf);
            ui_text_right(c, &font_inter_12, text_right + x_off, y + 19, 120, sub_buf,
                          sel ? UI_COL_SEL_TEXT_2 : UI_COL_TEXT_2);
            text_right -= sub_w + 8;
        }

        ui_text(c, &font_inter_16sb, 12 + x_off, y + 20, text_right - 12, row.title, title_col);

        if ((row.flags & UI_ROW_CHEVRON) != 0u) {
            ui_draw_glyph_chevron(c, 300 + x_off, y + 8, sel ? UI_COL_SEL_TEXT : UI_COL_CHEVRON);
        }
    }

    ui_draw_scrollbar(c, UI_LIST_Y, UI_LIST_H, (uint32_t)(s->scroll < 0 ? 0 : s->scroll),
                      UI_ROWS_VISIBLE, count, x_off);
}
