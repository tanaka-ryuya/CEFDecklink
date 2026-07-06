#!/bin/bash
set -e

# Ensure we are in the script directory
cd "$(dirname "$0")"

# 1. Create build output directory
mkdir -p build

# 2. Run CMake generation
echo "Configuring project with CMake..."
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 3. Run Build
echo "Building project in Release mode..."
cmake --build build --config Release

echo "=========================================================="
echo "Build Successful!"
echo "Main Application Bundle: build/Release/DeckLinkDX11.app"
echo "To run the application in CUI mode, execute:"
echo "  ./build/Release/DeckLinkDX11.app/Contents/MacOS/DeckLinkDX11"
echo "=========================================================="
