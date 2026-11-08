# lib/storage — microSD on SPI1 (FatFs) for PiPod

Read-only microSD support for the RP2350B "Purdue Proton" breadboard. The
library is carlk3's
[no-OS-FatFS-SD-SDIO-SPI-RPi-Pico](https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)
**v3.6.2** (Apache-2.0), vendored in `third_party/no-OS-FatFS/`, SPI backend
only (no SDIO/PIO). The hardware configuration lives in
`lib/storage/storage_hw_config.c`, outside the vendored tree.

The card is only ever read: this module never calls `f_write()`, `f_mkdir()`,
`f_unlink()` or any other mutating FatFs API.

## Wiring

Use a **3.3 V** microSD breakout (level shifters/5 V modules are not
supported). Keep the wires short and share a solid ground.

| microSD breakout | Proton / RP2350B | Notes |
|---|---|---|
| VCC | 3V3 | 3.3 V only |
| GND | GND | common ground, short lead |
| MISO / DO | GP28 | SPI1 RX, input |
| CS / SS | GP29 | plain GPIO, active low |
| SCK / CLK | GP30 | SPI1 SCK |
| MOSI / DI | GP31 | SPI1 TX |
| CD / card detect | — | not used (`use_card_detect = false`) |

SPI0 is reserved for the display, so the card is on SPI1. The driver
initializes the card at 400 kHz and then switches to **25 MHz**
(`STORAGE_SD_SPI_HZ`). If the wiring is marginal, lower it at build time:

```sh
-DSTORAGE_SD_SPI_HZ=12500000
```

or at run time after mount:

```c
storage_set_spi_hz(12500000);   /* returns the achieved rate */
```

`STORAGE_SD_SPI_MODE` selects SPI mode 0 (default) or 3. A 10 kΩ pull-up from
CS to 3V3 is recommended by the vendor (the card drives CS at power-up); it is
not required for a single card on a short bus but avoids a floating select.

## API

```c
#include "storage.h"
#include "media_library.h"

if (!storage_mount()) {                 /* 400 kHz init, then 25 MHz */
    storage_print_diagnostic();         /* card type, size, root listing */
} else {
    static media_library_t lib;
    /* Scan the whole card (or e.g. "0:/Music"); builds the flash cache once. */
    if (media_library_scan_sd(&lib, STORAGE_ROOT, NULL)) {
        const ui_library_t *ui_lib = media_library_ui(&lib);  /* ui_init() */
    }
}

/* ... playback / UI ... */

media_library_release(&lib);            /* close the open audio file, if any */
storage_unmount();
```

| Function | Purpose |
|---|---|
| `storage_init()` | one-time SPI/card-slot init, does not touch the card |
| `storage_mount()` | idempotent mount on `0:`; initializes the card |
| `storage_unmount()` | unmount; call `media_library_release()` first |
| `storage_mounted()` | mount state |
| `storage_info()` | card type, sectors, capacity, actual SPI clock, CID strings |
| `storage_set_spi_hz()` | change the post-init clock, returns the actual rate |
| `storage_card_type_str()` / `storage_status_str()` / `storage_fr_str()` | printable names |
| `storage_print_diagnostic()` | mount + card info + root listing over UART |

`media_library_scan_sd()` walks the tree recursively (up to
`STORAGE_FS_MAX_DEPTH = 12`), accepts `.mp3`, `.flac`, `.wav`, `.ogg`, `.opus`
(case-insensitive), and reuses the same metadata/art parser and flash cache as
the onboard flash-pack scan. Second boot with the same card is a cache hit: it
walks the directories to compute the key (path + size + FAT modification time
of every audio file) but parses no tags and decodes no art.

Limits: 512 tracks / 128 albums / 128 artists (same as the flash pack); the
scan stops after `MEDIA_SD_MAX_FILES = 4096` audio files. `stats.skipped`
counts unsupported/corrupt/over-capacity files, `stats.files_ignored` counts
non-audio files, `stats.file_cap_hit` reports the file cap. Very long paths
(>= 256 bytes) and directory trees deeper than 12 levels are skipped.

`storage_fs.h` is the small read-only filesystem interface used by
`lib/media`; `storage_fs.c` implements it with FatFs, and
`lib/media/host/storage_fs_host.c` with stdio for the host tests.

## Card format

* **FAT32** is the recommended format (the default for SDHC/SDXC cards up to
  2 TB). **exFAT is supported** (`FF_FS_EXFAT = 1`, `FF_USE_LFN = 3`, LFN API
  in UTF-8). FAT12/FAT16 also mount but are not needed for music cards.
* One volume on a **MBR** partition table (or a superfloppy with no partition
  table). The driver mounts the first volume as drive `0:`.
* Long file names up to 255 UTF-8 characters; non-ASCII names work as long as
  the host wrote them as UTF-8 (the FatFs API is configured for
  `FF_LFN_UNICODE = 2`).
* No journaling/replay: pull the card or power off only when no file is open.

## Resource cost

Measured on `pipod` (rp2350-arm-s, Release) as the delta between a build with
and without a reference to `media_library_scan_sd()` (i.e. when the integration
app actually links and uses the SD path):

| | |
|---|---|
| flash (`.text` + `.rodata`) | ≈ 81 KiB (81,304 B binary delta) |
| static RAM (`.data` + `.bss`) | ≈ 9 KiB (9,012 B) |

Most of the flash is FatFs (`ff.c` ≈ 24 KB) plus the SPI/DMA driver, the
SD data-token CRC table, and newlib bits pulled in by the vendored driver's
printf diagnostics. Static RAM: two 512-byte read caches + two `FIL` objects
(≈ 2.2 KB), the directory-walk path stack and `FILINFO` (≈ 3.7 KB), the FATFS
object (≈ 0.6 KB), the LFN working buffer (≈ 0.3 KB) and 2 KB of path offsets
in the media scan work area.

No heap is used after `storage_init()`; FatFs's LFN buffer is in `.bss`
(`FF_USE_LFN = 3`).

## Known limitations / unverified

* No card has been read on real hardware with this tree yet: the SPI wiring and
  25 MHz clock are from the team's breadboard pin map, not measured.
* No card-detect pin: hot-plug removal is not detected. Unmount before
  removing the card.
* One card, one volume, logical drive `0:`.
* `STORAGE_FS_MAX_OPEN = 2` file handles: one for the scanner, one for the
  player. Call `media_library_release()` before unmounting or scanning another
  library while a track source is still open.
