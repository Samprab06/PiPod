/**
 * @file ui_layout.h
 * @brief Every piece of screen geometry and the per-size font roles.
 *
 * Two layouts exist:
 *   - 320x240 for the 2.2" ILI9341 (the original, byte-for-byte unchanged)
 *   - 480x320 for the 3.5" ST7796 (larger rows, status bar, art and fonts)
 *
 * The panel size comes from lib/gfx/gfx_config.h (GFX_SCREEN_W /
 * GFX_SCREEN_H, selected by PIPOD_DISPLAY_ST7796). If a build does not provide
 * that header, the fallback below derives the same values from
 * PIPOD_DISPLAY_ST7796 so lib/ui can still be built stand-alone:
 *
 *   PIPOD_DISPLAY_ST7796=1 -> 480x320 landscape (ST7796)
 *   otherwise              -> 320x240 landscape (ILI9341, the default)
 *
 * Only lib/ui sources include this file; it is not part of the public API.
 */

#ifndef UI_LAYOUT_H
#define UI_LAYOUT_H

/* ------------------------------------------------------------------------- */
/* Panel size (fallback until gfx_config.h provides GFX_SCREEN_W/H)          */
/* ------------------------------------------------------------------------- */

#if defined(__has_include)
#if __has_include("gfx_config.h")
#include "gfx_config.h"
#endif
#endif

#ifndef GFX_SCREEN_W
#if defined(PIPOD_DISPLAY_ST7796) && (PIPOD_DISPLAY_ST7796)
#define GFX_SCREEN_W 480
#else
#define GFX_SCREEN_W 320
#endif
#endif

#ifndef GFX_SCREEN_H
#if defined(PIPOD_DISPLAY_ST7796) && (PIPOD_DISPLAY_ST7796)
#define GFX_SCREEN_H 320
#else
#define GFX_SCREEN_H 240
#endif
#endif

/** 1 for the 480x320 (ST7796) layout, 0 for the original 320x240 one. */
#if (GFX_SCREEN_W >= 480) || (GFX_SCREEN_H >= 320)
#define UI_LAYOUT_LARGE 1
#else
#define UI_LAYOUT_LARGE 0
#endif

#if UI_LAYOUT_LARGE && (GFX_SCREEN_W < 480 || GFX_SCREEN_H < 320)
#error "UI_LAYOUT_LARGE requires at least 480x320"
#endif

/* ------------------------------------------------------------------------- */
/* 320x240 (ILI9341) - the values this UI shipped with                       */
/* ------------------------------------------------------------------------- */

#if !UI_LAYOUT_LARGE

#define UI_SCREEN_W GFX_SCREEN_W
#define UI_SCREEN_H GFX_SCREEN_H

#define UI_STATUS_H 22
#define UI_CONTENT_Y 22
#define UI_CONTENT_H (UI_SCREEN_H - UI_CONTENT_Y)

/* Menu list. */
#define UI_ROW_H 27
#define UI_ROWS_VISIBLE 8
#define UI_LIST_Y (UI_CONTENT_Y + 2)
#define UI_LIST_H (UI_ROWS_VISIBLE * UI_ROW_H)
#define UI_SCROLLBAR_X 314
#define UI_SCROLLBAR_W 4
#define UI_SCROLL_THUMB_MIN 16

/* One menu row. */
#define UI_ROW_TEXT_X 12
#define UI_ROW_TITLE_BASE 20   /* baseline offset inside the row */
#define UI_ROW_SUB_BASE 19
#define UI_ROW_TEXT_RIGHT 306  /* right edge of the label without a chevron */
#define UI_ROW_TEXT_RIGHT_CHEVRON 292
#define UI_ROW_SUB_MAX_W 120
#define UI_ROW_SUB_GAP 8
#define UI_CHEVRON_X 300
#define UI_CHEVRON_Y 8
#define UI_CHEVRON_W 8
#define UI_CHEVRON_H 12
#define UI_CHEVRON_STROKE 2

/* Status bar. */
#define UI_STATUS_PLAY_X 8
#define UI_STATUS_PLAY_Y 6
#define UI_STATUS_PLAY_W 10
#define UI_STATUS_PLAY_H 12
#define UI_STATUS_PERF_X 22
#define UI_STATUS_PERF_BASE 15
#define UI_STATUS_PERF_MAX_W 72
#define UI_STATUS_SHUFFLE_X 258
#define UI_STATUS_REPEAT_X 272
#define UI_STATUS_MODE_Y 6
#define UI_STATUS_MODE_W 10
#define UI_STATUS_MODE_H 12
#define UI_STATUS_BATT_X 290
#define UI_STATUS_BATT_Y 6
#define UI_STATUS_BATT_W 20
#define UI_STATUS_BATT_H 10
#define UI_STATUS_BATT_NUB_W 2
#define UI_STATUS_TITLE_MAX_W 150

#define UI_RECT_PLAY_GLYPH_X 4
#define UI_RECT_PLAY_GLYPH_Y 3
#define UI_RECT_PLAY_GLYPH_W 16
#define UI_RECT_PLAY_GLYPH_H 16
#define UI_RECT_MODE_ICONS_X 254
#define UI_RECT_MODE_ICONS_Y 3
#define UI_RECT_MODE_ICONS_W 34
#define UI_RECT_MODE_ICONS_H 16
#define UI_RECT_BATTERY_X 286
#define UI_RECT_BATTERY_Y 3
#define UI_RECT_BATTERY_W 30
#define UI_RECT_BATTERY_H 16
#define UI_RECT_PERF_X 20
#define UI_RECT_PERF_Y 2
#define UI_RECT_PERF_W 78
#define UI_RECT_PERF_H 18

/* Now Playing. */
#define UI_ART_X 14
#define UI_ART_Y 40
#define UI_ART_SIZE 120
#define UI_ART_FRAME_R 3
#define UI_ART_REFL_Y 162
#define UI_ART_REFL_H 24
#define UI_ART_SCALE_NUM 1
#define UI_ART_SCALE_DEN 1
#define UI_NP_TITLE_COL_X 148
#define UI_NP_TITLE_COL_W 160
#define UI_NP_QUEUE_BASE 52
#define UI_NP_TITLE_BASE 84
#define UI_NP_ARTIST_BASE 108
#define UI_NP_ALBUM_BASE 128
#define UI_NP_TITLE_RECT_Y 62
#define UI_NP_TITLE_RECT_H 30
#define UI_NP_TEXT_RECT_Y 48
#define UI_NP_TEXT_RECT_H 98
#define UI_BAR_X 14
#define UI_BAR_Y 196
#define UI_BAR_W 292
#define UI_BAR_H 6
#define UI_BAR_LABEL_BASE 220
#define UI_PROGRESS_X 8
#define UI_PROGRESS_Y 186
#define UI_PROGRESS_W 304
#define UI_PROGRESS_H 42
#define UI_VOL_SPK_LOW_X 20
#define UI_VOL_SPK_HIGH_X 292
#define UI_VOL_SPK_Y 194
#define UI_VOL_SPK_NUM 1
#define UI_VOL_SPK_DEN 1
#define UI_VOL_BAR_X 48
#define UI_VOL_BAR_W 242
#define UI_NP_EMPTY_BASE 132
/* Album-art placeholder note, in 120x120 art-relative coordinates. */
#define UI_ART_NOTE_CX 52
#define UI_ART_NOTE_CY 96
#define UI_ART_NOTE_R 9
#define UI_ART_NOTE_STEM_X 59
#define UI_ART_NOTE_STEM_Y 28
#define UI_ART_NOTE_STEM_W 3
#define UI_ART_NOTE_STEM_H 68

/* About screen. */
#define UI_ABOUT_TITLE_BASE 62
#define UI_ABOUT_BUILD_BASE 94
#define UI_ABOUT_CLOCK_BASE 120
#define UI_ABOUT_RAM_BASE 146
#define UI_ABOUT_FOOT_BASE 188

/* Font roles (see lib/gfx/fonts/fonts.h). */
#define UI_FONT_SMALL (&font_inter_12)
#define UI_FONT_BODY (&font_inter_14)
#define UI_FONT_STATUS UI_FONT_BODY
#define UI_FONT_ROW (&font_inter_16sb)
#define UI_FONT_TITLE (&font_inter_20b)

/* ------------------------------------------------------------------------- */
/* 480x320 (ST7796) - same iPod Classic proportions on the bigger panel      */
/* ------------------------------------------------------------------------- */

#else /* UI_LAYOUT_LARGE */

#define UI_SCREEN_W GFX_SCREEN_W
#define UI_SCREEN_H GFX_SCREEN_H

#define UI_STATUS_H 30
#define UI_CONTENT_Y 30
#define UI_CONTENT_H (UI_SCREEN_H - UI_CONTENT_Y)

/* Menu list: 8 x 36 px rows fill the 290 px content area below the 2 px pad. */
#define UI_ROW_H 36
#define UI_ROWS_VISIBLE 8
#define UI_LIST_Y (UI_CONTENT_Y + 2)
#define UI_LIST_H (UI_ROWS_VISIBLE * UI_ROW_H)
#define UI_SCROLLBAR_X 472
#define UI_SCROLLBAR_W 6
#define UI_SCROLL_THUMB_MIN 24

/* One menu row. */
#define UI_ROW_TEXT_X 18
#define UI_ROW_TITLE_BASE 25
#define UI_ROW_SUB_BASE 24
#define UI_ROW_TEXT_RIGHT 462
#define UI_ROW_TEXT_RIGHT_CHEVRON 438
#define UI_ROW_SUB_MAX_W 180
#define UI_ROW_SUB_GAP 12
#define UI_CHEVRON_X 452
#define UI_CHEVRON_Y 12
#define UI_CHEVRON_W 10
#define UI_CHEVRON_H 16
#define UI_CHEVRON_STROKE 3

/* Status bar. */
#define UI_STATUS_PLAY_X 12
#define UI_STATUS_PLAY_Y 8
#define UI_STATUS_PLAY_W 14
#define UI_STATUS_PLAY_H 16
#define UI_STATUS_PERF_X 34
#define UI_STATUS_PERF_BASE 21
#define UI_STATUS_PERF_MAX_W 108
#define UI_STATUS_SHUFFLE_X 384
#define UI_STATUS_REPEAT_X 406
#define UI_STATUS_MODE_Y 7
#define UI_STATUS_MODE_W 14
#define UI_STATUS_MODE_H 16
#define UI_STATUS_BATT_X 440
#define UI_STATUS_BATT_Y 8
#define UI_STATUS_BATT_W 28
#define UI_STATUS_BATT_H 14
#define UI_STATUS_BATT_NUB_W 3
#define UI_STATUS_TITLE_MAX_W 240

#define UI_RECT_PLAY_GLYPH_X 8
#define UI_RECT_PLAY_GLYPH_Y 5
#define UI_RECT_PLAY_GLYPH_W 20
#define UI_RECT_PLAY_GLYPH_H 20
#define UI_RECT_MODE_ICONS_X 380
#define UI_RECT_MODE_ICONS_Y 4
#define UI_RECT_MODE_ICONS_W 44
#define UI_RECT_MODE_ICONS_H 22
#define UI_RECT_BATTERY_X 432
#define UI_RECT_BATTERY_Y 5
#define UI_RECT_BATTERY_W 40
#define UI_RECT_BATTERY_H 22
#define UI_RECT_PERF_X 32
#define UI_RECT_PERF_Y 2
#define UI_RECT_PERF_W 116
#define UI_RECT_PERF_H 26

/* Now Playing. */
#define UI_ART_X 20
#define UI_ART_Y 46
#define UI_ART_SIZE 180
#define UI_ART_FRAME_R 4
#define UI_ART_REFL_Y 229
#define UI_ART_REFL_H 30
#define UI_ART_SCALE_NUM 3
#define UI_ART_SCALE_DEN 2
#define UI_NP_TITLE_COL_X 228
#define UI_NP_TITLE_COL_W 236
#define UI_NP_QUEUE_BASE 82
#define UI_NP_TITLE_BASE 120
#define UI_NP_ARTIST_BASE 152
#define UI_NP_ALBUM_BASE 180
#define UI_NP_TITLE_RECT_Y 88
#define UI_NP_TITLE_RECT_H 44
#define UI_NP_TEXT_RECT_Y 62
#define UI_NP_TEXT_RECT_H 134
#define UI_BAR_X 22
#define UI_BAR_Y 270
#define UI_BAR_W 436
#define UI_BAR_H 8
#define UI_BAR_LABEL_BASE 300
#define UI_PROGRESS_X 12
#define UI_PROGRESS_Y 260
#define UI_PROGRESS_W 456
#define UI_PROGRESS_H 52
#define UI_VOL_SPK_LOW_X 28
#define UI_VOL_SPK_HIGH_X 432
#define UI_VOL_SPK_Y 262
#define UI_VOL_SPK_NUM 3
#define UI_VOL_SPK_DEN 2
#define UI_VOL_BAR_X 80
#define UI_VOL_BAR_W 340
#define UI_NP_EMPTY_BASE 176
/* Album-art placeholder note, in 120x120 art-relative coordinates. */
#define UI_ART_NOTE_CX 52
#define UI_ART_NOTE_CY 96
#define UI_ART_NOTE_R 9
#define UI_ART_NOTE_STEM_X 59
#define UI_ART_NOTE_STEM_Y 28
#define UI_ART_NOTE_STEM_W 3
#define UI_ART_NOTE_STEM_H 68

/* About screen. */
#define UI_ABOUT_TITLE_BASE 96
#define UI_ABOUT_BUILD_BASE 130
#define UI_ABOUT_CLOCK_BASE 162
#define UI_ABOUT_RAM_BASE 194
#define UI_ABOUT_FOOT_BASE 250

/* Font roles: larger faces generated by scripts/regen_assets.py. */
#define UI_FONT_SMALL (&font_inter_16)
#define UI_FONT_BODY (&font_inter_18)
#define UI_FONT_STATUS UI_FONT_BODY
#define UI_FONT_ROW (&font_inter_22sb)
#define UI_FONT_TITLE (&font_inter_28b)

#endif /* UI_LAYOUT_LARGE */

#endif /* UI_LAYOUT_H */
