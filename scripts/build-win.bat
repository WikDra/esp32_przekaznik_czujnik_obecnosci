@echo off
REM Convenience wrapper: builds the firmware inside WSL from a Windows shell.
REM
REM   scripts\build-win.bat [target]     target: esp32c3 (default) | esp32c6
setlocal
set TARGET=%1
if "%TARGET%"=="" set TARGET=esp32c3
wsl -e bash -lc "cd /mnt/d/wysypisko/esp32_przekaznik_czujnik_obecnosci && ./scripts/build.sh %TARGET%"
exit /b %ERRORLEVEL%
