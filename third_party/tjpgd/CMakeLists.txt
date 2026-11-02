# TJpgDec R0.03 (C)ChaN 2021, see the notice at the top of tjpgd.c.
add_library(tjpgd STATIC ${CMAKE_CURRENT_LIST_DIR}/tjpgd.c)
target_include_directories(tjpgd PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(tjpgd PRIVATE -O2 -w)
