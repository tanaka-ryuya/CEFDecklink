#!/bin/bash
set -e

TARGET_NAME="DeckLinkDX11.app"
ZIP_NAME="DeckLinkDX11_mac.zip"
PACKAGE_DIR_NAME="DeckLinkDX11_mac"

# Ensure we are in the script directory
cd "$(dirname "$0")"

FOUND_APP=""

# Search for the application bundle in known build locations
if [ -e "build/Release/${TARGET_NAME}" ]; then
    FOUND_APP="build/Release/${TARGET_NAME}"
    CONFIG_DIR="build/Release"
elif [ -e "build/${TARGET_NAME}" ]; then
    FOUND_APP="build/${TARGET_NAME}"
    CONFIG_DIR="build"
elif [ -e "build/Debug/${TARGET_NAME}" ]; then
    FOUND_APP="build/Debug/${TARGET_NAME}"
    CONFIG_DIR="build/Debug"
fi

if [ -z "$FOUND_APP" ]; then
    echo "[ERROR] ${TARGET_NAME} not found."
    echo "Please build the project first (e.g., run './build.sh')."
    exit 1
fi

# Create a temporary staging directory
STAGING_DIR="$(mktemp -d)"
PACKAGE_PATH="${STAGING_DIR}/${PACKAGE_DIR_NAME}"
mkdir -p "${PACKAGE_PATH}"

echo "Staging: ${FOUND_APP} → ${PACKAGE_PATH}/"
# Copy .app bundle (preserve symlinks with -R)
cp -R "${FOUND_APP}" "${PACKAGE_PATH}/"

# Copy config.json (from next to .app, then fallback to project root)
if [ -f "${CONFIG_DIR}/config.json" ]; then
    cp "${CONFIG_DIR}/config.json" "${PACKAGE_PATH}/"
elif [ -f "config.json" ]; then
    cp "config.json" "${PACKAGE_PATH}/"
else
    echo "[WARNING] config.json not found. Skipping."
fi

echo "Creating ZIP: ${ZIP_NAME}"
# -y preserves symlinks (critical for .app bundles)
(cd "${STAGING_DIR}" && zip -r -y "${OLDPWD}/${ZIP_NAME}" "${PACKAGE_DIR_NAME}")

# Cleanup
rm -rf "${STAGING_DIR}"

echo ""
echo "=========================================================="
echo "  Done! Created: ${ZIP_NAME}"
echo "  Contents:"
echo "    ${PACKAGE_DIR_NAME}/"
echo "    ├── ${TARGET_NAME}"
echo "    └── config.json"
echo "=========================================================="
