@echo off
:: This is the "Eye" - a persistent setup script for build tools.

:: 1. MSBuild
set "MSBUILD_PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
if exist "%MSBUILD_PATH%" (
    echo [Setup] Found MSBuild at %MSBUILD_PATH%
) else (
    echo [Setup] MSBuild not found at expected location. Searching...
    for /f "delims=" %%i in ('dir /b /s "C:\Program Files\MSBuild.exe"') do set "MSBUILD_PATH=%%i"
)

:: 2. CMake (Search if not in PATH)
where cmake >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo [Setup] CMake not in PATH. Checking common locations...
    if exist "C:\Program Files\CMake\bin\cmake.exe" (
        set "PATH=%PATH%;C:\Program Files\CMake\bin"
        echo [Setup] Added CMake to PATH.
    ) else (
         :: Try Look in VS
         if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
             set "PATH=%PATH%;C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
             echo [Setup] Added VS CMake to PATH.
         )
    )
)

:: 3. Aliases / Macros
doskey build_release="%MSBUILD_PATH%" DeckLinkDX11.sln /p:Configuration=Release
doskey build_debug="%MSBUILD_PATH%" DeckLinkDX11.sln /p:Configuration=Debug

echo [Setup] Environment Ready.
echo Use 'build_release' to build the project.
