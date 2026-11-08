/*
 * ST7796S 320x480 TFT driver (4-wire SPI + DMA) for the Purdue Proton.
 *
 * The module is used in landscape (480x320) with RGB565 pixels. Commands and
 * parameters use 8-bit SPI frames; pixel data uses 16-bit frames so native
 * uint16_t RGB565 values go out MSB first, as the panel expects.
 *
 * The module has no MISO/SDO pin: nothing can be read back. There are no
 * register-read or pixel-read entry points; st7796_self_test() reports
 * ST7796_SELFTEST_UNSUPPORTED instead of pretending to fail. The backlight pin
 * is tied to 3.3 V on the board, so st7796_set_backlight() is a no-op kept for
 * API parity with the ILI9341 driver.
 *
 * Threading: the driver is not internally synchronised. A single caller (the
 * UI core) owns it. Asynchronous pixel transfers are completed by
 * st7796_wait_idle(), which must be called before the source buffer is reused.
 */

#ifndef ST7796_H
#define ST7796_H

#include <stdbool.h>
#include <stdint.h>

#include "hardware/spi.h"

#include "st7796_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Panel orientation. Landscape modes are 480x320, portrait modes 320x480. */
typedef enum {
    ST7796_LANDSCAPE = 0,         /**< 480x320, origin top-left. */
    ST7796_LANDSCAPE_FLIPPED = 1, /**< 480x320, rotated 180 degrees. */
    ST7796_PORTRAIT = 2,          /**< 320x480, origin top-left. */
    ST7796_PORTRAIT_FLIPPED = 3   /**< 320x480, rotated 180 degrees. */
} st7796_rotation_t;

/** Runtime configuration (pins, SPI instance and clock). */
typedef struct {
    spi_inst_t *spi; /**< SPI instance, e.g. spi0. */
    uint pin_cs;     /**< Chip select GPIO, driven by SIO. */
    uint pin_sck;    /**< SPI clock GPIO. */
    uint pin_mosi;   /**< SPI MOSI / SDA GPIO. */
    uint pin_dc;     /**< Data/command GPIO (high = data). */
    uint pin_rst;    /**< Hardware reset GPIO (active low). */
    uint32_t spi_hz; /**< Requested SPI clock in Hz. */
    bool fast_pins;  /**< 12 mA fast-slew drive on SCK/MOSI/CS/DC. */
} st7796_config_t;

/** Transfer statistics, useful for benchmarks. */
typedef struct {
    uint64_t bytes_sent;   /**< Pixel bytes handed to DMA since the last reset. */
    uint32_t transfers;    /**< Number of DMA transfers started. */
    uint64_t dma_busy_us;  /**< Microseconds spent waiting for DMA to finish. */
    uint64_t flash_bytes;  /**< Of bytes_sent, bytes moved through the XIP stream (flash -> panel). */
    uint32_t flash_blits;  /**< st7796_blit_flash_async() calls that took the stream path. */
} st7796_stats_t;

/** Result of st7796_self_test(). */
typedef enum {
    /** The module has no MISO, so there is no readback path to test. */
    ST7796_SELFTEST_UNSUPPORTED = 0,
} st7796_selftest_result_t;

/**
 * \brief Fill a config with the board defaults from st7796_config.h.
 *
 * \param cfg Destination config; must not be NULL.
 */
void st7796_default_config(st7796_config_t *cfg);

/**
 * \brief Initialise the panel and the SPI/DMA hardware.
 *
 * Performs a hardware reset, sleep out (120 ms), the vendor-unlock, power,
 * VCOM, frame-rate, gamma and pixel-format init sequence, sends the vendor
 * lock commands, sets COLMOD to 16-bit RGB565 and landscape BGR MADCTL, then
 * turns the display on. Claims three DMA channels (strip blits plus the two
 * used by the flash stream path) and applies cfg->fast_pins.
 *
 * There is no readback on this module, so unlike ili9341_init() the result is
 * a pure initialisation status: false only when cfg is invalid.
 *
 * \param cfg Configuration to use; copied into the driver.
 * \return true when the panel was initialised.
 */
bool st7796_init(const st7796_config_t *cfg);

/**
 * \brief Change the SPI clock at runtime.
 *
 * Waits for the bus to become idle before reprogramming the clock. The PL022
 * produces clk_peri divided by an even divisor, so the achieved rate is
 * rounded down. 37.5 MHz is the default; 75 MHz may work but is untested on
 * this module (see st7796_config.h).
 *
 * \param hz Requested SPI clock in Hz.
 * \return The clock actually achieved by the SPI hardware.
 */
uint32_t st7796_set_spi_clock(uint32_t hz);

/**
 * \brief Select fast or default pad drive on the display control lines.
 *
 * \param fast true for 12 mA + fast slew, false for the 4 mA + slow defaults.
 */
void st7796_set_pin_drive(bool fast);

/**
 * \brief Get the current SPI clock.
 *
 * \return The actual SPI clock in Hz.
 */
uint32_t st7796_get_spi_clock(void);

/**
 * \brief Set the panel orientation and update st7796_width()/height().
 *
 * \param r New rotation.
 */
void st7796_set_rotation(st7796_rotation_t r);

/**
 * \brief Display width in pixels for the current rotation.
 *
 * \return 480 in landscape modes, 320 in portrait modes.
 */
uint16_t st7796_width(void);

/**
 * \brief Display height in pixels for the current rotation.
 *
 * \return 320 in landscape modes, 480 in portrait modes.
 */
uint16_t st7796_height(void);

/**
 * \brief Program the address window and start a pixel write (CASET/RASET/RAMWR).
 *
 * Leaves chip select asserted and the panel in pixel-write mode; the next
 * st7796_blit_async()/st7796_fill_rect() continues that transaction, and
 * st7796_wait_idle() releases chip select.
 *
 * \param x Left edge in pixels.
 * \param y Top edge in pixels.
 * \param w Window width in pixels.
 * \param h Window height in pixels.
 */
void st7796_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/**
 * \brief Start an asynchronous 16-bit DMA transfer of RGB565 pixels.
 *
 * Waits for any previous transfer, sets the window, then starts the DMA and
 * returns immediately. The caller must not modify \p px until
 * st7796_wait_idle() (or the next blit) has completed.
 *
 * \param x Left edge in pixels.
 * \param y Top edge in pixels.
 * \param w Width in pixels.
 * \param h Height in pixels.
 * \param px w*h RGB565 pixels, row-major.
 */
void st7796_blit_async(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *px);

/**
 * \brief Fill a rectangle with one colour using DMA (read address fixed).
 *
 * \param x Left edge in pixels.
 * \param y Top edge in pixels.
 * \param w Width in pixels.
 * \param h Height in pixels.
 * \param color RGB565 colour.
 */
void st7796_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/**
 * \brief Blit RGB565 pixels straight from XIP flash (or RAM) to the panel.
 *
 * Identical in design to ili9341_blit_flash_async(): for a pointer inside the
 * XIP window (0x10000000..0x11000000, 4-byte aligned) the pixels travel
 * through the RP2350 XIP stream unit into a 2 x 1 KB RAM ring that the SPI DMA
 * drains. A RAM pointer (or an unaligned XIP pointer) takes the plain DMA path
 * of st7796_blit_async() and always returns true.
 *
 * The blit claims the single stream unit through lib/flashstream (audio has
 * priority). If the claim fails, or audio takes the unit while the blit is
 * running, the function returns false and nothing is guaranteed: the caller
 * must repaint the rectangle with the strip path.
 *
 * The call is synchronous in both cases: all DMA has completed when it returns.
 *
 * \param x Left edge in pixels.
 * \param y Top edge in pixels.
 * \param w Width in pixels (also the source stride).
 * \param h Height in pixels.
 * \param px w*h RGB565 pixels at an XIP flash or RAM address.
 * \return true when the pixels were sent; false when the caller must fall back
 *         to the strip renderer.
 */
bool st7796_blit_flash_async(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                             const uint16_t *px);

/**
 * \brief Block until the DMA transfer is done, the SPI is idle and CS is high.
 *
 * Must be called before reusing a blit source buffer and before changing the
 * SPI configuration.
 */
void st7796_wait_idle(void);

/**
 * \brief Report whether a transfer is still in progress.
 *
 * \return true while DMA is pending or the SPI shifter is busy.
 */
bool st7796_is_busy(void);

/**
 * \brief Report that panel readback is unavailable.
 *
 * The module has no MISO pin, so register and pixel reads do not exist. This
 * returns ST7796_SELFTEST_UNSUPPORTED; it is never an error.
 *
 * \return ST7796_SELFTEST_UNSUPPORTED.
 */
st7796_selftest_result_t st7796_self_test(void);

/**
 * \brief Backlight control (no-op).
 *
 * BL is tied to 3.3 V on the board, so the brightness cannot be changed. The
 * call is accepted (and ignored) so the display layer can treat both panels
 * the same way.
 *
 * \param percent Ignored.
 */
void st7796_set_backlight(uint8_t percent);

/**
 * \brief Enter or leave panel sleep mode.
 *
 * \param sleep true for sleep in, false for sleep out.
 */
void st7796_sleep(bool sleep);

/**
 * \brief Enable or disable display inversion.
 *
 * \param on true for inverted colours.
 */
void st7796_invert(bool on);

/**
 * \brief Copy the transfer statistics.
 *
 * \param out Destination; must not be NULL.
 */
void st7796_get_stats(st7796_stats_t *out);

/** \brief Zero the transfer statistics. */
void st7796_reset_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* ST7796_H */
