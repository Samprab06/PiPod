/* Final-PCB-aligned breadboard map: Purdue Proton RP2350B.
 * See docs/breadboard_pinout.md. DAC signals are reserved, not driven. */
#ifndef PIPOD_BREADBOARD_PINS_H
#define PIPOD_BREADBOARD_PINS_H

#define PIPOD_PIN_UART0_TX 0u
#define PIPOD_PIN_UART0_RX 1u
/* SPI0: no MISO, backlight tied to 3V3 (no GPIO). */
#define PIPOD_PIN_DISPLAY_SCK  2u
#define PIPOD_PIN_DISPLAY_MOSI 3u
#define PIPOD_PIN_DISPLAY_DC   4u
#define PIPOD_PIN_DISPLAY_RST  5u
#define PIPOD_PIN_DISPLAY_CS   14u

#define PIPOD_PIN_MPR121_SDA 6u
#define PIPOD_PIN_MPR121_SCL 7u
#define PIPOD_PIN_MPR121_IRQ 12u
#define PIPOD_MPR121_I2C i2c1
#define PIPOD_MPR121_ADDRESS 0x5Au

/* microSD SPI1, no card detect. */
#define PIPOD_PIN_SD_MISO 8u
#define PIPOD_PIN_SD_CS   9u
#define PIPOD_PIN_SD_SCK  10u
#define PIPOD_PIN_SD_MOSI 11u

#define PIPOD_PIN_BTN_ONBOARD_SELECT 21u
#define PIPOD_PIN_BTN_ONBOARD_MENU   26u
/* GP22..GP25 on-board LEDs unused. */

/* Optional TTP223 slider test: left, middle, right. */
#define PIPOD_PIN_TTP223_1 27u
#define PIPOD_PIN_TTP223_2 28u
#define PIPOD_PIN_TTP223_3 29u
#ifndef PIPOD_TOUCH_ACTIVE_HIGH
#define PIPOD_TOUCH_ACTIVE_HIGH 1
#endif

/* TLV320DAC3100: NOT WIRED YET; do not initialise these pins. */
#define PIPOD_PIN_DAC_HEADPHONE_DETECT 13u
#define PIPOD_PIN_DAC_RST             15u
#define PIPOD_PIN_DAC_SDA             16u
#define PIPOD_PIN_DAC_SCL             17u
#define PIPOD_PIN_DAC_I2S_DIN         18u
#define PIPOD_PIN_DAC_I2S_WCLK        19u
#define PIPOD_PIN_DAC_I2S_BCLK        20u

/* Range/collision checks include reserved pins. */
#define PIPOD_PIN_LIST(X) \
    X(PIPOD_PIN_UART0_TX) X(PIPOD_PIN_UART0_RX) \
    X(PIPOD_PIN_DISPLAY_SCK) X(PIPOD_PIN_DISPLAY_MOSI) \
    X(PIPOD_PIN_DISPLAY_DC) X(PIPOD_PIN_DISPLAY_RST) X(PIPOD_PIN_DISPLAY_CS) \
    X(PIPOD_PIN_MPR121_SDA) X(PIPOD_PIN_MPR121_SCL) X(PIPOD_PIN_MPR121_IRQ) \
    X(PIPOD_PIN_SD_MISO) X(PIPOD_PIN_SD_CS) X(PIPOD_PIN_SD_SCK) X(PIPOD_PIN_SD_MOSI) \
    X(PIPOD_PIN_BTN_ONBOARD_SELECT) X(PIPOD_PIN_BTN_ONBOARD_MENU) \
    X(PIPOD_PIN_TTP223_1) X(PIPOD_PIN_TTP223_2) X(PIPOD_PIN_TTP223_3) \
    X(PIPOD_PIN_DAC_HEADPHONE_DETECT) X(PIPOD_PIN_DAC_RST) \
    X(PIPOD_PIN_DAC_SDA) X(PIPOD_PIN_DAC_SCL) X(PIPOD_PIN_DAC_I2S_DIN) \
    X(PIPOD_PIN_DAC_I2S_WCLK) X(PIPOD_PIN_DAC_I2S_BCLK)
#define PIPOD_PIN_COUNT 26u
#define PIPOD_PIN_BIT(p) (1ull << (unsigned)(p))
#define PIPOD_PIN_MASK_ENTRY(p) | PIPOD_PIN_BIT(p)
#define PIPOD_PINS_MASK (0ull PIPOD_PIN_LIST(PIPOD_PIN_MASK_ENTRY))
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define PIPOD_PIN_RANGE_CHECK(p) \
    _Static_assert((unsigned)(p) < 48u, "PIPOD pin outside RP2350B GPIO range");
PIPOD_PIN_LIST(PIPOD_PIN_RANGE_CHECK)
_Static_assert(__builtin_popcountll(PIPOD_PINS_MASK) == (int)PIPOD_PIN_COUNT,
               "PIPOD_PIN_* collision: two signals are assigned to the same GPIO");
#endif
#endif /* PIPOD_BREADBOARD_PINS_H */
