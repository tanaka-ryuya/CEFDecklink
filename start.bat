@echo off
setlocal

set TARGET_NAME=DeckLinkDX11.exe

if exist "build\Release\%TARGET_NAME%" set FOUND_PATH=build\Release\%TARGET_NAME%& goto :found
if exist "build\%TARGET_NAME%" set FOUND_PATH=build\%TARGET_NAME%& goto :found
if exist "out\build\x64-Release\%TARGET_NAME%" set FOUND_PATH=out\build\x64-Release\%TARGET_NAME%& goto :found
if exist "out\build\x64-Debug\%TARGET_NAME%" set FOUND_PATH=out\build\x64-Debug\%TARGET_NAME%& goto :found
if exist "build\Debug\%TARGET_NAME%" set FOUND_PATH=build\Debug\%TARGET_NAME%& goto :found

echo [ERROR] %TARGET_NAME% not found.
echo Please build the project first (e.g., using VS Code CMake Tools or 'cmake --build build --config Release').
pause
exit /b 1

:found
echo Starting %FOUND_PATH%...
cd /d "%~dp0"

:: Create logs directory
if not exist "build\Release\logs" mkdir "build\Release\logs"

:: Start the application (it will write its own structured log)
start "" "%FOUND_PATH%"

echo [Monitor] To analyze logs, run:
echo   powershell -ExecutionPolicy Bypass -File scripts\monitor.ps1
echo [Monitor] To follow live:
echo   powershell -ExecutionPolicy Bypass -File scripts\monitor.ps1 -Tail
