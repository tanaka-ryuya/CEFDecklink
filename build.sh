#!/bin/bash
set -e

# Ensure we are in the script directory
cd "$(dirname "$0")"

# --- Dependency Check: DeckLink SDK ---
DECKLINK_SDK_DIR="vender/Blackmagic DeckLink SDK 15.3"
DECKLINK_HEADER_MAC="${DECKLINK_SDK_DIR}/Mac/include/DeckLinkAPI.h"

if [ ! -f "${DECKLINK_HEADER_MAC}" ]; then
    echo ""
    echo "============================================================"
    echo "  ERROR: Blackmagic DeckLink SDK not found!"
    echo "============================================================"
    echo ""
    echo "  The DeckLink SDK headers are required to build this project."
    echo "  They cannot be included in this repository due to Blackmagic's EULA."
    echo ""
    echo "  Please follow these steps:"
    echo "    1. Download the DeckLink SDK from Blackmagic Design:"
    echo "       https://www.blackmagicdesign.com/support/"
    echo "       (Search for 'Desktop Video SDK' or 'DeckLink SDK')"
    echo ""
    echo "    2. Extract the downloaded archive."
    echo ""
    echo "    3. Place the extracted SDK folder into the 'vender' directory:"
    echo "       Expected path: ${DECKLINK_SDK_DIR}/"
    echo ""
    echo "    4. Run this script again."
    echo "============================================================"
    echo ""
    exit 1
fi

echo "Found DeckLink SDK at: ${DECKLINK_SDK_DIR}"

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
