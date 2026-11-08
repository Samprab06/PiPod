add_executable(touchpad_test main.c)

target_link_libraries(touchpad_test
    input
    ui
    pico_stdlib
)

target_include_directories(touchpad_test PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../pipod)
pipod_apply_pin_header(touchpad_test)

# stdio over UART0 (GP0 TX / GP1 RX), no USB CDC.
pico_enable_stdio_uart(touchpad_test 1)
pico_enable_stdio_usb(touchpad_test 0)

pico_add_extra_outputs(touchpad_test)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/check_firmware_size.cmake)
media_check_firmware_size(touchpad_test)
