# lib/input — hardware -> abstract UI event adapters
#
# Produces UI_INPUT_WHEEL / UI_INPUT_BUTTON_DOWN / UI_INPUT_BUTTON_UP through
# ui_post_wheel() / ui_post_button() only; it never touches UI internals.
# Links pico_stdlib + hardware_gpio and borrows the ui headers via target `ui`.

add_library(input STATIC
    input_buttons.c
    input_wheel.c
    input_uart.c
)

target_include_directories(input PUBLIC ${CMAKE_CURRENT_LIST_DIR})

# lib/ui is owned by another workstream. When its target exists, link it for
# the ui.h contract; if it is still being written, fall back to the headers so
# the input library itself stays buildable.
if(TARGET ui)
    target_link_libraries(input PUBLIC ui)
else()
    target_include_directories(input PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../ui
        ${CMAKE_CURRENT_LIST_DIR}/../gfx
    )
endif()

target_link_libraries(input PUBLIC
    pico_stdlib
    hardware_gpio
    m
)

# Host-side unit test for the wheel math (built with the host gcc, not arm):
#   gcc -std=c11 -Wall -Wextra -O2 -I lib/input -I lib/ui -I lib/gfx \
#       lib/input/test/wheel_host_test.c lib/input/input_wheel.c -lm \
#       -o build/wheel_host_test
