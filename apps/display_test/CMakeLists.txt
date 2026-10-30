add_executable(display_test main.c)

target_link_libraries(display_test
    ili9341
    pico_stdlib
)

# stdio over UART0 (GP0 TX / GP1 RX), no USB CDC.
pico_enable_stdio_uart(display_test 1)
pico_enable_stdio_usb(display_test 0)

pico_add_extra_outputs(display_test)
