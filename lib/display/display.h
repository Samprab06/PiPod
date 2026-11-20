/*
 * Compile-time display abstraction for the Purdue Proton.
 *
 * Two panels are supported and only one is compiled into a firmware image:
 *   - PIPOD_DISPLAY_ST7796 undefined or 0: ILI9341, 320x240 landscape.
 *   - PIPOD_DISPLAY_ST7796 = 1: ST7796S, 480x320 landscape.
 *
 * Every entry point is a static inline wrapper around the selected driver, so
 * gfx and the rest of the firmware are panel-agnostic and pay no runtime cost.
 * The definition must be visible when this header is included, i.e. it has to
 * be applied to lib/gfx and the application target as well as to lib/display
 * sources (see lib/display/CMakeLists.txt, pipod_display_select()).
 *
 * ili9341.h keeps its original API for existing callers; this layer only adds
 * the panel-neutral names.
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef PIPOD_DISPLAY_ST7796
#define PIPOD_DISPLAY_ST7796 0
#endif

#if PIPOD_DISPLAY_ST7796
#include "st7796.h"
typedef st7796_config_t display_config_t;
typedef st7796_rotation_t display_rotation_t;
typedef st7796_stats_t display_stats_t;
#else
#include "ili9341.h"
typedef ili9341_config_t display_config_t;
typedef ili9341_rotation_t display_rotation_t;
typedef ili9341_stats_t display_stats_t;
#endif

/** Rotation values shared by both panels (cast to display_rotation_t). */
#define DISPLAY_LANDSCAPE 0         /**< Landscape, origin top-left. */
#define DISPLAY_LANDSCAPE_FLIPPED 1 /**< Landscape, rotated 180 degrees. */
#define DISPLAY_PORTRAIT 2          /**< Portrait, origin top-left. */
#define DISPLAY_PORTRAIT_FLIPPED 3  /**< Portrait, rotated 180 degrees. */

/** Result of display_self_test(). */
typedef enum {
    DISPLAY_SELFTEST_OK = 0,          /**< Readback looks healthy. */
    DISPLAY_SELFTEST_FAILED = 1,      /**< Readback ran but looks unhealthy. */
    DISPLAY_SELFTEST_UNSUPPORTED = 2, /**< The panel has no readback path (ST7796). */
} display_selftest_result_t;

#if PIPOD_DISPLAY_ST7796
/**
 * Panel readback results.
 *
 * The ST7796 module has no MISO, so display_self_test() always returns
 * DISPLAY_SELFTEST_UNSUPPORTED and zeroes this struct. It mirrors
 * ili9341_selftest_t so diagnostic code can use one type for both panels.
 */
typedef struct {
    uint32_t rddid;     /**< Always 0 on ST7796. */
    uint8_t power_mode; /**< Always 0 on ST7796. */
    uint8_t madctl;     /**< Always 0 on ST7796. */
    uint8_t pixfmt;     /**< Always 0 on ST7796. */
    bool miso_alive;    /**< Always false on ST7796. */
    bool madctl_ok;     /**< Always false on ST7796. */
    bool pixfmt_ok;     /**< Always false on ST7796. */
    bool display_on;    /**< Always false on ST7796. */
    bool dummy_bit;     /**< Always false on ST7796. */
} display_selftest_t;
#else
typedef ili9341_selftest_t display_selftest_t;
#endif

#if PIPOD_DISPLAY_ST7796

/** Fill a config with the ST7796 board defaults. */
static inline void display_default_config(display_config_t *cfg) {
    st7796_default_config(cfg);
}

/**
 * \brief Initialise the selected panel.
 *
 * \return true when the panel was initialised. The ST7796 has no readback, so
 *         this is not a self-test result (see display_self_test()).
 */
static inline bool display_init(const display_config_t *cfg) {
    return st7796_init(cfg);
}

/** Display width in pixels for the current rotation. */
static inline uint16_t display_width(void) {
    return st7796_width();
}

/** Display height in pixels for the current rotation. */
static inline uint16_t display_height(void) {
    return st7796_height();
}

/** Program the address window and start a pixel write. */
static inline void display_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    st7796_set_window(x, y, w, h);
}

/** Start an asynchronous RGB565 DMA blit. */
static inline void display_blit_async(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      const uint16_t *px) {
    st7796_blit_async(x, y, w, h, px);
}

/** Blit RGB565 pixels straight from XIP flash when possible. */
static inline bool display_blit_flash_async(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                            const uint16_t *px) {
    return st7796_blit_flash_async(x, y, w, h, px);
}

/** Fill a rectangle with one RGB565 colour. */
static inline void display_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                     uint16_t color) {
    st7796_fill_rect(x, y, w, h, color);
}

/** Block until DMA and the SPI shifter are idle and CS is high. */
static inline void display_wait_idle(void) {
    st7796_wait_idle();
}

/** True while a transfer is still in progress. */
static inline bool display_is_busy(void) {
    return st7796_is_busy();
}

/** Backlight brightness (no-op: BL is tied to 3.3 V on the ST7796 module). */
static inline void display_set_backlight(uint8_t percent) {
    st7796_set_backlight(percent);
}

/** Change the SPI clock; returns the achieved rate. */
static inline uint32_t display_set_spi_clock(uint32_t hz) {
    return st7796_set_spi_clock(hz);
}

/** Current SPI clock in Hz. */
static inline uint32_t display_get_spi_clock(void) {
    return st7796_get_spi_clock();
}

/** Select fast or default pad drive on the control lines. */
static inline void display_set_pin_drive(bool fast) {
    st7796_set_pin_drive(fast);
}

/** Enter or leave panel sleep mode. */
static inline void display_sleep(bool sleep) {
    st7796_sleep(sleep);
}

/** Set the panel rotation. */
static inline void display_set_rotation(display_rotation_t r) {
    st7796_set_rotation(r);
}

/** Copy the transfer statistics. */
static inline void display_get_stats(display_stats_t *out) {
    st7796_get_stats(out);
}

/** Zero the transfer statistics. */
static inline void display_reset_stats(void) {
    st7796_reset_stats();
}

/**
 * \brief Probe the panel over MISO.
 *
 * Always returns DISPLAY_SELFTEST_UNSUPPORTED on the ST7796: the module has no
 * MISO/SDO pin, so neither register nor pixel readback exists. This is not an
 * error. \p out is zeroed when it is not NULL.
 */
static inline display_selftest_result_t display_self_test(display_selftest_t *out) {
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    (void)st7796_self_test();
    return DISPLAY_SELFTEST_UNSUPPORTED;
}

/** Human-readable panel name. */
static inline const char *display_name(void) {
    return "ST7796S";
}

#else /* ILI9341 */

/** Fill a config with the ILI9341 board defaults. */
static inline void display_default_config(display_config_t *cfg) {
    ili9341_default_config(cfg);
}

/** Initialise the selected panel (includes a readback self-test). */
static inline bool display_init(const display_config_t *cfg) {
    return ili9341_init(cfg);
}

/** Display width in pixels for the current rotation. */
static inline uint16_t display_width(void) {
    return ili9341_width();
}

/** Display height in pixels for the current rotation. */
static inline uint16_t display_height(void) {
    return ili9341_height();
}

/** Program the address window and start a pixel write. */
static inline void display_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    ili9341_set_window(x, y, w, h);
}

/** Start an asynchronous RGB565 DMA blit. */
static inline void display_blit_async(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      const uint16_t *px) {
    ili9341_blit_async(x, y, w, h, px);
}

/** Blit RGB565 pixels straight from XIP flash when possible. */
static inline bool display_blit_flash_async(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                            const uint16_t *px) {
    return ili9341_blit_flash_async(x, y, w, h, px);
}

/** Fill a rectangle with one RGB565 colour. */
static inline void display_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                     uint16_t color) {
    ili9341_fill_rect(x, y, w, h, color);
}

/** Block until DMA and the SPI shifter are idle and CS is high. */
static inline void display_wait_idle(void) {
    ili9341_wait_idle();
}

/** True while a transfer is still in progress. */
static inline bool display_is_busy(void) {
    return ili9341_is_busy();
}

/** Backlight brightness, 0..100. */
static inline void display_set_backlight(uint8_t percent) {
    ili9341_set_backlight(percent);
}

/** Change the SPI clock; returns the achieved rate. */
static inline uint32_t display_set_spi_clock(uint32_t hz) {
    return ili9341_set_spi_clock(hz);
}

/** Current SPI clock in Hz. */
static inline uint32_t display_get_spi_clock(void) {
    return ili9341_get_spi_clock();
}

/** Select fast or default pad drive on the control lines. */
static inline void display_set_pin_drive(bool fast) {
    ili9341_set_pin_drive(fast);
}

/** Enter or leave panel sleep mode. */
static inline void display_sleep(bool sleep) {
    ili9341_sleep(sleep);
}

/** Set the panel rotation. */
static inline void display_set_rotation(display_rotation_t r) {
    ili9341_set_rotation(r);
}

/** Copy the transfer statistics. */
static inline void display_get_stats(display_stats_t *out) {
    ili9341_get_stats(out);
}

/** Zero the transfer statistics. */
static inline void display_reset_stats(void) {
    ili9341_reset_stats();
}

/** Probe the panel over MISO and report what the readback shows. */
static inline display_selftest_result_t display_self_test(display_selftest_t *out) {
    if (!ili9341_has_readback()) {
        if (out != NULL) {
            *out = (display_selftest_t){0};
        }
        return DISPLAY_SELFTEST_UNSUPPORTED;
    }
    return ili9341_self_test(out) ? DISPLAY_SELFTEST_OK : DISPLAY_SELFTEST_FAILED;
}

/** Human-readable panel name. */
static inline const char *display_name(void) {
    return "ILI9341";
}

#endif /* PIPOD_DISPLAY_ST7796 */

#endif /* DISPLAY_H */
