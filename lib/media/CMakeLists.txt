# Media library: metadata parsing, flash media pack, art decoding, cache.
file(GLOB MEDIA_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/*.c)

add_library(media STATIC ${MEDIA_SOURCES})
target_include_directories(media PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    # media_library.h exposes a ui_library_t implementation; only ui.h and
    # gfx_image.h are needed here (headers, no link dependency on lib/ui).
    ${CMAKE_CURRENT_LIST_DIR}/../ui
)
target_link_libraries(media PUBLIC tjpgd gfx pico_stdlib hardware_flash pico_flash)
target_compile_options(media PRIVATE -O2)
