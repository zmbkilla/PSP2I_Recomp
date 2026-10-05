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
project(psp2i C)

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
    host/input_sdl.c
    host/audio_sdl.c
    host/atrac_ffmpeg.c
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

# Controllers: SDL3.dll is loaded at run time (host/input_sdl.c), so it is not a
# link dependency. If a copy sits in the project root, put it next to the exe.
set(PSP2I_SDL3_DLL ${CMAKE_CURRENT_SOURCE_DIR}/../SDL3.dll CACHE FILEPATH "SDL3.dll to copy next to psp2i.exe")
if(EXISTS ${PSP2I_SDL3_DLL})
    add_custom_command(TARGET psp2i POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PSP2I_SDL3_DLL} $<TARGET_FILE_DIR:psp2i>)
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
    set(PSP2I_FFMPEG_BIN ${CMAKE_CURRENT_SOURCE_DIR}/../ffmpeg-9.0.1-full_build-shared/bin CACHE PATH "FFmpeg DLLs for the community build")
    file(GLOB PSP2I_FFMPEG_DLLS ${PSP2I_FFMPEG_BIN}/avcodec-*.dll ${PSP2I_FFMPEG_BIN}/avutil-*.dll ${PSP2I_FFMPEG_BIN}/swresample-*.dll)
    add_custom_command(TARGET psp2i POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PSP2I_PROD_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:psp2i> ${PSP2I_PROD_DIR}
        COMMENT "Copying the community build to ${PSP2I_PROD_DIR}")
    foreach(dll ${PSP2I_SDL3_DLL} ${PSP2I_FFMPEG_DLLS})
        if(EXISTS ${dll})
            add_custom_command(TARGET psp2i POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dll} ${PSP2I_PROD_DIR})
        endif()
    endforeach()
endif()
