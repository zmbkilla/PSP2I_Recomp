# PSP2i for Android, native part (built by app/build.gradle).
#
#   libmain.so         the psprecomp runtime, the SDL3 host (host/sdl_main.c),
#                      the GE on OpenGL ES 3.0 (host/gl_ge.c) and the game's
#                      generated code (port/gen) when it is there
#   libSDL3.so         SDL3, from SDL_DIR (the source app/build.gradle fetches)
#   libpsp2i_atrac.so  the LGPL ATRAC decoder (atrac_dll/), loaded at run time
#
# port/gen is the C that allegrexrecomp generates from your own decrypted
# EBOOT.BIN; it is derived from the game, so the repository does not carry it.
# Without it the app builds (src/gen_stub.c) and says so when started.

cmake_minimum_required(VERSION 3.22)
project(psp2i_android C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(PORT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(PSPRECOMP_DIR ${PORT}/../psprecomp)
if(NOT SDL_DIR OR NOT EXISTS ${SDL_DIR}/CMakeLists.txt)
    message(FATAL_ERROR "SDL_DIR must point at SDL3's source (app/build.gradle fetches it)")
endif()

add_subdirectory(${SDL_DIR} SDL EXCLUDE_FROM_ALL)
add_subdirectory(${PSPRECOMP_DIR} psprecomp EXCLUDE_FROM_ALL)
add_subdirectory(${PORT}/atrac_dll atrac_dll)

if(EXISTS ${PORT}/gen/recomp_funcs.c)
    set(GAME_SOURCES ${PORT}/gen/recomp_funcs.c ${PORT}/gen/recomp_imports.c)
    # ~200 MB of C: -O1 keeps the compiler's time and memory bounded on it.
    set_source_files_properties(${PORT}/gen/recomp_funcs.c PROPERTIES COMPILE_OPTIONS "-O1")
    set(HAVE_GAME_CODE ON)
    message(STATUS "PSP2i: building with the game code in port/gen")
else()
    set(GAME_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/gen_stub.c)
    set(HAVE_GAME_CODE OFF)
    message(STATUS "PSP2i: no port/gen; the app will build but cannot start the game")
endif()

add_library(main SHARED
    ${PORT}/host/sdl_main.c
    ${PORT}/host/gl_ge.c
    ${PORT}/host/gl_load.c
    ${PORT}/host/atrac_at3.c
    ${PORT}/host/camera.c
    ${PORT}/host/framerate.c
    ${GAME_SOURCES})
target_include_directories(main PRIVATE ${PORT}/host ${PORT}/gen)
target_compile_definitions(main PRIVATE PSP2I_GLES)
if(NOT HAVE_GAME_CODE)
    target_compile_definitions(main PRIVATE PSP2I_NO_GAME_CODE)
endif()
target_link_libraries(main PRIVATE psprecomp SDL3::SDL3 android log m dl)
