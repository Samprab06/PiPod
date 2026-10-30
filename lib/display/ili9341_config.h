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

#ifndef ILI9341_SPI_HZ
#define ILI9341_SPI_HZ 37500000
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
