add_executable(hw_test main.c ../../lib/input/input_mpr121.c
    ../../lib/input/input_clickwheel.c)
# Do not link input: its UI event adapters pull in lib/ui and the ILI9341 gfx.
target_link_libraries(hw_test PRIVATE gfx_st7796 storage media pico_stdlib hardware_i2c)
target_include_directories(hw_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../pipod
    ${CMAKE_CURRENT_LIST_DIR}/../../lib/input)
pipod_display_select(hw_test ST7796)
pipod_apply_pin_header(hw_test)
pico_enable_stdio_uart(hw_test 1)
pico_enable_stdio_usb(hw_test 0)
pico_add_extra_outputs(hw_test)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/check_firmware_size.cmake)
media_check_firmware_size(hw_test)
