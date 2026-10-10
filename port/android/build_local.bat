@echo off
rem Build the playable Android APK on this PC, with the game code in port\gen.
rem
rem Everything the build needs lives in one folder, PSP2I_ANDROID_TOOLS
rem (default F:\psp2i-android-tools): jdk17\, sdk\ (platform 35, build-tools 34,
rem NDK 27, CMake 3.22), gradle\ (Gradle and its cache), android-user\ (the
rem debug signing key), home\ and tmp\. Delete that folder to remove all of it.
rem Nothing is written to C:.
rem
rem Result: app\build\outputs\apk\release\app-release.apk, copied to
rem FinalBuild\psp2i-android.apk. It contains your game code: keep it private.
setlocal
if not defined PSP2I_ANDROID_TOOLS set "PSP2I_ANDROID_TOOLS=F:\psp2i-android-tools"
set "T=%PSP2I_ANDROID_TOOLS%"
if not exist "%T%\jdk17\bin\java.exe" (echo %T%\jdk17 is missing. & exit /b 1)
if not exist "%T%\sdk\ndk" (echo %T%\sdk has no NDK. & exit /b 1)
if not exist "%~dp0..\gen\recomp_funcs.c" (echo port\gen is missing: generate it from your EBOOT first. & exit /b 1)

set "JAVA_HOME=%T%\jdk17"
set "ANDROID_HOME=%T%\sdk"
set "ANDROID_SDK_ROOT=%T%\sdk"
set "ANDROID_USER_HOME=%T%\android-user"
set "GRADLE_USER_HOME=%T%\gradle"
set "TMP=%T%\tmp"
set "TEMP=%T%\tmp"
set "JAVA_TOOL_OPTIONS=-Djava.io.tmpdir=%T%\tmp -Duser.home=%T%\home"
if not exist "%T%\tmp" mkdir "%T%\tmp"

cd /d "%~dp0"
rem Gradle finds the SDK through local.properties (git-ignored); backslashes escaped.
> local.properties echo sdk.dir=%ANDROID_HOME:\=\\%

call "%~dp0gradlew.bat" assembleRelease --no-daemon %*
if errorlevel 1 (echo. & echo BUILD FAILED. & exit /b 1)

copy /y "app\build\outputs\apk\release\app-release.apk" "%~dp0..\..\FinalBuild\psp2i-android.apk" >nul
echo.
echo Done: FinalBuild\psp2i-android.apk (contains your game code; keep it private).
exit /b 0
