/*
 * ILI9341 wiring and panel defaults for the Purdue Proton.
 *
 * Every setting can be overridden with a -D compiler definition or by
 * editing the values here (single source of truth per docs/design.md).
 */

#ifndef ILI9341_CONFIG_H
#define ILI9341_CONFIG_H

#include "hardware/spi.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ILI9341_SPI
#define ILI9341_SPI spi0
#endif

#ifndef ILI9341_PIN_MISO
#define ILI9341_PIN_MISO 32
#endif
#ifndef ILI9341_PIN_CS
#define ILI9341_PIN_CS 33
#endif
#ifndef ILI9341_PIN_SCK
#define ILI9341_PIN_SCK 34
#endif
#ifndef ILI9341_PIN_MOSI
#define ILI9341_PIN_MOSI 35
#endif
#ifndef ILI9341_PIN_DC
#define ILI9341_PIN_DC 36
#endif
#ifndef ILI9341_PIN_RST
#define ILI9341_PIN_RST 37
#endif
#ifndef ILI9341_PIN_BL
#define ILI9341_PIN_BL 38
#endif

/* 75 MHz is clk_peri / 2 at the stock 150 MHz: the fastest rate the PL022 can
 * produce. The panel is specified at 10 MHz, so this is an overclock that the
 * Proton hardware runs reliably with fast pad drive (see docs/performance.md).
 * Runtime rates stay available through ili9341_set_spi_clock(). */
#ifndef ILI9341_SPI_HZ
#define ILI9341_SPI_HZ 75000000
#endif

/* Fast (12 mA, fast slew) pad drive on SCK/MOSI/CS/DC, needed above ~37.5 MHz
 * on the breadboard wiring. ili9341_default_config() enables it; the runtime
 * switch stays available through ili9341_set_pin_drive(). */
#ifndef ILI9341_FAST_PINS
#define ILI9341_FAST_PINS 1
#endif

#ifndef ILI9341_WIDTH
#define ILI9341_WIDTH 320
#endif
#ifndef ILI9341_HEIGHT
#define ILI9341_HEIGHT 240
#endif

#ifdef __cplusplus
}
#endif

#endif /* ILI9341_CONFIG_H */
