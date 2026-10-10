# psp2i — Phantasy Star Portable 2 Infinity (NPJH50332), recompiled with psprecomp.
#
# Builds the psprecomp runtime from the toolkit source next to this directory,
# the generated C in gen/, and the host in host/. Regenerate gen/ with:
#
#   psprecomp/build/tools/allegrexrecomp/Release/allegrexrecomp.exe emit EBOOT.BIN port/gen recomp \
#       --hooks port/hooks.txt --fixes port/fixes.txt
#
# Configure and build (MSVC):
#   cmake -S port -B port/build -G "Visual Studio 17 2022" -A x64
#   cmake --build port/build --config Release

cmake_minimum_required(VERSION 3.16)
project(psp2i C CXX)                        # CXX: the ATRAC decoder DLL (atrac_dll/)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(PSPRECOMP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../psprecomp CACHE PATH "psprecomp toolkit source")
add_subdirectory(${PSPRECOMP_DIR} psprecomp EXCLUDE_FROM_ALL)

option(PSP2I_TRACE "Build the generated code with function-entry tracing" OFF)
# Community build: no command line, data found from the exe's folder, a start-up
# check of everything needed (host/prod.c); the exe and its DLLs are copied to
# <project>/prod. Build it in its own tree: cmake -S port -B port/build-prod -DPSP2I_PROD=ON
option(PSP2I_PROD "Community build: no arguments, checks for its data, copies to ../prod" OFF)

add_executable(psp2i
    host/main.c
    host/oracle.c
    host/d3d11.c
    host/present.c
    host/gl_load.c
    host/gl_ge.c
    host/gl_wgl.c
    host/input_sdl.c
    host/audio_sdl.c
    host/atrac_at3.c
    host/h264_openh264.c
    host/duck_win.c
    host/framerate.c
    host/menu.c
    host/camera.c
    host/tls.c
    host/rpcn.c
    host/online.c
    host/fbuf.c
    host/rpcn_rooms.c
    host/savecrypt.c
    host/gamelog.c
    host/prod.c
    host/login.c
    host/textedit.c
    ${PSPRECOMP_DIR}/tools/allegrexrecomp/decode.c
    gen/recomp_funcs.c
    gen/recomp_imports.c
)
target_include_directories(psp2i PRIVATE gen ${PSPRECOMP_DIR}/tools/allegrexrecomp)
target_link_libraries(psp2i PRIVATE psprecomp)

if(MSVC)
    # The generated translation unit is ~200 MB of C. /bigobj for its section
    # count; /O1 keeps the optimiser's time and memory bounded on it.
    target_compile_options(psp2i PRIVATE /bigobj /W1)
    set_source_files_properties(gen/recomp_funcs.c PROPERTIES COMPILE_OPTIONS "/O1;/Gy")
    target_link_options(psp2i PRIVATE /STACK:16777216)
    target_link_libraries(psp2i PRIVATE user32 gdi32 dbghelp winmm d3d11 d3dcompiler dxguid secur32 crypt32 ws2_32 winhttp bcrypt ole32)
    # Symbols for the crash report (and a debugger). /Zi does not change the
    # generated machine code.
    target_compile_options(psp2i PRIVATE /Zi)
    target_compile_options(psprecomp PRIVATE /Zi)
    target_link_options(psp2i PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()

# Controllers and sound: SDL3.dll is loaded at run time (host/input_sdl.c), so it
# is not a link dependency; any SDL 3.x works. cmake/FetchSDL3.cmake fetches the
# newest official release at configure time (or uses PSP2I_SDL3_DLL).
# ATRAC music: psp2i_atrac.dll (LGPL decoder, its own DLL; see atrac_dll/), built
# with the game and loaded at run time by host/atrac_at3.c. Replaces FFmpeg.
add_subdirectory(atrac_dll)
if(TARGET psp2i_atrac)
    add_dependencies(psp2i psp2i_atrac)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchSDL3.cmake)
psp2i_fetch_sdl3(PSP2I_SDL3_PATH)
if(PSP2I_SDL3_PATH)
    add_custom_command(TARGET psp2i POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PSP2I_SDL3_PATH} $<TARGET_FILE_DIR:psp2i>)
endif()

# Movies (sceMpeg): H.264 through Cisco's OpenH264 DLL, which the game downloads
# from Cisco on first start (host/h264_openh264.c; see cmake/FetchOpenH264.cmake
# for why it is not shipped). The build needs only the decoder's headers and
# bzip2's decompressor, fetched at configure time.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchOpenH264.cmake)
psp2i_fetch_openh264(PSP2I_H264_INC PSP2I_BZ2_DIR)
if(WIN32 AND PSP2I_H264_INC AND PSP2I_BZ2_DIR)
    add_library(psp2i_bz2 STATIC
        ${PSP2I_BZ2_DIR}/blocksort.c ${PSP2I_BZ2_DIR}/huffman.c ${PSP2I_BZ2_DIR}/crctable.c
        ${PSP2I_BZ2_DIR}/randtable.c ${PSP2I_BZ2_DIR}/compress.c ${PSP2I_BZ2_DIR}/decompress.c
        ${PSP2I_BZ2_DIR}/bzlib.c)
    target_include_directories(psp2i_bz2 PUBLIC ${PSP2I_BZ2_DIR})
    target_compile_definitions(psp2i_bz2 PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_include_directories(psp2i PRIVATE ${PSP2I_H264_INC})
    target_compile_definitions(psp2i PRIVATE PSP2I_OPENH264)
    target_link_libraries(psp2i PRIVATE psp2i_bz2)
    file(WRITE ${CMAKE_BINARY_DIR}/_deps/openh264/OpenH264-NOTICE.txt
"OpenH264 Video Codec provided by Cisco Systems, Inc.

Movies are decoded by Cisco's prebuilt OpenH264 ${PSP2I_OPENH264_VERSION} DLL. It is not
included: on first start the game downloads it from ciscobinary.openh264.org,
checks its SHA-256 and keeps it next to the exe. Set openh264=off in
psp2i_display.ini to stop the download (movies then play black, with sound);
delete the DLL to remove it. Cisco's binary licence:
https://www.openh264.org/BINARY_LICENSE.txt

OpenH264-LICENSE.txt: the licence of the OpenH264 headers used to build the game.
bzip2-LICENSE.txt: bzip2 1.0.8, compiled in to decompress Cisco's download.
")
    add_custom_command(TARGET psp2i POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:psp2i>/licenses
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/_deps/openh264/OpenH264-NOTICE.txt $<TARGET_FILE_DIR:psp2i>/licenses/OpenH264-NOTICE.txt
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/_deps/openh264/LICENSE $<TARGET_FILE_DIR:psp2i>/licenses/OpenH264-LICENSE.txt
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PSP2I_BZ2_DIR}/LICENSE $<TARGET_FILE_DIR:psp2i>/licenses/bzip2-LICENSE.txt)
endif()

# Unit tests for the settings menu, the FPS counter and run-time frame-rate
# switching (no game data needed): ctest -C Release, or run test_menu.
enable_testing()
add_executable(test_menu tests/test_menu.c host/menu.c host/framerate.c host/camera.c host/login.c host/textedit.c)
target_include_directories(test_menu PRIVATE host)
target_link_libraries(test_menu PRIVATE psprecomp)
add_test(NAME test_menu COMMAND test_menu)

# rpcn_probe: check an RPCN server (TLS, protocol version, refusal of a bad login).
# (port/tools/ is local-only, not in the repository: these are skipped without it.)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/rpcn_probe.c)
    add_executable(rpcn_probe tools/rpcn_probe.c host/rpcn.c host/tls.c)
    if(WIN32)
        target_link_libraries(rpcn_probe PRIVATE secur32 crypt32 ws2_32)
    endif()
endif()

if(PSP2I_TRACE)
    target_compile_definitions(psp2i PRIVATE PSPRECOMP_TRACE)
endif()

if(PSP2I_PROD)
    target_compile_definitions(psp2i PRIVATE PSP2I_PROD)
    # The exe's icon: <project>/asset/icon.ico, through a generated resource script.
    set(PSP2I_ICON ${CMAKE_CURRENT_SOURCE_DIR}/../asset/icon.ico CACHE FILEPATH "Icon for the community build")
    if(WIN32 AND EXISTS ${PSP2I_ICON})
        file(TO_CMAKE_PATH ${PSP2I_ICON} PSP2I_ICON_PATH)
        file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/psp2i_icon.rc "1 ICON \"${PSP2I_ICON_PATH}\"
")
        target_sources(psp2i PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/psp2i_icon.rc)
    endif()
    set(PSP2I_PROD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../prod CACHE PATH "Where the community build is copied")
    add_custom_command(TARGET psp2i POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PSP2I_PROD_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:psp2i> ${PSP2I_PROD_DIR}
        COMMENT "Copying the community build to ${PSP2I_PROD_DIR}")
    if(PSP2I_SDL3_PATH)
        add_custom_command(TARGET psp2i POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PSP2I_SDL3_PATH} ${PSP2I_PROD_DIR})
    endif()
    if(TARGET psp2i_atrac)                              # ATRAC music: the LGPL decoder DLL and its licence files
        add_custom_command(TARGET psp2i POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:psp2i_atrac> ${PSP2I_PROD_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_FILE_DIR:psp2i_atrac>/licenses ${PSP2I_PROD_DIR}/licenses)
    endif()
    add_custom_command(TARGET psp2i POST_BUILD             # the OpenH264 notice and licences (see above)
        COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_FILE_DIR:psp2i>/licenses ${PSP2I_PROD_DIR}/licenses)
endif()

# mpeg_probe: play one .pmf through the runtime's sceMpeg with the host codecs,
# without the game (pictures, audio frames, the end; saves some pictures).
if(TARGET psp2i_bz2 AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/mpeg_probe.c)
    add_executable(mpeg_probe tools/mpeg_probe.c host/h264_openh264.c host/atrac_at3.c
        ${PSPRECOMP_DIR}/tools/allegrexrecomp/crypto/sha1.c)
    target_include_directories(mpeg_probe PRIVATE host ${PSP2I_H264_INC} ${PSPRECOMP_DIR}/tools/allegrexrecomp)
    target_compile_definitions(mpeg_probe PRIVATE PSP2I_OPENH264)
    target_link_libraries(mpeg_probe PRIVATE psprecomp psp2i_bz2 winhttp bcrypt)
endif()

# atrac_probe: stream one ATRAC3plus track out of a file through the runtime's
# sceAtrac with the host decoder, as a game does (local tool, like mpeg_probe).
if(WIN32 AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/atrac_probe.c)
    add_executable(atrac_probe tools/atrac_probe.c host/atrac_at3.c ${PSPRECOMP_DIR}/tools/allegrexrecomp/crypto/sha1.c)
    target_include_directories(atrac_probe PRIVATE host ${PSPRECOMP_DIR}/tools/allegrexrecomp)
    target_link_libraries(atrac_probe PRIVATE psprecomp)
endif()
