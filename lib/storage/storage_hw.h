/**
 * @file storage_hw.h
 * @brief Private glue between storage.c and the vendored SD driver's
 *        hardware configuration object.
 */
#ifndef STORAGE_HW_H
#define STORAGE_HW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Actual SPI clock (Hz), or 0 before the driver is initialized. */
uint32_t storage_hw_spi_hz(void);

/** Change the SPI clock; returns the achieved rate (0 when not initialized). */
uint32_t storage_hw_set_spi_hz(uint32_t hz);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_HW_H */
