add_executable(spi_overclock main.c)

target_link_libraries(spi_overclock
    ili9341
    gfx
    pico_stdlib
    hardware_clocks
    hardware_pll
)

# stdio over UART0 (GP0 TX / GP1 RX), no USB CDC: USB is not usable once
# PLL_USB is reprogrammed for the clk_peri overclock.
pico_enable_stdio_uart(spi_overclock 1)
pico_enable_stdio_usb(spi_overclock 0)

pico_add_extra_outputs(spi_overclock)
