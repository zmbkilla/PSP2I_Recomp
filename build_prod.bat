@echo off
rem Build the COMMUNITY build (PSP2I_PROD): no command line, data found from the exe's folder, icon from
rem asset\icon.ico. The result is copied into prod\ together with SDL3.dll and the FFmpeg DLLs by the
rem build itself. It never touches prod\GameData.
rem
rem   build_prod.bat           regenerate port\gen, compile + copy into prod\ (the same as
rem                            "build_prod.bat regen"; double-clicking does this and waits at the end)
rem   build_prod.bat quick     compile + copy only (port\gen already current)
rem
rem Uses its own build tree (port\build-prod), separate from the port build. Never test inside prod\.
setlocal
set "MODE=%~1"
if "%MODE%"=="" set "MODE=regen"
set "ROOT=%~dp0"
cd /d "%ROOT%"

if not exist "%ROOT%port\testwork\tmp" mkdir "%ROOT%port\testwork\tmp"
set "TEMP=%ROOT%port\testwork\tmp"
set "TMP=%ROOT%port\testwork\tmp"

set "CMAKE=cmake"
where cmake >nul 2>nul
if errorlevel 1 set "CMAKE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

if /i "%MODE%"=="regen" (
    echo == Rebuilding the recompiler ==
    "%CMAKE%" -S psprecomp -B psprecomp\build
    if errorlevel 1 goto failed
    "%CMAKE%" --build psprecomp\build --config Release --target allegrexrecomp
    if errorlevel 1 goto failed
    echo == Regenerating port\gen from FinalBuild\EBOOT.BIN ==
    pushd port
    ..\psprecomp\build\tools\allegrexrecomp\Release\allegrexrecomp.exe emit ..\FinalBuild\EBOOT.BIN gen recomp --hooks hooks.txt --fixes fixes.txt --switches switches.txt
    if errorlevel 1 (popd & goto failed)
    popd
)

echo == Building the community build ==
"%CMAKE%" -S port -B port\build-prod -DPSP2I_PROD=ON
if errorlevel 1 goto failed
"%CMAKE%" --build port\build-prod --config Release --parallel --target psp2i
if errorlevel 1 goto failed

echo.
echo Done: %ROOT%prod\psp2i.exe
if "%~1"=="" pause
exit /b 0

:failed
echo.
echo BUILD FAILED. (If the copy into prod failed, close psp2i.exe first.)
if "%~1"=="" pause
exit /b 1
