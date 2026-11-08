/**
 * @file spi_only_stubs.c
 * @brief SPI-only build of the vendored driver: satisfy two SDIO symbols.
 *
 * sd_card.c keeps the SDIO backend selectable at run time, so it calls
 * sd_sdio_ctor() and rp2040_sdio_get_sd_status() even when the hardware
 * configuration only has an SD_IF_SPI card. PiPod does not compile the
 * PIO/SDIO sources (see CMakeLists.txt), so these stubs keep the SPI-only link
 * complete. Neither is reachable with the configuration in
 * lib/storage/storage_hw_config.c: sd_card_t.type is SD_IF_SPI.
 */
#include <stddef.h>

#include "sd_card.h"

void sd_sdio_ctor(sd_card_t *sd_card_p) {
    (void)sd_card_p; /* never called: the configured card is SD_IF_SPI */
}

bool rp2040_sdio_get_sd_status(sd_card_t *sd_card_p, uint8_t response[64]) {
    (void)sd_card_p;
    (void)response;
    return false;
}
