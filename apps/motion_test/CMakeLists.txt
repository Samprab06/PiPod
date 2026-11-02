add_executable(motion_test main.c)

target_link_libraries(motion_test
    ili9341
    gfx
    pico_stdlib
)

# stdio over UART0 (GP0 TX / GP1 RX), no USB CDC.
pico_enable_stdio_uart(motion_test 1)
pico_enable_stdio_usb(motion_test 0)

pico_add_extra_outputs(motion_test)
