@echo off
REM Flash a firmware built inside WSL, using esptool from the Windows ESP-IDF install.
REM
REM   scripts\flash-win.bat [COM port] [target]
REM   scripts\flash-win.bat COM5 esp32c3
REM
REM Note: MSYSTEM is cleared because ESP-IDF export.bat refuses to run under Git Bash/MSYS.
setlocal
set PORT=%1
if "%PORT%"=="" set PORT=COM5
set TARGET=%2
if "%TARGET%"=="" set TARGET=esp32c3

set IDF_WIN=C:\Users\1thew\esp\v5.5.1\esp-idf
set BUILD_DIR=%~dp0..\firmware\build.%TARGET%

if not exist "%BUILD_DIR%\flash_args" (
    echo Build directory %BUILD_DIR% not found - run "wsl ./scripts/build.sh %TARGET%" first.
    exit /b 1
)

set MSYSTEM=
call "%IDF_WIN%\export.bat" >nul 2>&1
if errorlevel 1 exit /b 1

pushd "%BUILD_DIR%"
python -m esptool --chip %TARGET% --port %PORT% --baud 921600 write_flash @flash_args
set RC=%ERRORLEVEL%
popd
exit /b %RC%
