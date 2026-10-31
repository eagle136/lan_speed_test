@echo off
rem Build and run the core loopback self-test.
rem Usage: build_and_run.bat [1|0]   1 = pipeline thread on (default), 0 = off
setlocal
set "ROOT=%~dp0.."
set "MODE=%~1"
if "%MODE%"=="" set "MODE=1"
set "OUTDIR=%ROOT%\build"

where gcc >nul 2>nul
if errorlevel 1 (
    echo [x] gcc not found in PATH.
    exit /b 1
)

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

gcc -O2 -std=c11 -I"%ROOT%\core" ^
    "%ROOT%\tests\ls_selftest.c" "%ROOT%\core\ls_core.c" "%ROOT%\core\ls_disc.c" ^
    -o "%OUTDIR%\ls_selftest.exe" -lws2_32 -liphlpapi
if errorlevel 1 (
    echo [x] build failed
    exit /b 1
)

echo [run] pipeline=%MODE%
"%OUTDIR%\ls_selftest.exe" %MODE%
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
    echo [x] SELFTEST FAILED ^(exit %RC%^)
    exit /b %RC%
)
echo [ok] SELFTEST OK
exit /b 0
