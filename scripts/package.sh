#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------
#
# openfpgaOS SDK — Release Packager (generic dispatcher)
#
# Zips built cores/apps under build/<target>/ into release ZIPs under
# releases/<target>/.  The per-target zip layout lives entirely in
# src/sdk/platforms/<target>/package.sh, so this dispatcher names no
# target — adding a target = add a platforms/<target>/ directory.
#
# Usage:
#   TARGET=<target> ./scripts/package.sh             Package everything built for <target>
#   TARGET=<target> ./scripts/package.sh <Name>      Package only build/<target>/<Name>
#
set -e

SDK_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SPECIFIC="$1"
TARGET="${TARGET:-pocket}"

PLAT="$SDK_DIR/src/sdk/platforms/$TARGET"
[ -f "$PLAT/package.sh" ] || {
    echo "No packager for target '$TARGET' (expected $PLAT/package.sh)."
    echo "Available targets: $(ls "$SDK_DIR/src/sdk/platforms" 2>/dev/null | tr '\n' ' ')"
    exit 1
}

REL="$SDK_DIR/releases/$TARGET"
BUILD="$SDK_DIR/build/$TARGET"
mkdir -p "$REL"

pkg() { bash "$PLAT/package.sh" "$1" "$2" "$REL"; }

if [ -n "$SPECIFIC" ]; then
    [ -d "$BUILD/$SPECIFIC" ] || {
        echo "Error: build/$TARGET/$SPECIFIC/ not found. Build it first (make build CORE=$SPECIFIC TARGET=$TARGET)."
        exit 1
    }
    pkg "$BUILD/$SPECIFIC" "$SPECIFIC"
else
    if [ ! -d "$BUILD" ] || [ -z "$(ls -A "$BUILD" 2>/dev/null)" ]; then
        echo "No $TARGET builds found in build/$TARGET/."
        echo "Build first, e.g.: make build TARGET=$TARGET"
        exit 1
    fi
    for d in "$BUILD"/*/; do
        [ -d "$d" ] || continue
        pkg "$d" "$(basename "$d")"
    done
fi
