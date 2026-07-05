#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

#
# openfpgaOS SDK — Copy to Pocket SD Card
#
# Copies build/pocket/sdk/ (the complete SD card image) to the mounted SD card.
# Run 'make' from src/apps/ first to assemble build/pocket/sdk/.
#
# Usage:
#   ./scripts/copy.sh                       Auto-detect Pocket SD card
#   ./scripts/copy.sh /mnt/sdcard           Copy to specific path
#

set -e

GREEN='\033[92m'
RESET='\033[0m'

# The SDK demo core is a Pocket-only bundle; on MiSTer each app is its own
# disk image, so there is no single "SDK demo core" image to copy.  Steer
# the user to the per-app path instead of silently copying a Pocket tree.
if [ "${TARGET:-pocket}" = "mister" ]; then
    echo "The SDK demo core ships per-app on MiSTer (one disk image each)."
    echo "Push a single app instead, e.g.: make copy APP=<app> TARGET=mister"
    exit 1
fi

SDK_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$SDK_DIR/build/pocket/sdk"

# ── Check build exists ───────────────────────────────────────────────
if [ ! -d "$BUILD_DIR/Cores" ]; then
    echo "Error: build/pocket/sdk/ not found. Run 'make' from src/apps/ first."
    exit 1
fi

# ── Find / mount SD card ─────────────────────────────────────────────
SDCARD="$1"
source "$(dirname "$0")/sdcard.sh"

echo "Deploying build/pocket/sdk/ to $SDCARD"

# ── Sync (only modified files) ───────────────────────────────────────
RSYNC_OPTS=(-rlptv --checksum)

# openfpgaOS folders: sync with --delete to remove stale files.  Every
# ThinkElastic.openfpgaOS* core dir is synced (the os25 demo core AND the os30
# 3D core ship as separate per-variant cores), plus the shared assets.
CORE_SUBS=$(cd "$BUILD_DIR" && ls -d Cores/ThinkElastic.openfpgaOS* 2>/dev/null)
# Asset trees: the os25 demo core (Assets/openfpgaos) AND the os30 3D core, which
# lives on its own platform (Assets/openfpgaos3d) so the Pocket reconfigures the
# FPGA on launch instead of reusing the demo core's already-loaded bitstream.
ASSET_SUBS=$(cd "$BUILD_DIR" && ls -d Assets/openfpgaos* 2>/dev/null)
for sub in $CORE_SUBS $ASSET_SUBS; do
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
