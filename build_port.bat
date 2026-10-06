@echo off
rem Build the PORT (my build playground): port\build\Release\psp2i.exe.
rem
rem   build_port.bat           compile only (what you want most of the time)
rem   build_port.bat regen     first rebuild the recompiler and regenerate port\gen from FinalBuild\EBOOT.BIN
rem                            (needed after changing port\hooks.txt, port\fixes.txt or the recompiler)
rem
rem Nothing here copies into FinalBuild or prod; see build_finalbuild.bat / build_prod.bat.
setlocal
set "ROOT=%~dp0"
cd /d "%ROOT%"

rem Temp files on F: (C: runs out of space).
if not exist "%ROOT%port\testwork\tmp" mkdir "%ROOT%port\testwork\tmp"
set "TEMP=%ROOT%port\testwork\tmp"
set "TMP=%ROOT%port\testwork\tmp"

set "CMAKE=cmake"
where cmake >nul 2>nul
if errorlevel 1 set "CMAKE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

if /i "%~1"=="regen" (
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

echo == Building the port ==
if not exist "port\build\CMakeCache.txt" "%CMAKE%" -S port -B port\build
if errorlevel 1 goto failed
"%CMAKE%" --build port\build --config Release --parallel --target psp2i
if errorlevel 1 goto failed

echo.
echo Done: %ROOT%port\build\Release\psp2i.exe
exit /b 0

:failed
echo.
echo BUILD FAILED.
exit /b 1
