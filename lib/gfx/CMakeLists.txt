add_library(gfx STATIC
    gfx.c
    gfx_render.c
)

target_include_directories(gfx PUBLIC ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(gfx PUBLIC
    ili9341
    pico_stdlib
)

# Drawing is hot: the UI renders strips on core 1 while core 0 runs audio.
target_compile_options(gfx PRIVATE -O2)

# Generated fonts (owned by the asset pipeline) add themselves to this target.
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/fonts/fonts.cmake)
    include(${CMAKE_CURRENT_LIST_DIR}/fonts/fonts.cmake)
endif()
