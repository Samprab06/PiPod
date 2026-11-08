/**
 * @file storage_hw_config.c
 * @brief Hardware configuration for carlk3's no-OS-FatFS SD driver.
 *
 * This is the application-side "hw_config" the vendored library asks for
 * (sd_get_num / sd_get_by_num). It is deliberately outside third_party/ so the
 * vendored tree stays untouched.
 *
 * Proton breadboard wiring (see lib/storage/README.md):
 *   SPI1, MISO GP28, CS GP29, SCK GP30, MOSI GP31, no card-detect pin.
 *
 * The pins are STORAGE_SD_PIN_* below. When the application defines
 * PIPOD_PINS_HEADER (the PiPod apps do, through
 * pipod_apply_pin_header() in the top-level CMakeLists.txt) the values come
 * from that board header; otherwise they keep the breadboard defaults. The
 * vendored driver is untouched either way.
 *
 * The SPI bus is initialized at 100 kHz / 400 kHz by the driver and then
 * switched to STORAGE_SD_SPI_HZ (25 MHz by default).
 */
#include <stddef.h>
#include <stdint.h>

#include "hardware/spi.h"
#include "hw_config.h"
#include "sd_card.h"
#include "storage.h"
#include "storage_hw.h"

#if defined(PIPOD_PINS_HEADER)
#include PIPOD_PINS_HEADER
#endif

#ifndef STORAGE_SD_PIN_MISO
#if defined(PIPOD_PIN_SD_MISO)
#define STORAGE_SD_PIN_MISO PIPOD_PIN_SD_MISO
#else
#define STORAGE_SD_PIN_MISO 28
#endif
#endif
#ifndef STORAGE_SD_PIN_CS
#if defined(PIPOD_PIN_SD_CS)
#define STORAGE_SD_PIN_CS PIPOD_PIN_SD_CS
#else
#define STORAGE_SD_PIN_CS 29
#endif
#endif
#ifndef STORAGE_SD_PIN_SCK
#if defined(PIPOD_PIN_SD_SCK)
#define STORAGE_SD_PIN_SCK PIPOD_PIN_SD_SCK
#else
#define STORAGE_SD_PIN_SCK 30
#endif
#endif
#ifndef STORAGE_SD_PIN_MOSI
#if defined(PIPOD_PIN_SD_MOSI)
#define STORAGE_SD_PIN_MOSI PIPOD_PIN_SD_MOSI
#else
#define STORAGE_SD_PIN_MOSI 31
#endif
#endif

static spi_t s_spi = {
    .hw_inst = spi1,
    .miso_gpio = STORAGE_SD_PIN_MISO,
    .mosi_gpio = STORAGE_SD_PIN_MOSI,
    .sck_gpio = STORAGE_SD_PIN_SCK,
    .baud_rate = STORAGE_SD_SPI_HZ,
    .spi_mode = STORAGE_SD_SPI_MODE,
    .no_miso_gpio_pull_up = false,
    .set_drive_strength = false,
    .use_static_dma_channels = false,
};

static sd_spi_if_t s_spi_if = {
    .spi = &s_spi,
    .ss_gpio = STORAGE_SD_PIN_CS, /* plain GPIO, active low; no card detect */
    .set_drive_strength = false,
};

static sd_card_t s_sd_card = {
    .type = SD_IF_SPI,
    .spi_if_p = &s_spi_if,
    .use_card_detect = false,
    .card_detect_gpio = 0,
    .card_detected_true = 0,
    .card_detect_use_pull = false,
    .card_detect_pull_hi = false,
};

size_t sd_get_num(void) {
    return 1;
}

sd_card_t *sd_get_by_num(size_t num) {
    return num == 0u ? &s_sd_card : NULL;
}

uint32_t storage_hw_spi_hz(void) {
    if (!s_spi.initialized) {
        return 0;
    }
    return spi_get_baudrate(s_spi.hw_inst);
}

uint32_t storage_hw_set_spi_hz(uint32_t hz) {
    if (hz == 0u) {
        return 0;
    }
    s_spi.baud_rate = hz;
    if (!s_spi.initialized) {
        return hz;
    }
    return spi_set_baudrate(s_spi.hw_inst, hz);
}
