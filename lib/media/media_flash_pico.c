/**
 * @file media_flash_pico.c
 * @brief RP2350 implementation of the flash erase/program primitives.
 *
 * Only flash_safe_execute() is used: it disables interrupts on this core and
 * parks the other core so no code (or DMA) is reading XIP while the sector is
 * erased/reprogrammed. The other core must have called
 * flash_safe_execute_core_init() (see docs/media_design.md "Flash writes").
 *
 * This file is empty on host builds; the simulator backend lives in
 * lib/media/host/media_flash_host.c.
 */
#include "flash_layout.h"

#if defined(PICO_ON_DEVICE)

#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/flash.h"
#include "pico/multicore.h"

/* Generous: one 4 KB sector erase is a few ms; the timeout only covers the
 * handshake with the other core. */
#define MEDIA_FLASH_SAFE_TIMEOUT_MS 2000u

typedef struct {
    uint32_t offset;
    uint32_t size;
    const uint8_t *data;
} media_flash_job_t;

/* Flash writes are serialised by design (scan-time cache writes only). */
static media_flash_job_t s_job;

static void media_flash_erase_cb(void *param) {
    (void)param;
    flash_range_erase(s_job.offset, s_job.size);
}

static void media_flash_program_cb(void *param) {
    (void)param;
    flash_range_program(s_job.offset, s_job.data, s_job.size);
}

/**
 * flash_safe_execute() parks the other core through multicore_lockout, but a
 * core that has not been launched cannot register as a lockout victim, so the
 * SDK returns PICO_ERROR_NOT_PERMITTED on a single-core boot. The library
 * contract writes the cache only during the boot scan, before core 1 exists
 * (apps/pipod/main.c does the scan before multicore_launch_core1), so in that
 * window disabling interrupts here is equivalent and safe. If core 1 is
 * running it has called flash_safe_execute_core_init() and lockout_ready() is
 * true, so the SDK path below is used.
 */
static bool media_flash_execute(void (*cb)(void *)) {
    if (!multicore_lockout_ready()) {
        uint32_t save = save_and_disable_interrupts();
        cb(NULL);
        restore_interrupts(save);
        return true;
    }
    return flash_safe_execute(cb, NULL, MEDIA_FLASH_SAFE_TIMEOUT_MS) == PICO_OK;
}

bool media_flash_erase(uint32_t offset, uint32_t size) {
    if (size == 0u || (offset & (FLASH_SECTOR_SIZE - 1u)) != 0u ||
        (size & (FLASH_SECTOR_SIZE - 1u)) != 0u) {
        return false;
    }
    if (offset > MEDIA_FLASH_SIZE || size > MEDIA_FLASH_SIZE - offset) {
        return false;
    }
    s_job.offset = offset;
    s_job.size = size;
    s_job.data = NULL;
    return media_flash_execute(media_flash_erase_cb);
}

bool media_flash_program(uint32_t offset, const void *data, uint32_t size) {
    if (data == NULL || size == 0u ||
        (offset & (FLASH_PAGE_SIZE - 1u)) != 0u ||
        (size & (FLASH_PAGE_SIZE - 1u)) != 0u) {
        return false;
    }
    if (offset > MEDIA_FLASH_SIZE || size > MEDIA_FLASH_SIZE - offset) {
        return false;
    }
    s_job.offset = offset;
    s_job.size = size;
    s_job.data = (const uint8_t *)data;
    return media_flash_execute(media_flash_program_cb);
}

#else

/* Keep the translation unit non-empty on host builds (it is never compiled
 * into host tests, but globbing host builders must not choke on it). */
typedef int media_flash_pico_unused_t;

#endif /* PICO_ON_DEVICE */
