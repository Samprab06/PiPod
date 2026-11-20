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

# ---------------------------------------------------------------------------
# PiPod board pin map
#
# One header name reaches every target that compiles hardware pins (display
# configs at init, the SD hardware config and the app itself). To add a PCB
# revision: create boards/pipod_pcb_pins.h with the same PIPOD_PIN_* names and
# select it with -DPIPOD_PINS=PCB. Nothing else changes.
# ---------------------------------------------------------------------------
set(PIPOD_PINS "BREADBOARD" CACHE STRING
    "PiPod pin map header selector: BREADBOARD (or PCB once boards/pipod_pcb_pins.h exists)")
set(PIPOD_PINS_DIR "${CMAKE_CURRENT_LIST_DIR}/boards")

if(PIPOD_PINS STREQUAL "BREADBOARD")
    set(PIPOD_PINS_HEADER "pipod_breadboard_pins.h")
elseif(PIPOD_PINS STREQUAL "PCB")
    if(NOT EXISTS "${PIPOD_PINS_DIR}/pipod_pcb_pins.h")
        message(FATAL_ERROR
            "PIPOD_PINS=PCB selected but ${PIPOD_PINS_DIR}/pipod_pcb_pins.h does not exist")
    endif()
    set(PIPOD_PINS_HEADER "pipod_pcb_pins.h")
else()
    message(FATAL_ERROR
        "Unknown PIPOD_PINS='${PIPOD_PINS}' (use BREADBOARD or PCB)")
endif()
message(STATUS "PiPod pin map: ${PIPOD_PINS} (${PIPOD_PINS_HEADER})")

# Apply the selected pin header to a target that needs PIPOD_PIN_* macros.
function(pipod_apply_pin_header target)
    target_include_directories(${target} PRIVATE "${PIPOD_PINS_DIR}")
    target_compile_definitions(${target} PRIVATE
        PIPOD_PINS_HEADER="${PIPOD_PINS_HEADER}")
endfunction()

add_subdirectory(lib/display)
foreach(panel ili9341 st7796)
    pipod_apply_pin_header(${panel})
    # Config headers are public and are also included by stand-alone tests.
    target_include_directories(${panel} PUBLIC "${PIPOD_PINS_DIR}")
    target_compile_definitions(${panel} PUBLIC PIPOD_PINS_HEADER="${PIPOD_PINS_HEADER}")
endforeach()

# Optional layers owned by other workstreams: only build them when present.
add_subdirectory(third_party/tjpgd)

# SD card stack (carlk3 no-OS-FatFS v3.6.2) and the PiPod hardware config.
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/third_party/no-OS-FatFS/CMakeLists.txt)
    add_subdirectory(third_party/no-OS-FatFS)
endif()
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/lib/storage/CMakeLists.txt)
    add_subdirectory(lib/storage)
endif()

foreach(layer lib/gfx lib/flashstream lib/media lib/ui lib/input lib/ui_demo_assets apps/pipod)
    if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/${layer}/CMakeLists.txt)
        add_subdirectory(${layer})
    endif()
endforeach()

add_subdirectory(apps/display_test)
add_subdirectory(apps/spi_overclock)
add_subdirectory(apps/motion_test)

# Breadboard bring-up tools (UART stdio only, no display required).
if(TARGET storage AND TARGET media)
    add_subdirectory(apps/sd_test)
    add_subdirectory(apps/hw_test)
endif()
if(TARGET input AND TARGET ui)
    add_subdirectory(apps/touchpad_test)
endif()
