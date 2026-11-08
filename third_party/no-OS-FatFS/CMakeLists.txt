# no-OS-FatFS-SD-SDIO-SPI-RPi-Pico v3.6.2 (Apache-2.0), vendored for PiPod.
#
# The upstream project ships an INTERFACE target that always compiles both the
# SDIO (PIO) and SPI backends. PiPod only uses the hardware-SPI path, so this
# wrapper builds FatFs plus the SPI driver and skips the SDIO sources (see
# lib/storage/storage_sdio_stub.c for the two unused SDIO symbols sd_card.c
# still references). Configuration lives in src/include/ffconf.h (FatFs LFN
# UTF-8, exFAT on) and in the application's hw_config implementation
# (lib/storage/storage_hw_config.c).
add_library(fatfs_sd STATIC
    ${CMAKE_CURRENT_LIST_DIR}/src/ff15/source/ff.c
    ${CMAKE_CURRENT_LIST_DIR}/src/ff15/source/ffsystem.c
    ${CMAKE_CURRENT_LIST_DIR}/src/ff15/source/ffunicode.c
    ${CMAKE_CURRENT_LIST_DIR}/src/sd_driver/sd_card.c
    ${CMAKE_CURRENT_LIST_DIR}/src/sd_driver/sd_timeouts.c
    ${CMAKE_CURRENT_LIST_DIR}/src/sd_driver/SPI/my_spi.c
    ${CMAKE_CURRENT_LIST_DIR}/src/sd_driver/SPI/sd_card_spi.c
    ${CMAKE_CURRENT_LIST_DIR}/src/sd_driver/SPI/sd_spi.c
    ${CMAKE_CURRENT_LIST_DIR}/src/src/crc.c
    ${CMAKE_CURRENT_LIST_DIR}/src/src/f_util.c
    ${CMAKE_CURRENT_LIST_DIR}/src/src/glue.c
    ${CMAKE_CURRENT_LIST_DIR}/src/src/my_debug.c
    ${CMAKE_CURRENT_LIST_DIR}/src/src/my_rtc.c
    ${CMAKE_CURRENT_LIST_DIR}/src/src/util.c
    # SPI-only build: stubs for the two SDIO symbols sd_card.c references.
    ${CMAKE_CURRENT_LIST_DIR}/spi_only_stubs.c
)

target_include_directories(fatfs_sd PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/src/include
    ${CMAKE_CURRENT_LIST_DIR}/src/sd_driver
    ${CMAKE_CURRENT_LIST_DIR}/src/sd_driver/SPI
    ${CMAKE_CURRENT_LIST_DIR}/src/ff15/source
    ${CMAKE_CURRENT_LIST_DIR}/src/include
)

target_compile_definitions(fatfs_sd PUBLIC
    USE_PRINTF=1
)
target_compile_definitions(fatfs_sd PRIVATE
    PICO_MAX_SHARED_IRQ_HANDLERS=8u
)

# Third-party code only: silence its warnings so the project-wide -Wall -Wextra
# keeps meaning something for our own targets.
target_compile_options(fatfs_sd PRIVATE -w)

target_link_libraries(fatfs_sd PUBLIC
    hardware_dma
    hardware_pio
    hardware_spi
    pico_aon_timer
    pico_stdlib
    pico_time
    cmsis_core
)
