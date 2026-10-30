/*
 * ILI9341 240x320 TFT driver (4-wire SPI + DMA) for the Purdue Proton.
 *
 * The panel is used in landscape (320x240) with RGB565 pixels. Commands and
 * parameters use 8-bit SPI frames; pixel data uses 16-bit frames so native
 * uint16_t RGB565 values go out MSB first, as the panel expects.
 *
 * Threading: the driver is not internally synchronised. A single caller (the
 * UI core) owns it. Asynchronous pixel transfers are completed by
 * ili9341_wait_idle(), which must be called before the source buffer is
 * reused.
 */

#ifndef ILI9341_H
#define ILI9341_H

#include <stdbool.h>
#include <stdint.h>

#include "hardware/spi.h"

#include "ili9341_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Panel orientation. Landscape modes are 320x240, portrait modes 240x320. */
typedef enum {
    ILI9341_LANDSCAPE = 0,         /**< 320x240, origin top-left. */
    ILI9341_LANDSCAPE_FLIPPED = 1, /**< 320x240, rotated 180 degrees. */
    ILI9341_PORTRAIT = 2,          /**< 240x320, origin top-left. */
    ILI9341_PORTRAIT_FLIPPED = 3   /**< 240x320, rotated 180 degrees. */
} ili9341_rotation_t;

/** Runtime configuration (pins, SPI instance and clock). */
typedef struct {
    spi_inst_t *spi; /**< SPI instance, e.g. spi0. */
    uint pin_miso;   /**< MISO / SDO GPIO (readback only). */
    uint pin_cs;     /**< Chip select GPIO, driven by SIO. */
    uint pin_sck;    /**< SPI clock GPIO. */
    uint pin_mosi;   /**< SPI MOSI / SDI GPIO. */
    uint pin_dc;     /**< Data/command GPIO (high = data). */
    uint pin_rst;    /**< Hardware reset GPIO (active low). */
    uint pin_bl;     /**< Backlight PWM GPIO. */
    uint32_t spi_hz; /**< Requested SPI clock in Hz. */
} ili9341_config_t;

/** Transfer statistics, useful for benchmarks. */
typedef struct {
    uint64_t bytes_sent;   /**< Pixel bytes handed to DMA since the last reset. */
    uint32_t transfers;    /**< Number of DMA transfers started. */
    uint64_t dma_busy_us;  /**< Microseconds spent waiting for DMA to finish. */
} ili9341_stats_t;

/** Panel readback self-test results (see ili9341_self_test()). */
typedef struct {
    uint32_t rddid;     /**< 24-bit RDDID; 0x009341 on a genuine ILI9341, often 0x000000 on clones. */
    uint8_t power_mode; /**< RDDPM (0x0A): bit 4 sleep-out, bit 2 display on. */
    uint8_t madctl;     /**< RDMADCTL (0x0B) value as read back. */
    uint8_t pixfmt;     /**< RDDCOLMOD (0x0C) value as read back. */
    bool miso_alive;    /**< true when at least one read returned something other than 0x00/0xFF. */
    bool madctl_ok;     /**< true when RDMADCTL matches the programmed orientation. */
    bool pixfmt_ok;     /**< true when RDDCOLMOD reports 16 bpp DBI (low nibble 0x5). */
    bool display_on;    /**< true when the panel is awake (sleep-out) and DISPON is set. */
    bool dummy_bit;     /**< true when 8-bit register reads carry one leading dummy bit. */
} ili9341_selftest_t;

/**
 * \brief Fill a config with the board defaults from ili9341_config.h.
 *
 * \param cfg Destination config; must not be NULL.
 */
void ili9341_default_config(ili9341_config_t *cfg);

/**
 * \brief Initialise the panel and the SPI/DMA hardware.
 *
 * Performs a hardware reset, the full power/VCOM/gamma/frame-rate init
 * sequence, sets COLMOD to 16-bit RGB565 and landscape BGR MADCTL, releases
 * sleep (120 ms) and turns the display on. Claims one DMA channel and starts
 * the backlight PWM at 100%.
 *
 * The panel is left initialised even when the readback self-test fails, so
 * the caller can retry or ignore the failure.
 *
 * \param cfg Configuration to use; copied into the driver.
 * \return The result of ili9341_self_test(): true when the readback looks
 *         healthy, false if cfg is invalid or the self-test fails. A false
 *         result does not mean the panel is dead: many modules leave MISO/SDO
 *         unconnected and still work write-only.
 */
bool ili9341_init(const ili9341_config_t *cfg);

/**
 * \brief Change the SPI clock at runtime.
 *
 * Waits for the bus to become idle before reprogramming the clock.
 *
 * \param hz Requested SPI clock in Hz.
 * \return The clock actually achieved by the SPI hardware.
 */
uint32_t ili9341_set_spi_clock(uint32_t hz);

/**
 * \brief Get the current SPI clock.
 *
 * \return The actual SPI clock in Hz.
 */
uint32_t ili9341_get_spi_clock(void);

/**
 * \brief Set the panel orientation and update ili9341_width()/height().
 *
 * \param r New rotation.
 */
void ili9341_set_rotation(ili9341_rotation_t r);

/**
 * \brief Display width in pixels for the current rotation.
 *
 * \return 320 in landscape modes, 240 in portrait modes.
 */
uint16_t ili9341_width(void);

/**
 * \brief Display height in pixels for the current rotation.
 *
 * \return 240 in landscape modes, 320 in portrait modes.
 */
uint16_t ili9341_height(void);

/**
 * \brief Program the address window and start a pixel write (CASET/PASET/RAMWR).
 *
 * Leaves chip select asserted and the panel in pixel-write mode; the next
 * ili9341_blit_async()/ili9341_fill_rect() continues that transaction, and
 * ili9341_wait_idle() releases chip select.
 *
 * \param x Left edge in pixels.
 * \param y Top edge in pixels.
 * \param w Window width in pixels.
 * \param h Window height in pixels.
 */
void ili9341_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/**
 * \brief Start an asynchronous 16-bit DMA transfer of RGB565 pixels.
 *
 * Waits for any previous transfer, sets the window, then starts the DMA and
 * returns immediately. The caller must not modify \p px until
 * ili9341_wait_idle() (or the next blit) has completed.
 *
 * \param x Left edge in pixels.
 * \param y Top edge in pixels.
 * \param w Width in pixels.
 * \param h Height in pixels.
 * \param px w*h RGB565 pixels, row-major.
 */
void ili9341_blit_async(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *px);

/**
 * \brief Fill a rectangle with one colour using DMA (read address fixed).
 *
 * \param x Left edge in pixels.
 * \param y Top edge in pixels.
 * \param w Width in pixels.
 * \param h Height in pixels.
 * \param color RGB565 colour.
 */
void ili9341_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/**
 * \brief Block until the DMA transfer is done, the SPI is idle and CS is high.
 *
 * Must be called before reusing a blit source buffer and before changing the
 * SPI configuration.
 */
void ili9341_wait_idle(void);

/**
 * \brief Report whether a transfer is still in progress.
 *
 * \return true while DMA is pending or the SPI shifter is busy.
 */
bool ili9341_is_busy(void);

/**
 * \brief Read the 24-bit RDDID response over MISO.
 *
 * Runs the read at 6 MHz or lower (the panel cannot read faster) and restores
 * the previous clock afterwards.
 *
 * \return 24-bit display ID (0x009341 for a genuine ILI9341), or 0 if the
 *         driver is not initialised.
 */
uint32_t ili9341_read_id(void);

/**
 * \brief Read an 8-bit panel status register over MISO.
 *
 * Suitable for RDDPM (0x0A), RDMADCTL (0x0B), RDDCOLMOD (0x0C) and similar
 * single-parameter registers. The read runs at 6 MHz or lower and the previous
 * clock is restored afterwards. 16 bits are clocked in and decoded with the
 * dummy-bit alignment detected by ili9341_self_test(); call that once first,
 * because modules differ on whether the first clock after the command is a
 * dummy bit.
 *
 * \param cmd Register command, e.g. 0x0B for RDMADCTL.
 * \return Register value, or 0 if the driver is not initialised.
 */
uint8_t ili9341_read_reg8(uint8_t cmd);

/**
 * \brief Probe the panel over MISO and report what the readback shows.
 *
 * Reads RDDID (0x04), RDMADCTL (0x0B) at both possible byte alignments to
 * detect whether 8-bit reads carry a leading dummy bit, then RDDCOLMOD (0x0C)
 * and RDDPM (0x0A). The detected alignment is cached for ili9341_read_reg8()
 * and ili9341_init() uses this function as its liveness check.
 *
 * Readback is optional on many modules: when SDO/MISO is not wired every read
 * floats, miso_alive is false and the function returns false, yet the panel
 * still works write-only.
 *
 * \param out Destination for the full results; must not be NULL.
 * \return true when RDMADCTL matches the programmed orientation, RDDCOLMOD
 *         reports 16 bpp DBI and the panel is awake with DISPON set; false
 *         otherwise.
 */
bool ili9341_self_test(ili9341_selftest_t *out);

/**
 * \brief Set the backlight brightness.
 *
 * \param percent 0 (off) to 100 (full brightness); larger values are clamped.
 */
void ili9341_set_backlight(uint8_t percent);

/**
 * \brief Enter or leave panel sleep mode.
 *
 * \param sleep true for sleep in, false for sleep out.
 */
void ili9341_sleep(bool sleep);

/**
 * \brief Enable or disable display inversion.
 *
 * \param on true for inverted colours.
 */
void ili9341_invert(bool on);

/**
 * \brief Define the vertical scrolling areas.
 *
 * Meaningful along the panel's native 320-pixel axis only.
 *
 * \param top_fixed Rows before the scrolling area.
 * \param scroll_area Height of the scrolling area in rows.
 * \param bottom_fixed Rows after the scrolling area.
 */
void ili9341_set_vertical_scroll(uint16_t top_fixed, uint16_t scroll_area, uint16_t bottom_fixed);

/**
 * \brief Scroll the display to a given line.
 *
 * Meaningful along the panel's native 320-pixel axis only.
 *
 * \param line First visible line of the scrolling area.
 */
void ili9341_scroll_to(uint16_t line);

/**
 * \brief Copy the transfer statistics.
 *
 * \param out Destination; must not be NULL.
 */
void ili9341_get_stats(ili9341_stats_t *out);

/** \brief Zero the transfer statistics. */
void ili9341_reset_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* ILI9341_H */
