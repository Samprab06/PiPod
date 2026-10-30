/**
 * @file ui_settings.c
 * @brief Settings screen rows and the static About screen.
 *
 * Settings are adjusted in place: the wheel changes the backlight brightness
 * while that row is selected, SELECT cycles the display clock, toggles the
 * shuffle/repeat flags (posted to the command queue) and opens About. The
 * shuffle/repeat values follow the audio core through ui_settings_sync().
 */

#include "ui_internal.h"

#include <stdio.h>
#include <string.h>

#include "fonts.h"
#include "ili9341.h"
#include "ui_widgets.h"

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

#define UI_SETTING_ROWS 6u

static struct {
    uint8_t backlight; /**< 10..100 percent, applied immediately. */
    uint8_t clock_idx; /**< Index into s_clock_hz of the last selection. */
    bool shuffle;
    bool repeat;
    bool perf;
} s_set;

/* Only rates that clk_peri (150 MHz) can actually produce: 150 / (2 * n). */
static const uint32_t s_clock_hz[] = {18750000u, 25000000u, 37500000u, 75000000u};

/** Per-row subtitle buffers (rows are drawn one at a time, but keep them apart). */
static char s_sub[UI_SETTING_ROWS][32];

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static void format_mhz(char *buf, size_t cap) {
    if (buf == NULL || cap == 0u) {
        return;
    }
    const uint32_t hz = ili9341_get_spi_clock();
    if (hz == 0u) {
        (void)snprintf(buf, cap, "--");
        return;
    }
    /* Hundredths of a MHz so 18.75 MHz is not rounded to 18.8 MHz. */
    const uint32_t hundredths = (hz + 5000u) / 10000u;
    const char *exp = (hz >= 70000000u) ? " (exp)" : "";
    if ((hundredths % 100u) == 0u) {
        (void)snprintf(buf, cap, "%lu MHz%s", (unsigned long)(hundredths / 100u), exp);
    } else if ((hundredths % 10u) == 0u) {
        (void)snprintf(buf, cap, "%lu.%lu MHz%s", (unsigned long)(hundredths / 100u),
                       (unsigned long)((hundredths / 10u) % 10u), exp);
    } else {
        (void)snprintf(buf, cap, "%lu.%02lu MHz%s", (unsigned long)(hundredths / 100u),
                       (unsigned long)(hundredths % 100u), exp);
    }
}

static uint32_t free_ram_kb(void) {
    /* Linker-provided heap bounds (see the SDK memmap). Estimate only. */
    extern char __end__;
    extern char __HeapLimit;
    const uintptr_t start = (uintptr_t)&__end__;
    const uintptr_t limit = (uintptr_t)&__HeapLimit;
    if (limit <= start) {
        return 0u;
    }
    return (uint32_t)((limit - start) / 1024u);
}

/* ------------------------------------------------------------------------- */
/* Public (internal) API                                                     */
/* ------------------------------------------------------------------------- */

void ui_settings_init(void) {
    s_set.backlight = 100u;
    s_set.clock_idx = 2u;
    s_set.shuffle = false;
    s_set.repeat = false;
    s_set.perf = false;
    memset(s_sub, 0, sizeof s_sub);
}

void ui_settings_sync(const ui_np_t *np) {
    if (np == NULL) {
        return;
    }
    s_set.shuffle = np->shuffle;
    s_set.repeat = np->repeat;
}

uint32_t ui_settings_count(void) {
    return UI_SETTING_ROWS;
}

void ui_settings_row(uint32_t index, ui_row_t *out) {
    if (out == NULL) {
        return;
    }
    out->subtitle = NULL;
    out->flags = 0;

    switch (index) {
    case 0:
        out->title = "Backlight";
        (void)snprintf(s_sub[0], sizeof s_sub[0], "%u %%", (unsigned)s_set.backlight);
        out->subtitle = s_sub[0];
        break;
    case 1:
        out->title = "Shuffle";
        out->subtitle = s_set.shuffle ? "On" : "Off";
        break;
    case 2:
        out->title = "Repeat";
        out->subtitle = s_set.repeat ? "On" : "Off";
        break;
    case 3:
        out->title = "Display Clock";
        format_mhz(s_sub[3], sizeof s_sub[3]);
        out->subtitle = s_sub[3];
        break;
    case 4:
        out->title = "Performance overlay";
        out->subtitle = s_set.perf ? "On" : "Off";
        break;
    case 5:
        out->title = "About";
        out->flags = UI_ROW_CHEVRON;
        break;
    default:
        out->title = "";
        break;
    }
}

void ui_settings_activate(const ui_np_t *np, uint32_t index) {
    (void)np;
    switch (index) {
    case 1:
        s_set.shuffle = !s_set.shuffle;
        ui_shared_post_simple(UI_CMD_SET_SHUFFLE, s_set.shuffle ? 1 : 0);
        break;
    case 2:
        s_set.repeat = !s_set.repeat;
        ui_shared_post_simple(UI_CMD_SET_REPEAT, s_set.repeat ? 1 : 0);
        break;
    case 3: {
        /* Cycle through the fixed list; the row shows the achieved clock. */
        const size_t n = sizeof s_clock_hz / sizeof s_clock_hz[0];
        s_set.clock_idx = (uint8_t)((s_set.clock_idx + 1u) % n);
        (void)ili9341_set_spi_clock(s_clock_hz[s_set.clock_idx]);
        break;
    }
    case 4:
        s_set.perf = !s_set.perf;
        break;
    case 5:
        (void)ui_nav_push(UI_SCR_ABOUT);
        break;
    default:
        break;
    }
}

bool ui_settings_wheel(uint32_t selected, int ticks) {
    if (selected != 0u || ticks == 0) {
        return false;
    }
    int value = (int)s_set.backlight + ticks * 5;
    if (value < 10) {
        value = 10;
    }
    if (value > 100) {
        value = 100;
    }
    s_set.backlight = (uint8_t)value;
    ili9341_set_backlight(s_set.backlight);
    return true;
}

bool ui_settings_perf_overlay(void) {
    return s_set.perf;
}

void ui_settings_about_draw(gfx_canvas_t *c, int x_off) {
    if (c == NULL || !ui_visible(c, x_off, UI_CONTENT_Y, UI_SCREEN_W, UI_CONTENT_H)) {
        return;
    }
    gfx_fill_rect(c, x_off, UI_CONTENT_Y, UI_SCREEN_W, UI_CONTENT_H, UI_COL_BG);

    ui_text_center(c, &font_inter_20b, 160 + x_off, 62, 0, "PiPod", UI_COL_TEXT);

    char line[40];
    char clk[32];
    (void)snprintf(line, sizeof line, "Build %s", __DATE__);
    ui_text_center(c, &font_inter_14, 160 + x_off, 94, 0, line, UI_COL_TEXT_2);

    format_mhz(clk, sizeof clk);
    (void)snprintf(line, sizeof line, "Display %s", clk);
    ui_text_center(c, &font_inter_14, 160 + x_off, 120, 0, line, UI_COL_TEXT_2);

    (void)snprintf(line, sizeof line, "Free RAM ~%lu KB", (unsigned long)free_ram_kb());
    ui_text_center(c, &font_inter_14, 160 + x_off, 146, 0, line, UI_COL_TEXT_2);

    ui_text_center(c, &font_inter_12, 160 + x_off, 188, 0, "Purdue Proton / RP2350B", UI_COL_TEXT_2);
}
