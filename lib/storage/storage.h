/**
 * @file storage.h
 * @brief microSD card on SPI1: mount/unmount, card info, diagnostics.
 *
 * Hardware (Purdue Proton breadboard, see lib/storage/README.md):
 *
 * @code
 *   microSD 3.3 V breakout        RP2350B
 *   ----------------------        -------
 *   VCC  (3.3 V only!)      ->    3V3
 *   GND                     ->    GND
 *   MISO / DO               ->    GP28  (SPI1 RX)
 *   CS   / SS               ->    GP29  (plain GPIO, active low)
 *   SCK  / CLK              ->    GP30  (SPI1 SCK)
 *   MOSI / DI               ->    GP31  (SPI1 TX)
 *   CD / card detect        ->    not connected (no card-detect pin)
 * @endcode
 *
 * The card is initialized at 400 kHz by the vendored driver
 * (third_party/no-OS-FatFS, carlk3's no-OS-FatFS v3.6.2, Apache-2.0) and then
 * switched to STORAGE_SD_SPI_HZ (default 25 MHz, overridable at compile time
 * with -DSTORAGE_SD_SPI_HZ=... and at run time with storage_set_spi_hz()).
 *
 * Everything here is read-only: the library never calls f_write(), f_mkdir()
 * or any other mutating FatFs API.
 *
 * RAM: the FATFS object (~560 B) and the mounted card state are static.
 * Flash: FatFs + the SPI driver are about 20 KB (see lib/storage/README.md).
 */
#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Logical FatFs drive the card is mounted on ("0:"). */
#define STORAGE_DRIVE "0:"
/** Root path handed to the scanners when the caller has no preference. */
#define STORAGE_ROOT STORAGE_DRIVE "/"

/**
 * SPI clock used after card initialization. The card itself is initialized at
 * 400 kHz. 25 MHz is the team's ceiling for the breadboard breakout; lower it
 * with -DSTORAGE_SD_SPI_HZ=... or at run time if the wiring is marginal.
 */
#ifndef STORAGE_SD_SPI_HZ
#define STORAGE_SD_SPI_HZ 25000000u
#endif

/** SPI mode used for the card (0 or 3; both are legal for SD cards). */
#ifndef STORAGE_SD_SPI_MODE
#define STORAGE_SD_SPI_MODE 0
#endif

/** High-level storage result codes (negative = error). */
typedef enum {
    STORAGE_OK = 0,
    STORAGE_ERR_NO_CARD = -1,     /**< no card answered on the SPI bus */
    STORAGE_ERR_MOUNT = -2,       /**< the card answered but mount failed */
    STORAGE_ERR_NOT_MOUNTED = -3, /**< a file operation ran before mount */
    STORAGE_ERR_IO = -4,          /**< read/seek error */
    STORAGE_ERR_NO_SLOT = -5,     /**< too many files open (STORAGE_FS_MAX_OPEN) */
    STORAGE_ERR_RANGE = -6,       /**< offset/size outside the file */
    STORAGE_ERR_UNSUPPORTED = -7, /**< unsupported filesystem/feature */
} storage_status_t;

/** Card description, filled by storage_mount(). */
typedef struct {
    bool     mounted;
    uint8_t  card_type;      /**< card_type_t: V1 / V2 / V2HC / unknown */
    uint32_t sector_count;   /**< 512-byte sectors */
    uint64_t capacity_bytes;
    uint32_t spi_hz;         /**< actual SPI clock after the high-speed switch */
    char     product[8];     /**< CID product name, "" when unknown */
    char     manufacturer[4];/**< CID manufacturer, "" when unknown */
} storage_info_t;

/**
 * @brief Initialize the driver (SPI + card slot), once.
 *
 * Does not touch the card. Safe to call repeatedly.
 * @return false when the driver could not be initialized (usually a pin clash).
 */
bool storage_init(void);

/**
 * @brief Mount the card (idempotent).
 *
 * Initializes the card at 400 kHz, mounts the first FAT32/exFAT/FAT16 volume
 * on "0:" and switches the bus to STORAGE_SD_SPI_HZ. Read-only.
 *
 * @return false on error; call storage_print_diagnostic() for details.
 */
bool storage_mount(void);

/** @brief Unmount and power down the card interface ("0:"). Fails quietly. */
void storage_unmount(void);

/** @brief True when a volume is mounted on "0:". */
bool storage_mounted(void);

/**
 * @brief Last mount result. Always non-NULL.
 *
 * The values stay valid after storage_unmount(); check `mounted`.
 */
const storage_info_t *storage_info(void);

/**
 * @brief Change the SPI clock after mount (does not remount).
 * @return the achieved clock in Hz (0 on error / when not initialized).
 */
uint32_t storage_set_spi_hz(uint32_t hz);

/** @brief "SD V2HC", "SD V2", "SD V1", "none", "unknown". */
const char *storage_card_type_str(uint8_t card_type);

/** @brief Human-readable storage_status_t text. */
const char *storage_status_str(int status);

/** @brief Human-readable FatFs FRESULT text ("FR_OK", "FR_NO_FILESYSTEM", ...). */
const char *storage_fr_str(int fr);

/** FatFs result of the last mount attempt (FRESULT as int; see storage_fr_str()). */
int storage_last_fr(void);

/**
 * @brief Print a mount/card/root-directory diagnostic on the UART.
 *
 * Mounts the card if needed, prints the card type, capacity, SPI clock, the
 * FatFs configuration and a short root-directory listing. Never writes.
 */
void storage_print_diagnostic(void);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_H */
