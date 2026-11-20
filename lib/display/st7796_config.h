/*
 * ST7796S wiring and panel defaults for the 3.5-inch XIITIA module.
 *
 * The module has nine pins: GND, VCC, SCL (SPI clock), SDA (MOSI), RST, DC,
 * CS, BL and SDA-0 (unused). There is NO MISO: the panel cannot be read back,
 * so any read or self-test reports "unsupported". BL is tied to 3.3 V on the
 * breadboard, so backlight control is a no-op.
 *
 * Control lines follow the selected PiPod board header (SPI0).
 *
 * Every setting can be overridden with a -D compiler definition.
 */

#ifndef ST7796_CONFIG_H
#define ST7796_CONFIG_H

#include "hardware/spi.h"
#ifdef PIPOD_PINS_HEADER
#include PIPOD_PINS_HEADER
#else
#include "../../boards/pipod_breadboard_pins.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ST7796_SPI
#define ST7796_SPI spi0
#endif

#ifndef ST7796_PIN_CS
#define ST7796_PIN_CS PIPOD_PIN_DISPLAY_CS
#endif
#ifndef ST7796_PIN_SCK
#define ST7796_PIN_SCK PIPOD_PIN_DISPLAY_SCK
#endif
#ifndef ST7796_PIN_MOSI
#define ST7796_PIN_MOSI PIPOD_PIN_DISPLAY_MOSI
#endif
#ifndef ST7796_PIN_DC
#define ST7796_PIN_DC PIPOD_PIN_DISPLAY_DC
#endif
#ifndef ST7796_PIN_RST
#define ST7796_PIN_RST PIPOD_PIN_DISPLAY_RST
#endif

/* 75 MHz = clk_peri (150 MHz) / 2. This is an overclock, far above the
 * ST7796 rated serial clock: verify signal integrity on the actual breadboard.
 * Settings -> Display Clock offers 37.5 MHz fallback. SPI1/SD has its own
 * PL022 divider and remains at 25 MHz (or its mount fallback rate). */
#ifndef ST7796_SPI_HZ
#define ST7796_SPI_HZ 75000000
#endif

/* Fast (12 mA, fast slew) pad drive on SCK/MOSI/CS/DC. The pads are shared
 * with the ILI9341 build, which needs this above ~37.5 MHz on the breadboard
 * wiring; required for the 75 MHz overclock. */
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
