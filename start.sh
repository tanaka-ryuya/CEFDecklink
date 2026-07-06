#!/bin/bash
set -e

TARGET_NAME="DeckLinkDX11.app"
BINARY_NAME="DeckLinkDX11"

# Ensure we are in the script directory
cd "$(dirname "$0")"

FOUND_PATH=""

# Search for the application bundle in known build locations
if [ -e "build/Release/${TARGET_NAME}" ]; then
    FOUND_PATH="build/Release/${TARGET_NAME}"
elif [ -e "build/${TARGET_NAME}" ]; then
    FOUND_PATH="build/${TARGET_NAME}"
elif [ -e "build/Debug/${TARGET_NAME}" ]; then
    FOUND_PATH="build/Debug/${TARGET_NAME}"
fi

if [ -z "$FOUND_PATH" ]; then
    echo "[ERROR] ${TARGET_NAME} not found."
    echo "Please build the project first (e.g., run './build.sh')."
    exit 1
fi

BINARY="${FOUND_PATH}/Contents/MacOS/${BINARY_NAME}"

if [ ! -x "$BINARY" ]; then
    echo "[ERROR] Binary not found: ${BINARY}"
    exit 1
fi

# Create logs directory inside the bundle
mkdir -p "${FOUND_PATH}/Contents/MacOS/logs"

BUNDLE_DIR="$(dirname "${FOUND_PATH}")"
echo "[Config] ${BUNDLE_DIR}/config.json"
echo "Starting ${BINARY}..."
echo ""

# Run binary directly (foreground) so TUI works in terminal
exec "${BINARY}"
