@echo off
rem Build the Windows GUI client. Requires MinGW-w64 gcc in PATH.
setlocal
set "ROOT=%~dp0.."
set "OUT=%ROOT%\dist"

where gcc >nul 2>nul
if errorlevel 1 (
    echo [x] gcc not found in PATH.
    echo     Install MSYS2 mingw-w64 and run this from its shell, or add its bin dir to PATH.
    exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"

gcc -O2 -std=c11 -municode -mwindows -static -s -I"%ROOT%\core" ^
    "%ROOT%\pc\pc_main.c" "%ROOT%\core\ls_core.c" "%ROOT%\core\ls_disc.c" ^
    -o "%OUT%\LanSpeedTest-PC.exe" -lws2_32 -liphlpapi
if errorlevel 1 (
    echo [x] build failed
    exit /b 1
)

echo [ok] %OUT%\LanSpeedTest-PC.exe
for %%F in ("%OUT%\LanSpeedTest-PC.exe") do echo      size: %%~zF bytes
exit /b 0
