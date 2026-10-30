add_library(ili9341 STATIC ili9341.c)

target_include_directories(ili9341 PUBLIC ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(ili9341 PUBLIC
    pico_stdlib
    hardware_spi
    hardware_dma
    hardware_pwm
    hardware_gpio
)
