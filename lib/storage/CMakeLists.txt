# microSD card on SPI1 (FatFs + read-only storage_fs interface).
add_library(storage STATIC
    storage.c
    storage_fs.c
    storage_hw_config.c
)

target_include_directories(storage PUBLIC ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(storage PUBLIC
    fatfs_sd
    hardware_spi
    pico_stdlib
)

# Route the card pins from the project-wide board header when one is selected
# (storage_hw_config.c then uses PIPOD_PIN_SD_*); the driver defaults remain.
if(COMMAND pipod_apply_pin_header)
    pipod_apply_pin_header(storage)
endif()

target_compile_options(storage PRIVATE -O2)
