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
    target_link_libraries(psp2i PRIVATE user32 gdi32 dbghelp winmm d3d11 d3dcompiler dxguid secur32 crypt32 ws2_32 winhttp bcrypt)
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

# Unit tests for the settings menu, the FPS counter and run-time frame-rate
# switching (no game data needed): ctest -C Release, or run test_menu.
enable_testing()
add_executable(test_menu tests/test_menu.c host/menu.c host/framerate.c host/camera.c host/login.c host/textedit.c)
target_include_directories(test_menu PRIVATE host)
target_link_libraries(test_menu PRIVATE psprecomp)
add_test(NAME test_menu COMMAND test_menu)

# rpcn_probe: check an RPCN server (TLS, protocol version, refusal of a bad login).
add_executable(rpcn_probe tools/rpcn_probe.c host/rpcn.c host/tls.c)
if(WIN32)
    target_link_libraries(rpcn_probe PRIVATE secur32 crypt32 ws2_32)
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
endif()
