# SDL3.dll for controllers and sound, without a local prebuilt.
#
# host/input_sdl.c and host/audio_sdl.c load SDL3.dll at run time and declare
# the few SDL3 functions they use themselves, so the build needs neither SDL
# headers nor an import library -- only the DLL to ship, and any SDL 3.x works
# (the SDL3 ABI is stable within the major version).
#
# psp2i_fetch_sdl3(<out-var>) sets <out-var> to an SDL3.dll path:
#   1. PSP2I_SDL3_DLL, if set (a specific DLL you want to ship);
#   2. else the newest official release (libsdl-org/SDL, SDL3-*-win32-x64.zip),
#      fetched at configure time into the build tree and reused afterwards;
#      PSP2I_SDL3_REFRESH=ON checks for a newer release on the next configure;
#   3. offline: ../SDL3.dll if present, else empty (controllers and sound off,
#      the game still runs).

set(PSP2I_SDL3_DLL "" CACHE FILEPATH "SDL3.dll to ship; empty = fetch the newest official release")
option(PSP2I_SDL3_REFRESH "Look for a newer SDL3 release at the next configure" OFF)

function(psp2i_fetch_sdl3 out)
    if(PSP2I_SDL3_DLL AND EXISTS "${PSP2I_SDL3_DLL}")
        set(${out} "${PSP2I_SDL3_DLL}" PARENT_SCOPE)
        return()
    endif()
    set(dir "${CMAKE_BINARY_DIR}/_deps/sdl3")
    set(dll "${dir}/SDL3.dll")
    if(EXISTS "${dll}" AND NOT PSP2I_SDL3_REFRESH)
        set(${out} "${dll}" PARENT_SCOPE)
        return()
    endif()
    file(MAKE_DIRECTORY "${dir}")
    message(STATUS "SDL3: looking up the newest release")
    file(DOWNLOAD "https://api.github.com/repos/libsdl-org/SDL/releases/latest" "${dir}/latest.json"
         STATUS st TIMEOUT 30)
    list(GET st 0 code)
    set(url "")
    if(code EQUAL 0)
        file(READ "${dir}/latest.json" json)
        string(JSON tag ERROR_VARIABLE jerr GET "${json}" tag_name)
        string(JSON n ERROR_VARIABLE jerr LENGTH "${json}" assets)
        if(NOT jerr AND n GREATER 0)
            math(EXPR last "${n} - 1")
            foreach(i RANGE ${last})
                string(JSON name GET "${json}" assets ${i} name)
                if(name MATCHES "^SDL3-[0-9.]+-win32-x64\\.zip$")
                    string(JSON url GET "${json}" assets ${i} browser_download_url)
                    break()
                endif()
            endforeach()
        endif()
    endif()
    if(url)
        message(STATUS "SDL3: downloading ${tag}")
        file(DOWNLOAD "${url}" "${dir}/sdl3.zip" STATUS st TIMEOUT 120)
        list(GET st 0 code)
        if(code EQUAL 0)
            file(REMOVE_RECURSE "${dir}/x")
            file(ARCHIVE_EXTRACT INPUT "${dir}/sdl3.zip" DESTINATION "${dir}/x" TOUCH)   # TOUCH: some drives refuse the archive timestamps
            file(GLOB_RECURSE found "${dir}/x/SDL3.dll")
            if(found)
                list(GET found 0 f)
                file(COPY_FILE "${f}" "${dll}")
                file(WRITE "${dir}/VERSION" "${tag}\n")
                message(STATUS "SDL3: ${tag} -> ${dll}")
                set(${out} "${dll}" PARENT_SCOPE)
                return()
            endif()
        endif()
    endif()
    if(EXISTS "${dll}")                                  # a refresh that failed: keep the old one
        message(STATUS "SDL3: could not refresh; keeping the one fetched before")
        set(${out} "${dll}" PARENT_SCOPE)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../SDL3.dll")
        message(STATUS "SDL3: offline; using ../SDL3.dll")
        set(${out} "${CMAKE_CURRENT_SOURCE_DIR}/../SDL3.dll" PARENT_SCOPE)
    else()
        message(WARNING "SDL3: no SDL3.dll (offline?). The game runs without controllers and SDL sound.")
        set(${out} "" PARENT_SCOPE)
    endif()
endfunction()
