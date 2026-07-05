#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

#
# openfpgaOS SDK — Copy to Pocket SD Card
#
# Copies build/<app>/ to the Analogue Pocket SD card.
# The build directory is already a complete SD card image.
#
# Usage: copy.sh <APP_NAME> <ELF_PATH> [SD_PATH]
#
# Platform: Analogue Pocket (APF)
#

set -e

APP="$1"
ELF="$2"
SDCARD="$3"
SDK_ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
BUILD_DIR="$SDK_ROOT/build/pocket/$APP"

GREEN='\033[92m'
RED='\033[91m'
RESET='\033[0m'
ok()   { echo -e "  ${GREEN}+${RESET} $1"; }
fail() { echo -e "  ${RED}x${RESET} $1"; exit 1; }

[[ -z "$APP" ]] && { echo "Usage: $0 <app_name> <elf_path> [sd_path]"; exit 1; }

# ── Find / mount SD card ─────────────────────────────────────────────
source "$SDK_ROOT/scripts/sdcard.sh"

# ── Copy build/<app>/ to SD card ────────────────────────────────────
if [[ -d "$BUILD_DIR/Cores" ]]; then
    echo "Deploying build/pocket/$APP/ to $SDCARD"
    cp -r "$BUILD_DIR"/Cores/* "$SDCARD/Cores/" 2>/dev/null && ok "Cores"
    cp -r "$BUILD_DIR"/Assets/* "$SDCARD/Assets/" 2>/dev/null && ok "Assets"
    cp -r "$BUILD_DIR"/Platforms/* "$SDCARD/Platforms/" 2>/dev/null && ok "Platforms"
else
    fail "build/pocket/$APP/ not found or incomplete. Run 'make' first."
fi

sync 2>/dev/null || true
echo -e "${GREEN}Deployed!${RESET} Eject SD card and boot your Pocket."
