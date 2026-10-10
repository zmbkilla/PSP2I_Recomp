# H.264 movies (sceMpeg) through Cisco's OpenH264 decoder.
#
# host/h264_openh264.c loads Cisco's prebuilt openh264 DLL at run time. Cisco's
# H.264 patent licence covers that binary only when each user's machine
# downloads it from Cisco, so the DLL is not shipped: the game fetches it from
# ciscobinary.openh264.org on first start, checks its SHA-256, and keeps it
# next to the exe (psp2i_display.ini openh264=off turns this off).
#
# What the build needs, fetched here at configure time:
#   - the decoder's C API headers (BSD-2), matching the DLL's version exactly,
#     since the interface is a vtable;
#   - bzip2 1.0.8's source (BSD-style licence): Cisco publishes the DLL only
#     as .bz2, so its decompressor is compiled into the game.
#
# psp2i_fetch_openh264(<inc-out> <bz2-out>) sets the header directory and the
# bzip2 source directory, or empties both when offline (movies are then black,
# with their sound).

set(PSP2I_OPENH264_VERSION 2.6.0)

function(psp2i_fetch_openh264 inc_out bz2_out)
    set(${inc_out} "" PARENT_SCOPE)
    set(${bz2_out} "" PARENT_SCOPE)
    set(dir "${CMAKE_BINARY_DIR}/_deps/openh264")
    set(base "https://raw.githubusercontent.com/cisco/openh264/v${PSP2I_OPENH264_VERSION}")
    set(files
        "codec/api/wels/codec_api.h"     21f29b20c24f7c7946f2e243d0bc2532fb3542f6c28af338209477e70d9036c9
        "codec/api/wels/codec_app_def.h" a40581a24263866dca19911928f7bc4eb354ff78d9dd56dbf0f55fc4fd923726
        "codec/api/wels/codec_def.h"     f974d269b5935e8dc7265b8bfc02f60e5185b4d6165d30541d2758a4506f1979
        "codec/api/wels/codec_ver.h"     9a241e20b7c9221a5786cccd9eae3afed91afba3525b5b9b16c2101976516f94
        "LICENSE"                        dd5c1c9668512530fa5a96e4c29ac4033d70a7eeb0eed7a42fddb6dd794ebdbb)
    list(LENGTH files n)
    math(EXPR last "${n} - 1")
    foreach(i RANGE 0 ${last} 2)
        math(EXPR j "${i} + 1")
        list(GET files ${i} path)
        list(GET files ${j} hash)
        get_filename_component(name "${path}" NAME)
        if(name STREQUAL "LICENSE")
            set(dest "${dir}/LICENSE")
        else()
            set(dest "${dir}/include/wels/${name}")
        endif()
        if(NOT EXISTS "${dest}")
            message(STATUS "OpenH264: fetching ${name}")
            file(DOWNLOAD "${base}/${path}" "${dest}" EXPECTED_HASH SHA256=${hash} STATUS st TIMEOUT 60)
            list(GET st 0 code)
            if(NOT code EQUAL 0)
                file(REMOVE "${dest}")
                message(WARNING "OpenH264: could not fetch ${name}; movies will be black")
                return()
            endif()
        endif()
    endforeach()

    set(bz "${dir}/bzip2-1.0.8")
    if(NOT EXISTS "${bz}/bzlib.c")
        message(STATUS "OpenH264: fetching bzip2 1.0.8")
        file(DOWNLOAD "https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz" "${dir}/bzip2.tar.gz"
             EXPECTED_HASH SHA256=ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269
             STATUS st TIMEOUT 120)
        list(GET st 0 code)
        if(NOT code EQUAL 0)
            message(WARNING "OpenH264: could not fetch bzip2; movies will be black")
            return()
        endif()
        file(ARCHIVE_EXTRACT INPUT "${dir}/bzip2.tar.gz" DESTINATION "${dir}" TOUCH)
    endif()
    set(${inc_out} "${dir}/include" PARENT_SCOPE)
    set(${bz2_out} "${bz}" PARENT_SCOPE)
endfunction()
