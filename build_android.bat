@echo off
rem Build the Android APK into FinalBuild\psp2i-android.apk (port\android\build_local.bat does the work;
rem its header lists the tools it needs in PSP2I_ANDROID_TOOLS, default F:\psp2i-android-tools).
rem
rem   build_android.bat              compile with at most 2 game-code compiles at once
rem   build_android.bat regen        regenerate port\gen from FinalBuild\EBOOT.BIN first
rem   build_android.bat jobs 4       allow 4 compiles at once (each takes ~1-2 GB of memory:
rem                                  keep it low while the game or anything big is running)
rem
rem Close the game first if memory is tight: a large -j ran this PC out of memory before.
setlocal
set "ROOT=%~dp0"
cd /d "%ROOT%"

set "REGEN="
set "JOBS=2"
:args
if "%~1"=="" goto args_done
if /i "%~1"=="regen" (set "REGEN=1" & shift & goto args)
if /i "%~1"=="jobs" (set "JOBS=%~2" & shift & shift & goto args)
echo Unknown option %1 (use: regen, jobs N)
exit /b 2
:args_done

set "CMAKE=cmake"
where cmake >nul 2>nul
if errorlevel 1 set "CMAKE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

if defined REGEN (
    echo == Rebuilding the recompiler ==
    "%CMAKE%" -S psprecomp -B psprecomp\build
    if errorlevel 1 goto failed
    "%CMAKE%" --build psprecomp\build --config Release --target allegrexrecomp
    if errorlevel 1 goto failed
    echo == Regenerating port\gen from FinalBuild\EBOOT.BIN ==
    pushd port
    ..\psprecomp\build\tools\allegrexrecomp\Release\allegrexrecomp.exe emit ..\FinalBuild\EBOOT.BIN gen recomp --hooks hooks.txt --fixes fixes.txt
    if errorlevel 1 (popd & goto failed)
    popd
)

echo == Building the Android APK (%JOBS% compiles at once) ==
call "%ROOT%port\android\build_local.bat" -Ppsp2iJobs=%JOBS%
if errorlevel 1 goto failed
exit /b 0

:failed
echo.
echo BUILD FAILED.
exit /b 1
