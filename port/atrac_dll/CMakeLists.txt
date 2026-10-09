# psp2i_atrac.dll -- ATRAC3 / ATRAC3plus decoding for the game's music,
# replacing the FFmpeg DLLs.
#
# The decoder is FFmpeg's, as extracted to stand alone by PPSSPP
# (hrydgard/ppsspp, ext/at3_standalone): LGPL-2.1-or-later. It is built here as
# its own DLL and loaded at run time (host/atrac_at3.c), so the MIT-licensed
# recompiled code never links it -- the LGPL boundary is the DLL. The sources
# are downloaded at configure time from a pinned PPSSPP commit (reproducible
# builds); the LGPL text and a note on where the source comes from ship next to
# the DLL (licenses/).
#
# shim/ holds this project's own small stand-ins for the three PPSSPP headers
# the decoder includes (ppsspp_config.h, Common/Log.h, Common/MemoryUtil.h).

set(PSP2I_AT3_COMMIT "0ac523c4b56d91b1db264858f719fcb1e1dffe56" CACHE STRING "hrydgard/ppsspp commit to take ext/at3_standalone from")
set(AT3_DIR "${CMAKE_BINARY_DIR}/_deps/at3_standalone/${PSP2I_AT3_COMMIT}")
set(AT3_URL "https://raw.githubusercontent.com/hrydgard/ppsspp/${PSP2I_AT3_COMMIT}/ext/at3_standalone")
set(AT3_FILES
    at3_decoders.h atrac.cpp atrac.h atrac3.cpp atrac3data.h atrac3plus.cpp atrac3plus.h
    atrac3plus_data.h atrac3plusdec.cpp atrac3plusdsp.cpp compat.cpp compat.h fft.cpp fft.h
    float_dsp.h get_bits.cpp get_bits.h intreadwrite.h mem.cpp mem.h README.txt)

set(AT3_OK TRUE)
foreach(f ${AT3_FILES})
    if(NOT EXISTS "${AT3_DIR}/${f}")
        file(DOWNLOAD "${AT3_URL}/${f}" "${AT3_DIR}/${f}" STATUS st TIMEOUT 60)
        list(GET st 0 code)
        if(NOT code EQUAL 0)
            file(REMOVE "${AT3_DIR}/${f}")
            set(AT3_OK FALSE)
            message(WARNING "ATRAC: could not download ${f} (${st}); ATRAC music will be silent")
            break()
        endif()
    endif()
endforeach()
set(AT3_LICENSE "${CMAKE_BINARY_DIR}/_deps/at3_standalone/COPYING.LGPLv2.1")
if(NOT EXISTS "${AT3_LICENSE}")
    file(DOWNLOAD "https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/COPYING.LGPLv2.1" "${AT3_LICENSE}" STATUS st TIMEOUT 60)
endif()

if(AT3_OK)
    if(NOT AT3_ANNOUNCED)
        message(STATUS "ATRAC: decoder sources from hrydgard/ppsspp ${PSP2I_AT3_COMMIT} (LGPL-2.1+)")
        set(AT3_ANNOUNCED TRUE CACHE INTERNAL "")
    endif()
    add_library(psp2i_atrac SHARED
        ${AT3_DIR}/atrac.cpp ${AT3_DIR}/atrac3.cpp ${AT3_DIR}/atrac3plus.cpp ${AT3_DIR}/atrac3plusdec.cpp
        ${AT3_DIR}/atrac3plusdsp.cpp ${AT3_DIR}/get_bits.cpp ${AT3_DIR}/compat.cpp ${AT3_DIR}/fft.cpp
        ${AT3_DIR}/mem.cpp
        at3_export.cpp)
    target_include_directories(psp2i_atrac PRIVATE ${AT3_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/shim)
    if(MSVC)
        target_compile_options(psp2i_atrac PRIVATE /W0 /O2)
        target_compile_definitions(psp2i_atrac PRIVATE _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
    endif()
    # Next to psp2i.exe, which loads it.
    set_target_properties(psp2i_atrac PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/Release
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/Debug)
    # The LGPL text and the source note, next to the DLL.
    set(AT3_NOTE "${CMAKE_BINARY_DIR}/_deps/at3_standalone/psp2i_atrac.txt")
    file(WRITE "${AT3_NOTE}"
"psp2i_atrac.dll -- ATRAC3 / ATRAC3plus audio decoding

This DLL is FFmpeg's ATRAC3 and ATRAC3plus decoder as extracted to stand alone
by PPSSPP (ext/at3_standalone), licensed under the GNU Lesser General Public
License, version 2.1 or later (COPYING.LGPLv2.1). Source:
  https://github.com/hrydgard/ppsspp/tree/${PSP2I_AT3_COMMIT}/ext/at3_standalone
plus the build files and the small compatibility headers in port/atrac_dll of
this project. You may replace this DLL with your own build of that source; the
game loads it by name at run time and uses only the functions exported by
port/atrac_dll/at3_export.cpp.
")
    add_custom_command(TARGET psp2i_atrac POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:psp2i_atrac>/licenses
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AT3_NOTE} $<TARGET_FILE_DIR:psp2i_atrac>/licenses/psp2i_atrac.txt)
    if(EXISTS "${AT3_LICENSE}")
        add_custom_command(TARGET psp2i_atrac POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AT3_LICENSE} $<TARGET_FILE_DIR:psp2i_atrac>/licenses/COPYING.LGPLv2.1)
    endif()
endif()
