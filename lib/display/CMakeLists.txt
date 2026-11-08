# Both panel drivers are always built; the firmware selects one at compile time
# with PIPOD_DISPLAY_ST7796 (see display.h). Only symbols that are actually
# referenced are pulled out of these static archives, so linking both costs no
# flash space.
add_library(ili9341 STATIC ili9341.c)

target_include_directories(ili9341 PUBLIC ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(ili9341 PUBLIC
    pico_stdlib
    hardware_spi
    hardware_dma
    hardware_pwm
    hardware_gpio
    flashstream
)

add_library(st7796 STATIC st7796.c)

target_include_directories(st7796 PUBLIC ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(st7796 PUBLIC
    pico_stdlib
    hardware_spi
    hardware_dma
    hardware_gpio
    flashstream
)

# Select the compiled-in panel for one target (the default is ILI9341 when
# PIPOD_DISPLAY_ST7796 is not defined).
#
#   pipod_display_select(<target> ST7796)    # 480x320, st7796.c
#   pipod_display_select(<target> ILI9341)   # 320x240, ili9341.c
#
# The definition is PUBLIC, so it also reaches consumers of <target>. display.h
# and gfx_config.h are compile-time selections, so the same define must be
# applied to EVERY target that compiles against them. With the current layout
# that is lib/gfx and the application, e.g.:
#
#   pipod_display_select(gfx ST7796)
#   pipod_display_select(pipod ST7796)
#
# Forgetting one target mixes driver mappings and screen sizes (and can fail
# to link), so keep the calls together.
function(pipod_display_select target panel)
    string(TOUPPER "${panel}" _panel)
    if(_panel STREQUAL "ST7796")
        target_compile_definitions(${target} PUBLIC PIPOD_DISPLAY_ST7796=1)
    elseif(_panel STREQUAL "ILI9341")
        target_compile_definitions(${target} PUBLIC PIPOD_DISPLAY_ST7796=0)
    else()
        message(FATAL_ERROR
            "pipod_display_select(${target}): unknown panel '${panel}' (use ILI9341 or ST7796)")
    endif()
endfunction()
