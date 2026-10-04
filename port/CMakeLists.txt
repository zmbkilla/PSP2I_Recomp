# psp2i — Phantasy Star Portable 2 Infinity (NPJH50332), recompiled with psprecomp.
#
# Builds the psprecomp runtime from the toolkit source next to this directory,
# the generated C in gen/, and the host in host/. Regenerate gen/ with:
#
#   psprecomp/build/tools/allegrexrecomp/Release/allegrexrecomp.exe emit EBOOT.BIN port/gen recomp
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

add_executable(psp2i
    host/main.c
    host/oracle.c
    host/d3d11.c
    host/input_sdl.c
    host/audio_sdl.c
    host/atrac_ffmpeg.c
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
    target_link_libraries(psp2i PRIVATE user32 gdi32 dbghelp winmm d3d11 d3dcompiler dxguid)
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

if(PSP2I_TRACE)
    target_compile_definitions(psp2i PRIVATE PSPRECOMP_TRACE)
endif()
