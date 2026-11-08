add_executable(sd_test main.c)

target_link_libraries(sd_test
    storage
    media
    pico_stdlib
)

target_include_directories(sd_test PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../pipod)
pipod_apply_pin_header(sd_test)

# stdio over UART0 (GP0 TX / GP1 RX), no USB CDC.
pico_enable_stdio_uart(sd_test 1)
pico_enable_stdio_usb(sd_test 0)

pico_add_extra_outputs(sd_test)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/check_firmware_size.cmake)
media_check_firmware_size(sd_test)
