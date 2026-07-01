@echo off
setlocal enabledelayedexpansion

:: --- 1. Find MSBuild ---
set "MSBUILD_PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
if not exist "!MSBUILD_PATH!" (
    :: Try VS 18 (VS 2025?)
    set "MSBUILD_PATH=C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
)
if not exist "!MSBUILD_PATH!" (
    :: Fallback search
    for /r "C:\Program Files" %%i in (MSBuild.exe) do (
        if "%%~nxi"=="MSBuild.exe" (
            set "MSBUILD_PATH=%%i"
            goto :found_msbuild
        )
    )
    for /r "C:\Program Files (x86)" %%i in (MSBuild.exe) do (
        if "%%~nxi"=="MSBuild.exe" (
             set "MSBUILD_PATH=%%i"
             goto :found_msbuild
        )
    )
)
:found_msbuild
if not exist "!MSBUILD_PATH!" (
    echo [Error] Could not find MSBuild.exe
    exit /b 1
)
echo [Setup] Found MSBuild: "!MSBUILD_PATH!"

:: --- 2. Add CMake to PATH ---
:: Explicit path found from cache
set "CMAKE_DIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
if not exist "!CMAKE_DIR!\cmake.exe" (
    :: Try VS 18 Path
    set "CMAKE_DIR=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
)
if exist "!CMAKE_DIR!\cmake.exe" (
    set "PATH=!PATH!;!CMAKE_DIR!"
    echo [Setup] Added CMake from BuildTools to PATH.
) else (
    echo [Warning] Explicit CMake path not found. Checking common locations...
    if exist "C:\Program Files\CMake\bin\cmake.exe" set "PATH=%PATH%;C:\Program Files\CMake\bin"
)

:: --- 3. Build Solution ---
echo [Setup] Configuring project...
cmake -S . -B build
if %ERRORLEVEL% NEQ 0 (
    echo [Setup] Configuration Failed!
    exit /b %ERRORLEVEL%
)

echo [Build] Building Release via CMake...
cmake --build build --config Release -v
if %ERRORLEVEL% NEQ 0 (
    echo [Build] Build Failed!
    exit /b %ERRORLEVEL%
)


echo [Build] Build Success!
echo Executable: build\Release\DeckLinkDX11.exe

:: --- 4. Copy generated DeckLinkAPI.h for IntelliSense ---
echo [IntelliSense] Copying generated headers to src\decklink...
if exist "build\DeckLinkDX11.dir\Release\DeckLinkAPI.h" (
    copy /y "build\DeckLinkDX11.dir\Release\DeckLinkAPI.h" "src\decklink\DeckLinkAPI.h" > nul
    echo [IntelliSense] DeckLinkAPI.h copied.
) else if exist "build\DeckLinkDX11.dir\Debug\DeckLinkAPI.h" (
    copy /y "build\DeckLinkDX11.dir\Debug\DeckLinkAPI.h" "src\decklink\DeckLinkAPI.h" > nul
    echo [IntelliSense] DeckLinkAPI.h copied from Debug.
) else (
    echo [Warning] DeckLinkAPI.h not found in build output. IntelliSense may show errors.
)

:: --- 5. Build Installer ---
echo [Installer] Checking for Inno Setup compiler...
set "ISCC_PATH="

:: Try finding in PATH
for %%i in (ISCC.exe) do set "ISCC_PATH=%%~$PATH:i"

:: Try typical installation directories if not found in PATH
if not defined ISCC_PATH (
    if exist "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" (
        set "ISCC_PATH=C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
    ) else if exist "C:\Program Files\Inno Setup 6\ISCC.exe" (
        set "ISCC_PATH=C:\Program Files\Inno Setup 6\ISCC.exe"
    )
)

if defined ISCC_PATH (
    echo [Installer] Found Inno Setup: "!ISCC_PATH!"
    echo [Installer] Building installer package...
    "!ISCC_PATH!" installer.iss
    if errorlevel 1 (
        echo [Installer] Installer compilation failed!
        exit /b 1
    )
    echo [Installer] Installer created successfully: build\CEFDecklink-Setup.exe
) else (
    echo [Installer] Inno Setup ISCC.exe not found. Skipping installer creation.
    echo             Please install Inno Setup to build the installer: https://jrsoftware.org/isdl.php
)

endlocal

