cmake_minimum_required(VERSION 3.13)

# Custom board headers must be visible to the SDK before pico_sdk_init().
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)

include(${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake)

project(pipod C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

pico_sdk_init()

# Warnings for our own targets only (the SDK is added above).
add_compile_options(-Wall -Wextra)

add_subdirectory(lib/display)

# Optional layers owned by other workstreams: only build them when present.
foreach(layer lib/gfx lib/ui lib/input lib/ui_demo_assets apps/pipod)
    if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/${layer}/CMakeLists.txt)
        add_subdirectory(${layer})
    endif()
endforeach()

add_subdirectory(apps/display_test)
