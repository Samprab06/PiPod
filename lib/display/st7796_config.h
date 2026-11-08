/*
 * ST7796S wiring and panel defaults for the 3.5-inch XIITIA module.
 *
 * The module has nine pins: GND, VCC, SCL (SPI clock), SDA (MOSI), RST, DC,
 * CS, BL and SDA-0 (unused). There is NO MISO: the panel cannot be read back,
 * so any read or self-test reports "unsupported". BL is tied to 3.3 V on the
 * breadboard, so backlight control is a no-op.
 *
 * The control lines share the ILI9341 build's pinout (SPI0, CS GP33, SCK GP34,
 * MOSI GP35, DC GP36, RST GP37); only MISO GP32 and BL PWM GP38 have no
 * counterpart on this module.
 *
 * Every setting can be overridden with a -D compiler definition.
 */

#ifndef ST7796_CONFIG_H
#define ST7796_CONFIG_H

#include "hardware/spi.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ST7796_SPI
#define ST7796_SPI spi0
#endif

#ifndef ST7796_PIN_CS
#define ST7796_PIN_CS 33
#endif
#ifndef ST7796_PIN_SCK
#define ST7796_PIN_SCK 34
#endif
#ifndef ST7796_PIN_MOSI
#define ST7796_PIN_MOSI 35
#endif
#ifndef ST7796_PIN_DC
#define ST7796_PIN_DC 36
#endif
#ifndef ST7796_PIN_RST
#define ST7796_PIN_RST 37
#endif

/* Conservative default: 37.5 MHz = clk_peri (150 MHz) / 4, a divider the
 * PL022 produces exactly. Achievable rates are clk_peri divided by an even
 * divisor (75, 37.5, 25, 18.75, 15 MHz, ...; see st7796_set_spi_clock()).
 *
 * 75 MHz (clk_peri / 2) may work and is the rate the ILI9341 build uses, but
 * the ST7796 module has not been characterised on this wiring: raise it only
 * after a long-run test with fast pad drive (ST7796_FAST_PINS). The vendor
 * manual gives no SPI timing table, so nothing above 37.5 MHz is assumed. */
#ifndef ST7796_SPI_HZ
#define ST7796_SPI_HZ 37500000
#endif

/* Fast (12 mA, fast slew) pad drive on SCK/MOSI/CS/DC. The pads are shared
 * with the ILI9341 build, which needs this above ~37.5 MHz on the breadboard
 * wiring; keeping it on costs nothing at the conservative clock. */
#ifndef ST7796_FAST_PINS
#define ST7796_FAST_PINS 1
#endif

/* Default (landscape) resolution in pixels. The panel is natively 320x480
 * portrait; this config follows the ILI9341 build's convention of storing the
 * dimensions of the default landscape rotation, so the portrait rotation
 * swaps them. */
#ifndef ST7796_WIDTH
#define ST7796_WIDTH 480
#endif
#ifndef ST7796_HEIGHT
#define ST7796_HEIGHT 320
#endif

#ifdef __cplusplus
}
#endif

#endif /* ST7796_CONFIG_H */
