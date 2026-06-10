#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

#
# openfpgaOS SDK — Release Packager
#
# Packages custom game cores and/or the SDK release into distributable ZIPs.
# Auto-detects custom cores in build/ (created by customize.sh + make).
#
# Usage:
#   ./scripts/package.sh                    Package SDK + all custom cores found
#   ./scripts/package.sh <ShortName>        Package only a specific custom core
#

set -e

GREEN='\033[92m'
CYAN='\033[96m'
RESET='\033[0m'

SDK_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SPECIFIC="$1"

mkdir -p "$SDK_DIR/releases"

package_core() {
    local INPUT="$1"
    local LABEL="$2"

    [ ! -d "$INPUT/Cores" ] && return

    # Detect core name
    local CORE_NAME=$(ls "$INPUT/Cores/" 2>/dev/null | head -1)
    [ -z "$CORE_NAME" ] && return

    # Read metadata
    local GAME_NAME=$(python3 -c "
import json
with open('$INPUT/Cores/$CORE_NAME/core.json') as f:
    d = json.load(f)
print(d['core']['metadata']['description'])
" 2>/dev/null || echo "$LABEL")

    local GAME_VERSION=$(python3 -c "
import json
with open('$INPUT/Cores/$CORE_NAME/core.json') as f:
    d = json.load(f)
print(d['core']['metadata']['version'])
" 2>/dev/null || echo "1.0.0")

    local OUTPUT="$SDK_DIR/releases/${LABEL}-v${GAME_VERSION}.zip"

    # Generate INSTALL.txt
    cat > "$INPUT/INSTALL.txt" << EOF
$GAME_NAME
$(printf '=%.0s' $(seq 1 ${#GAME_NAME}))

Version: $GAME_VERSION

Installation:
1. Extract this ZIP to your Analogue Pocket SD card root
2. Merge with existing folders if prompted
3. The game will appear in the Pocket menu

Save files are created automatically on first use.
EOF

    # Create ZIP
    cd "$INPUT"
    rm -f "$OUTPUT" 2>/dev/null || true
    zip -r "$OUTPUT" \
        Cores/ Assets/ Platforms/ INSTALL.txt \
        -x "*.DS_Store" "Thumbs.db" 2>/dev/null
    cd "$SDK_DIR"

    echo -e "${GREEN}Package created: $OUTPUT${RESET}"
    echo "  Size: $(du -h "$OUTPUT" | cut -f1)"
}

if [ -n "$SPECIFIC" ]; then
    # Package a specific core
    if [ -d "$SDK_DIR/build/$SPECIFIC" ]; then
        package_core "$SDK_DIR/build/$SPECIFIC" "$SPECIFIC"
    else
        echo "Error: build/$SPECIFIC/ not found. Run 'make' first."
        exit 1
    fi
else
    # Package SDK if present
    if [ -d "$SDK_DIR/build/sdk/Cores" ]; then
        package_core "$SDK_DIR/build/sdk" "openfpgaOS-SDK"
    fi

    # Package all custom cores
    for coredir in "$SDK_DIR/build"/*/; do
        name=$(basename "$coredir")
        [ "$name" = "sdk" ] && continue
        [ ! -d "$coredir/Cores" ] && continue
        package_core "$coredir" "$name"
    done
fi
