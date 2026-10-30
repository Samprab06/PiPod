# lib/ui_demo_assets — procedurally generated demo album art
#
# 120x120 RGB565 covers produced by scripts/gen_demo_art.py. Everything here is
# `const`, so the pixel data lives in flash, not RAM. Link this library only
# into the pipod app (or any app that wants the sample artwork).

add_library(ui_demo_assets STATIC
    ${CMAKE_CURRENT_LIST_DIR}/art_0.c
    ${CMAKE_CURRENT_LIST_DIR}/art_1.c
    ${CMAKE_CURRENT_LIST_DIR}/art_2.c
    ${CMAKE_CURRENT_LIST_DIR}/art_3.c
    ${CMAKE_CURRENT_LIST_DIR}/art_4.c
    ${CMAKE_CURRENT_LIST_DIR}/art_5.c
    ${CMAKE_CURRENT_LIST_DIR}/demo_art.c
)

target_include_directories(ui_demo_assets PUBLIC ${CMAKE_CURRENT_LIST_DIR})

# gfx_image_t lives in gfx; PUBLIC so consumers of ui_demo_assets also see it.
target_link_libraries(ui_demo_assets PUBLIC gfx)
