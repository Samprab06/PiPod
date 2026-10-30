# lib/ui - iPod-style user interface (screens, menus, animations, input).
#
# The UI owns the display and runs on its own core; everything else talks to it
# through ui.h (input events, playback state, commands). All state is static:
# there is no malloc after ui_init().

add_library(ui STATIC
    ui_core.c
    ui_shared.c
    ui_anim.c
    ui_widgets.c
    ui_menu.c
    ui_nowplaying.c
    ui_settings.c
)

# ui.h is the public contract; consumers include it as "ui.h".
target_include_directories(ui PUBLIC ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(ui PUBLIC
    gfx
    ili9341
    pico_stdlib
    pico_multicore
    pico_util
    hardware_sync
)

# Rendering is hot: it shares core 1 with input handling at 30 FPS.
target_compile_options(ui PRIVATE -O2)
