# Vendored: no-OS-FatFS-SD-SDIO-SPI-RPi-Pico

- Upstream: https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
- Version: release tag **v3.6.2**
- License: Apache-2.0, see `LICENSE` (upstream ships no separate NOTICE file).
- Fetched with:
  `git clone --depth 1 --branch v3.6.2 <url>` and the `.git` directory removed.

PiPod compiles only the **hardware-SPI** path: FatFs (`src/ff15`), the SPI
block driver (`src/sd_driver/SPI`), `sd_card.c`, and the small utility files
listed in `CMakeLists.txt`. The SDIO/PIO sources, the C++ `FatFsSd` wrapper,
the stdio/file-stream helpers and the examples are present but not compiled.
`spi_only_stubs.c` supplies the two SDIO symbols `sd_card.c` still references;
the runtime configuration in `lib/storage/storage_hw_config.c` only creates an
`SD_IF_SPI` card.

FatFs configuration is `src/include/ffconf.h` as shipped by the vendor
(`FF_USE_LFN = 3`, `FF_LFN_UNICODE = 2` UTF-8 API, `FF_FS_EXFAT = 1`,
`FF_USE_MKFS = 1` but never called by PiPod).
