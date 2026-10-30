# apps/pipod — the PiPod UI with a simulated music library and fake audio.
#
# Needs lib/ui (target `ui`). Skip configure errors while that workstream is
# still landing: re-run cmake once ui/CMakeLists.txt exists.

if(NOT TARGET ui OR NOT TARGET gfx OR NOT TARGET ili9341 OR NOT TARGET input)
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
    ui_demo_assets
    pico_stdlib
    pico_multicore
    pico_sync
)

# stdio over UART0 (GP0 TX / GP1 RX), no USB CDC.
pico_enable_stdio_uart(pipod 1)
pico_enable_stdio_usb(pipod 0)

pico_add_extra_outputs(pipod)
