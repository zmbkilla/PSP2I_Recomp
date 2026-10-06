@echo off
rem Build the port and hand it to the user for testing: copies psp2i.exe and psp2i.pdb into FinalBuild\.
rem
rem   build_finalbuild.bat           port build, then copy
rem   build_finalbuild.bat regen     regenerate port\gen first (see build_port.bat)
rem   build_finalbuild.bat server    also publish the SEGA test server into FinalBuild\sega_server
rem                                  (close the running server first; it locks its files)
rem
rem Close the game first: a running psp2i.exe cannot be overwritten.
setlocal
set "ROOT=%~dp0"
cd /d "%ROOT%"

set "REGEN="
set "SERVER="
if /i "%~1"=="regen" set "REGEN=regen"
if /i "%~1"=="server" set "SERVER=1"
if /i "%~2"=="server" set "SERVER=1"
if /i "%~2"=="regen" set "REGEN=regen"

call "%ROOT%build_port.bat" %REGEN%
if errorlevel 1 goto failed

echo == Copying to FinalBuild ==
copy /y "%ROOT%port\build\Release\psp2i.exe" "%ROOT%FinalBuild\" >nul
if errorlevel 1 goto locked
copy /y "%ROOT%port\build\Release\psp2i.pdb" "%ROOT%FinalBuild\" >nul
if errorlevel 1 goto locked

if defined SERVER (
    echo == Publishing the SEGA server ==
    if not exist "%ROOT%port\tools\sega_server\SegaServer.csproj" (
        echo port\tools\sega_server is missing.
        goto failed
    )
    pushd port\tools\sega_server
    dotnet publish -c Release -o "%ROOT%FinalBuild\sega_server"
    if errorlevel 1 (popd & goto failed)
    popd
)

echo.
echo Done: FinalBuild\psp2i.exe is up to date.
exit /b 0

:locked
echo.
echo Could not copy into FinalBuild. Is psp2i.exe still running?
:failed
echo.
echo BUILD FAILED.
exit /b 1
