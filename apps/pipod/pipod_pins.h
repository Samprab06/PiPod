/**
 * @file pipod_pins.h
 * @brief Resolve the PiPod board pin map for the application.
 *
 * The concrete map is the header named by PIPOD_PINS_HEADER (a compile
 * definition set by the top-level CMakeLists.txt from the PIPOD_PINS cache
 * variable; the default is BREADBOARD ->
 * "pipod_breadboard_pins.h"). A PCB revision adds
 * boards/pipod_pcb_pins.h, sets PIPOD_PINS=PCB in CMake and defines the same
 * PIPOD_PIN_* names; no application source changes.
 *
 * Include this BEFORE the driver headers so a pin override (see below) is in
 * scope for them.
 */

#ifndef PIPOD_PINS_H
#define PIPOD_PINS_H

#ifndef PIPOD_PINS_HEADER
#define PIPOD_PINS_HEADER "pipod_breadboard_pins.h"
#endif

#include PIPOD_PINS_HEADER

/* The map must provide every signal the app uses. */
#ifndef PIPOD_PIN_DISPLAY_CS
#error "PIPOD_PINS_HEADER did not define PIPOD_PIN_DISPLAY_CS"
#endif
#ifndef PIPOD_PIN_SD_MISO
#error "PIPOD_PINS_HEADER did not define PIPOD_PIN_SD_MISO"
#endif
#ifndef PIPOD_PIN_MPR121_SDA
#error "PIPOD_PINS_HEADER did not define PIPOD_PIN_MPR121_SDA"
#endif

/* TTP223 output polarity; boards/pipod_breadboard_pins.h defines the default. */
#ifndef PIPOD_TOUCH_ACTIVE_HIGH
#define PIPOD_TOUCH_ACTIVE_HIGH 1
#endif

#endif /* PIPOD_PINS_H */
