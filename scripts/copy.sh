#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

#
# openfpgaOS SDK — Copy to Pocket SD Card
#
# Copies build/sdk/ (the complete SD card image) to the mounted SD card.
# Run 'make' from src/apps/ first to assemble build/sdk/.
#
# Usage:
#   ./scripts/copy.sh                       Auto-detect Pocket SD card
#   ./scripts/copy.sh /mnt/sdcard           Copy to specific path
#

set -e

GREEN='\033[92m'
RESET='\033[0m'

SDK_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$SDK_DIR/build/sdk"

# ── Check build exists ───────────────────────────────────────────────
if [ ! -d "$BUILD_DIR/Cores" ]; then
    echo "Error: build/sdk/ not found. Run 'make' from src/apps/ first."
    exit 1
fi

# ── Find / mount SD card ─────────────────────────────────────────────
SDCARD="$1"
source "$(dirname "$0")/sdcard.sh"

echo "Deploying build/sdk/ to $SDCARD"

# ── Sync (only modified files) ───────────────────────────────────────
RSYNC_OPTS=(-rlptv --checksum)

# openfpgaOS folders: sync with --delete to remove stale files
for sub in Cores/ThinkElastic.openfpgaOS Assets/openfpgaos; do
    if [ -d "$BUILD_DIR/$sub" ]; then
        mkdir -p "$SDCARD/$sub"
        rsync "${RSYNC_OPTS[@]}" --delete "$BUILD_DIR/$sub/" "$SDCARD/$sub/"
        echo -e "  ${GREEN}+${RESET} $sub"
    fi
done

# Platforms: sync without --delete (shared with other cores)
if [ -d "$BUILD_DIR/Platforms" ]; then
    mkdir -p "$SDCARD/Platforms"
    rsync "${RSYNC_OPTS[@]}" "$BUILD_DIR/Platforms/" "$SDCARD/Platforms/"
    echo -e "  ${GREEN}+${RESET} Platforms"
fi

sync 2>/dev/null || true
echo -e "\n${GREEN}Deployed!${RESET} Eject SD card and boot your Pocket."
