# apps/pipod — the PiPod UI with the real media library (SD card first, then
# the flash media pack, then the demo tables) and a fake audio engine.
#
# Two firmware images are built from the same sources:
#
#   pipod          ILI9341, 320x240 landscape (unchanged behaviour)
#   pipod_st7796   ST7796S, 480x320 landscape (PIPOD_DISPLAY_ST7796=1)
#
# Each panel size needs its own lib/gfx and lib/ui archive because the strip
# buffers, layout, fonts and precomputed assets are compile-time selections.
# pipod_display_select() applies the matching define to the archive and to the
# application target.

if(NOT TARGET ui OR NOT TARGET gfx OR NOT TARGET ili9341 OR NOT TARGET input OR
   NOT TARGET media OR NOT TARGET flashstream)
    message(STATUS "pipod: required libraries not configured yet -- skipping pipod")
    return()
endif()

# --- ST7796 variants of the size-dependent libraries -----------------------
if(NOT TARGET gfx_st7796)
    pipod_add_gfx_library(gfx_st7796)
    pipod_display_select(gfx_st7796 ST7796)

    pipod_add_ui_library(ui_st7796 gfx_st7796)
    pipod_display_select(ui_st7796 ST7796)
endif()

set(PIPOD_SOURCES
    main.c
    sim_library.c
    sim_player.c
    audio_load_sim.c
)

# --- pipod: ILI9341 320x240 ------------------------------------------------
add_executable(pipod ${PIPOD_SOURCES})

target_link_libraries(pipod
    ui
    gfx
    input
    media
    flashstream
    ui_demo_assets
    storage
    pico_stdlib
    pico_multicore
    pico_sync
)

pipod_display_select(pipod ILI9341)
pipod_apply_pin_header(pipod)

# stdio over UART0 (GP0 TX / GP1 RX), no USB CDC.
pico_enable_stdio_uart(pipod 1)
pico_enable_stdio_usb(pipod 0)

pico_add_extra_outputs(pipod)

# Fail the build when the image would overlap the media pack (1 MB region).
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/check_firmware_size.cmake)
media_check_firmware_size(pipod)

# --- pipod_st7796: ST7796S 480x320 -----------------------------------------
add_executable(pipod_st7796 ${PIPOD_SOURCES})

target_link_libraries(pipod_st7796
    ui_st7796
    gfx_st7796
    input
    media
    flashstream
    storage
    pico_stdlib
    pico_multicore
    pico_sync
)

# The six generated demo covers (~169 KiB of const pixel data) do not fit in
# the 1 MB firmware region together with the 480x320 asset set. This image
# keeps the demo tables but draws the UI's built-in placeholder cover, exactly
# as it does for a library entry without cached art. SD-card and flash-pack
# covers are unaffected.
target_compile_definitions(pipod_st7796 PRIVATE PIPOD_DEMO_ART=0)

pipod_display_select(pipod_st7796 ST7796)
pipod_apply_pin_header(pipod_st7796)

# stdio over UART0 (GP0 TX / GP1 RX), no USB CDC.
pico_enable_stdio_uart(pipod_st7796 1)
pico_enable_stdio_usb(pipod_st7796 0)

pico_add_extra_outputs(pipod_st7796)

media_check_firmware_size(pipod_st7796)
