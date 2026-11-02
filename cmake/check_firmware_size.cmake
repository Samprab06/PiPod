# check_firmware_size.cmake
#
# Fails the build when the pipod firmware would overlap the media pack region
# (MEDIA_FIRMWARE_MAX = 1 MB, see lib/media/flash_layout.h).
#
# Usage, after add_executable()/pico_add_extra_outputs() in the app CMakeLists:
#
#   include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/check_firmware_size.cmake)
#   media_check_firmware_size(pipod)
#
# The limit is read from lib/media/flash_layout.h (single source of truth).
# The size comes from `arm-none-eabi-size` (text + data) on the just-linked
# ELF; if the tool cannot be found, the existing <target>.bin is used instead.
# The check runs POST_BUILD, so it only ever fails the build *after* a
# successful link.

set(MEDIA_CHECK_FW_SCRIPT_DIR "${CMAKE_CURRENT_LIST_DIR}"
    CACHE INTERNAL "check_firmware_size.cmake location")

function(_media_read_firmware_max out_var)
    set(_header "${MEDIA_CHECK_FW_SCRIPT_DIR}/../lib/media/flash_layout.h")
    set(_max "")
    if(EXISTS "${_header}")
        file(STRINGS "${_header}" _lines
             REGEX "MEDIA_FIRMWARE_MAX[ \t]+0[xX][0-9a-fA-F]+")
        foreach(_line IN LISTS _lines)
            if(_line MATCHES "MEDIA_FIRMWARE_MAX[ \t]+(0[xX][0-9a-fA-F]+)")
                set(_max "${CMAKE_MATCH_1}")
            endif()
        endforeach()
    endif()
    if(NOT _max)
        message(WARNING
            "media_check_firmware_size: MEDIA_FIRMWARE_MAX not found in "
            "${_header}; using 0x100000")
        set(_max "0x100000")
    endif()
    set(${out_var} "${_max}" PARENT_SCOPE)
endfunction()

# media_check_firmware_size(<target>)
function(media_check_firmware_size target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "media_check_firmware_size(): '${target}' is not a CMake target")
    endif()

    _media_read_firmware_max(_media_max)

    set(_size_tool "")
    if(CMAKE_SIZE AND EXISTS "${CMAKE_SIZE}")
        set(_size_tool "${CMAKE_SIZE}")
    elseif(PICO_TOOLCHAIN_PATH)
        find_program(_size_tool NAMES arm-none-eabi-size
                     HINTS "${PICO_TOOLCHAIN_PATH}/bin")
    endif()
    if(NOT _size_tool)
        find_program(_size_tool NAMES arm-none-eabi-size)
    endif()

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DMEDIA_CHECK_FIRMWARE_SIZE_MODE=1"
            "-DMEDIA_FIRMWARE_ELF=$<TARGET_FILE:${target}>"
            "-DMEDIA_FIRMWARE_BIN=$<TARGET_FILE_DIR:${target}>/${target}.bin"
            "-DMEDIA_SIZE_TOOL=${_size_tool}"
            "-DMEDIA_FIRMWARE_MAX=${_media_max}"
            -P "${MEDIA_CHECK_FW_SCRIPT_DIR}/check_firmware_size.cmake"
        VERBATIM
        COMMENT "Checking ${target} size against MEDIA_FIRMWARE_MAX (${_media_max})"
    )
endfunction()

# ---- script mode: run by the POST_BUILD step -------------------------------

if(MEDIA_CHECK_FIRMWARE_SIZE_MODE)
    math(EXPR _media_max "${MEDIA_FIRMWARE_MAX}")
    set(_media_size -1)
    set(_media_source "unknown")

    if(MEDIA_SIZE_TOOL
       AND EXISTS "${MEDIA_SIZE_TOOL}"
       AND EXISTS "${MEDIA_FIRMWARE_ELF}")
        # Preferred: the freshly linked ELF is always current at POST_BUILD.
        execute_process(
            COMMAND "${MEDIA_SIZE_TOOL}" "${MEDIA_FIRMWARE_ELF}"
            RESULT_VARIABLE _media_rc
            OUTPUT_VARIABLE _media_out
            ERROR_VARIABLE _media_err)
        if(_media_rc EQUAL 0 AND
           _media_out MATCHES "\n[ \t]*([0-9]+)[ \t]+([0-9]+)[ \t]+([0-9]+)[ \t]+([0-9]+)[ \t]+[0-9a-fA-F]+")
            math(EXPR _media_size "${CMAKE_MATCH_1} + ${CMAKE_MATCH_2}")
            set(_media_source "arm-none-eabi-size text+data")
        else()
            message(WARNING
                "media_check_firmware_size: could not size "
                "${MEDIA_FIRMWARE_ELF}: ${_media_err}")
        endif()
    endif()

    if(_media_size LESS 0 AND EXISTS "${MEDIA_FIRMWARE_BIN}")
        file(SIZE "${MEDIA_FIRMWARE_BIN}" _media_size)
        set(_media_source "bin")
    endif()

    if(_media_size LESS 0)
        message(WARNING
            "media_check_firmware_size: firmware size unavailable for "
            "${MEDIA_FIRMWARE_ELF}; skipping the ${MEDIA_FIRMWARE_MAX}-byte check")
    elseif(_media_size GREATER _media_max)
        math(EXPR _media_over "${_media_size} - ${_media_max}")
        message(FATAL_ERROR
            "Firmware image is too large: ${_media_size} bytes (${_media_source}) "
            "exceeds MEDIA_FIRMWARE_MAX = ${_media_max} bytes by ${_media_over} "
            "bytes.\n"
            "The media pack starts at flash offset 0x100000; shrink the "
            "firmware or move MEDIA_PACK_OFFSET in lib/media/flash_layout.h.")
    else()
        math(EXPR _media_free "${_media_max} - ${_media_size}")
        message(STATUS
            "Firmware size OK: ${_media_size} / ${_media_max} bytes "
            "(${_media_source}), ${_media_free} bytes free")
    endif()
endif()
