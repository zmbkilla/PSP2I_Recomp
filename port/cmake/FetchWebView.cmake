# Web pages over the game window (host/web_overlay.cpp: PSP2i's "open website").
#
# The webview library (MIT, https://github.com/webview/webview) wraps each
# system's own engine: WebView2 on Windows (the Edge engine that comes with
# Windows 10/11, loaded by webview's built-in loader, so nothing is shipped),
# WebKitGTK on Linux, WebKit on macOS. It is one header, fetched here at
# configure time together with Microsoft's WebView2.h (from the
# Microsoft.Web.WebView2 NuGet package; BSD-style licence), both pinned by
# SHA-256.
#
# psp2i_fetch_webview(<inc-out>) sets the directory holding webview/webview.h
# and WebView2.h, or empties it when offline (the game then opens the page in
# the default browser instead).

set(PSP2I_WEBVIEW_VERSION 0.12.0)
set(PSP2I_WEBVIEW2_VERSION 1.0.2903.40)

function(psp2i_fetch_webview inc_out)
    set(${inc_out} "" PARENT_SCOPE)
    set(dir "${CMAKE_BINARY_DIR}/_deps/webview")
    set(inc "${dir}/include")

    if(NOT EXISTS "${inc}/webview/webview.h")
        message(STATUS "webview: fetching ${PSP2I_WEBVIEW_VERSION}")
        file(DOWNLOAD "https://github.com/webview/webview/archive/refs/tags/${PSP2I_WEBVIEW_VERSION}.tar.gz"
             "${dir}/webview.tar.gz"
             EXPECTED_HASH SHA256=e2c8d0bed3fcd13e624074448b7d3b1d0b0d03546a15839f17ee0013f2cba1db
             STATUS st TIMEOUT 120)
        list(GET st 0 code)
        if(NOT code EQUAL 0)
            file(REMOVE "${dir}/webview.tar.gz")
            message(WARNING "webview: could not fetch it; \"open website\" will use the default browser")
            return()
        endif()
        file(ARCHIVE_EXTRACT INPUT "${dir}/webview.tar.gz" DESTINATION "${dir}/src"
             PATTERNS "webview-${PSP2I_WEBVIEW_VERSION}/core/include/webview/webview.h"
                      "webview-${PSP2I_WEBVIEW_VERSION}/LICENSE" TOUCH)
        file(COPY "${dir}/src/webview-${PSP2I_WEBVIEW_VERSION}/core/include/webview/webview.h" DESTINATION "${inc}/webview")
        file(COPY "${dir}/src/webview-${PSP2I_WEBVIEW_VERSION}/LICENSE" DESTINATION "${dir}")
    endif()

    if(WIN32 AND NOT EXISTS "${inc}/WebView2.h")
        message(STATUS "webview: fetching the WebView2 SDK header ${PSP2I_WEBVIEW2_VERSION}")
        file(DOWNLOAD "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${PSP2I_WEBVIEW2_VERSION}"
             "${dir}/webview2.zip"
             EXPECTED_HASH SHA256=ef128016dd1e51c59178c827ed5b8aa3322c57afa8675d930f8109505542ad74
             STATUS st TIMEOUT 300)
        list(GET st 0 code)
        if(NOT code EQUAL 0)
            file(REMOVE "${dir}/webview2.zip")
            message(WARNING "webview: could not fetch the WebView2 SDK; \"open website\" will use the default browser")
            return()
        endif()
        file(ARCHIVE_EXTRACT INPUT "${dir}/webview2.zip" DESTINATION "${dir}/webview2"
             PATTERNS "build/native/include/WebView2.h" "LICENSE.txt" TOUCH)
        file(COPY "${dir}/webview2/build/native/include/WebView2.h" DESTINATION "${inc}")
        file(COPY "${dir}/webview2/LICENSE.txt" DESTINATION "${dir}")
        file(RENAME "${dir}/LICENSE.txt" "${dir}/WebView2-LICENSE.txt")
    endif()
    set(${inc_out} "${inc}" PARENT_SCOPE)
endfunction()
