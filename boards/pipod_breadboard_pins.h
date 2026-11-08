/*
 * PiPod breadboard pin map (Purdue Proton, RP2350B).
 *
 * This is the single source of truth for the pins the PiPod *application*
 * uses. Apps include it through apps/pipod/pipod_pins.h, which resolves the
 * header named by PIPOD_PINS_HEADER (see the top-level CMakeLists.txt). A PCB
 * build can therefore drop in boards/pipod_pcb_pins.h and select
 * -DPIPOD_PINS=PCB without touching any application source.
 *
 * The library-level defaults are deliberately left alone:
 *   - lib/display/ili9341_config.h / st7796_config.h keep the breadboard pins
 *     as their #ifndef defaults; apps/pipod/main.c overrides every pin field of
 *     the runtime display config from these macros.
 *   - lib/storage/storage_hw_config.c uses STORAGE_SD_PIN_* which fall back to
 *     these same numbers when no PIPOD_PINS_HEADER is supplied.
 *   - lib/input keeps its own defaults; the app passes the touchpad map and the
 *     button map explicitly.
 *
 * Approved breadboard map (do not change without updating
 * docs/breadboard_pinout.md):
 *
 *   UART0 stdio ............ GP0 TX, GP1 RX (on-board debug probe)
 *   Display SPI0 ........... CS GP33, SCK GP34, MOSI GP35, DC GP36, RST GP37
 *                            ILI9341 only: MISO GP32, BL PWM GP38
 *   microSD SPI1 ........... MISO GP28, CS GP29, SCK GP30, MOSI GP31
 *   TTP223 slider .......... LEFT GP2, MIDDLE GP3, RIGHT GP4
 *   TTP223 buttons ......... SELECT GP5, MENU GP6, PLAY/PAUSE GP7
 *   Physical buttons ....... PREV GP8, NEXT GP9
 *   On-board Proton buttons  SELECT GP21, MENU GP26
 *   On-board LEDs ........... GP22..GP25 (unused by PiPod)
 */

#ifndef PIPOD_BREADBOARD_PINS_H
#define PIPOD_BREADBOARD_PINS_H

/* --- UART0 (stdio over the on-board debug probe) ----------------------- */
#define PIPOD_PIN_UART0_TX 0u
#define PIPOD_PIN_UART0_RX 1u

/* --- TTP223 slider (left -> right) ------------------------------------- */
#define PIPOD_PIN_TOUCH_LEFT       2u
#define PIPOD_PIN_TOUCH_MIDDLE     3u
#define PIPOD_PIN_TOUCH_RIGHT      4u

/* --- TTP223 buttons ---------------------------------------------------- */
#define PIPOD_PIN_TOUCH_SELECT     5u
#define PIPOD_PIN_TOUCH_MENU       6u
#define PIPOD_PIN_TOUCH_PLAY_PAUSE 7u

/* --- Physical buttons (active high, external pull-down) ---------------- */
#define PIPOD_PIN_BTN_PREV 8u
#define PIPOD_PIN_BTN_NEXT 9u

/* --- On-board Proton buttons (active high, on-board pull-down) --------- */
#define PIPOD_PIN_BTN_ONBOARD_SELECT 21u
#define PIPOD_PIN_BTN_ONBOARD_MENU   26u

/* --- microSD on SPI1 (3.3 V module, no card detect) -------------------- */
#define PIPOD_PIN_SD_MISO 28u
#define PIPOD_PIN_SD_CS   29u
#define PIPOD_PIN_SD_SCK  30u
#define PIPOD_PIN_SD_MOSI 31u

/* --- Display on SPI0 --------------------------------------------------- */
#define PIPOD_PIN_DISPLAY_MISO 32u /* ILI9341 only; ST7796 has no MISO */
#define PIPOD_PIN_DISPLAY_CS   33u
#define PIPOD_PIN_DISPLAY_SCK  34u
#define PIPOD_PIN_DISPLAY_MOSI 35u
#define PIPOD_PIN_DISPLAY_DC   36u
#define PIPOD_PIN_DISPLAY_RST  37u
#define PIPOD_PIN_DISPLAY_BL   38u /* ILI9341 only; ST7796 BL is tied to 3V3 */

/*
 * TTP223 output polarity. With the modules' default jumpers the output is
 * push-pull, high while touched (momentary, active high). If a module is
 * configured the other way, build with -DPIPOD_TOUCH_ACTIVE_HIGH=0.
 */
#ifndef PIPOD_TOUCH_ACTIVE_HIGH
#define PIPOD_TOUCH_ACTIVE_HIGH 1
#endif

/**
 * Number of distinct GPIOs named above.
 *
 * The mask covers every PIPOD_PIN_* macro. The _Static_assert below fails the
 * build when two signals land on the same GPIO, which is the one wiring bug
 * that is otherwise invisible until bring-up.
 */
#define PIPOD_PIN_COUNT 23u

#define PIPOD_PIN_BIT(p) (1ull << (unsigned)(p))
#define PIPOD_PINS_MASK                                                                     \
    (PIPOD_PIN_BIT(PIPOD_PIN_UART0_TX) | PIPOD_PIN_BIT(PIPOD_PIN_UART0_RX) |                \
     PIPOD_PIN_BIT(PIPOD_PIN_TOUCH_LEFT) | PIPOD_PIN_BIT(PIPOD_PIN_TOUCH_MIDDLE) |           \
     PIPOD_PIN_BIT(PIPOD_PIN_TOUCH_RIGHT) | PIPOD_PIN_BIT(PIPOD_PIN_TOUCH_SELECT) |          \
     PIPOD_PIN_BIT(PIPOD_PIN_TOUCH_MENU) | PIPOD_PIN_BIT(PIPOD_PIN_TOUCH_PLAY_PAUSE) |       \
     PIPOD_PIN_BIT(PIPOD_PIN_BTN_PREV) | PIPOD_PIN_BIT(PIPOD_PIN_BTN_NEXT) |                 \
     PIPOD_PIN_BIT(PIPOD_PIN_BTN_ONBOARD_SELECT) | PIPOD_PIN_BIT(PIPOD_PIN_BTN_ONBOARD_MENU) |\
     PIPOD_PIN_BIT(PIPOD_PIN_SD_MISO) | PIPOD_PIN_BIT(PIPOD_PIN_SD_CS) |                     \
     PIPOD_PIN_BIT(PIPOD_PIN_SD_SCK) | PIPOD_PIN_BIT(PIPOD_PIN_SD_MOSI) |                    \
     PIPOD_PIN_BIT(PIPOD_PIN_DISPLAY_MISO) | PIPOD_PIN_BIT(PIPOD_PIN_DISPLAY_CS) |           \
     PIPOD_PIN_BIT(PIPOD_PIN_DISPLAY_SCK) | PIPOD_PIN_BIT(PIPOD_PIN_DISPLAY_MOSI) |          \
     PIPOD_PIN_BIT(PIPOD_PIN_DISPLAY_DC) | PIPOD_PIN_BIT(PIPOD_PIN_DISPLAY_RST) |            \
     PIPOD_PIN_BIT(PIPOD_PIN_DISPLAY_BL))

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
/* RP2350B has 48 GPIOs: 0..47. */
#define PIPOD_PIN_VALID(p) ((unsigned)(p) < 48u)
_Static_assert(PIPOD_PIN_VALID(PIPOD_PIN_UART0_TX) &&
                   PIPOD_PIN_VALID(PIPOD_PIN_UART0_RX) &&
                   PIPOD_PIN_VALID(PIPOD_PIN_TOUCH_LEFT) &&
                   PIPOD_PIN_VALID(PIPOD_PIN_TOUCH_MIDDLE) &&
                   PIPOD_PIN_VALID(PIPOD_PIN_TOUCH_RIGHT) &&
                   PIPOD_PIN_VALID(PIPOD_PIN_TOUCH_SELECT) &&
                   PIPOD_PIN_VALID(PIPOD_PIN_TOUCH_MENU) &&
                   PIPOD_PIN_VALID(PIPOD_PIN_TOUCH_PLAY_PAUSE) &&
                   PIPOD_PIN_VALID(PIPOD_PIN_BTN_PREV) &&
                   PIPOD_PIN_VALID(PIPOD_PIN_BTN_NEXT) &&
                   PIPOD_PIN_VALID(PIPOD_PIN_BTN_ONBOARD_SELECT) &&
                   PIPOD_PIN_VALID(PIPOD_PIN_BTN_ONBOARD_MENU) &&
                   PIPOD_PIN_VALID(PIPOD_PIN_SD_MISO) &&
                   PIPOD_PIN_VALID(PIPOD_PIN_SD_CS) &&
                   PIPOD_PIN_VALID(PIPOD_PIN_SD_SCK) &&
                   PIPOD_PIN_VALID(PIPOD_PIN_SD_MOSI) &&
                   PIPOD_PIN_VALID(PIPOD_PIN_DISPLAY_MISO) &&
                   PIPOD_PIN_VALID(PIPOD_PIN_DISPLAY_CS) &&
                   PIPOD_PIN_VALID(PIPOD_PIN_DISPLAY_SCK) &&
                   PIPOD_PIN_VALID(PIPOD_PIN_DISPLAY_MOSI) &&
                   PIPOD_PIN_VALID(PIPOD_PIN_DISPLAY_DC) &&
                   PIPOD_PIN_VALID(PIPOD_PIN_DISPLAY_RST) &&
                   PIPOD_PIN_VALID(PIPOD_PIN_DISPLAY_BL),
               "PIPOD_PIN_* value outside the RP2350B GPIO range 0..47");
_Static_assert(__builtin_popcountll(PIPOD_PINS_MASK) == (int)PIPOD_PIN_COUNT,
               "PIPOD_PIN_* collision: two signals are assigned to the same GPIO");
#endif

#endif /* PIPOD_BREADBOARD_PINS_H */
