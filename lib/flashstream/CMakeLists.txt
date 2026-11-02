# lib/flashstream - arbitration for the single RP2350 XIP stream unit.
#
# There is exactly one stream unit (XIP_CTRL STREAM_ADDR/STREAM_CTR, FIFO via
# XIP_AUX, DREQ_XIP_STREAM); lib/display and the audio path share it. See
# flashstream.h for the protocol (audio has priority, non-blocking claims).

add_library(flashstream STATIC
    flashstream.c
)

target_include_directories(flashstream PUBLIC ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(flashstream PUBLIC
    pico_stdlib
    hardware_sync
)

# A few instructions on the hot path of every flash blit.
target_compile_options(flashstream PRIVATE -O2)
