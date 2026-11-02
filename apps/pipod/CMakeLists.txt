# apps/pipod — the PiPod UI with the real media-pack library (demo tables as
# the fallback) and a fake audio engine.
#
# Needs lib/ui (target `ui`) and lib/media (target `media`); skip configure
# errors while those workstreams are still landing.

if(NOT TARGET ui OR NOT TARGET gfx OR NOT TARGET ili9341 OR NOT TARGET input OR
   NOT TARGET media OR NOT TARGET flashstream)
    message(STATUS "pipod: required libraries not configured yet -- skipping pipod")
    return()
endif()

add_executable(pipod
    main.c
    sim_library.c
    sim_player.c
    audio_load_sim.c
)

target_link_libraries(pipod
    ui
    gfx
    ili9341
    input
    media
    flashstream
    ui_demo_assets
    pico_stdlib
    pico_multicore
    pico_sync
)

# stdio over UART0 (GP0 TX / GP1 RX), no USB CDC.
pico_enable_stdio_uart(pipod 1)
pico_enable_stdio_usb(pipod 0)

pico_add_extra_outputs(pipod)

# Fail the build when the image would overlap the media pack (1 MB region).
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/check_firmware_size.cmake)
media_check_firmware_size(pipod)
