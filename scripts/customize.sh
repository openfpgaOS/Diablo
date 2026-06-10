#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

#
# openfpgaOS SDK — Scaffold a custom core
#
# Creates a CUSTOM CORE: a standalone openFPGA core wrapping a single
# app, with its own dist/<name>/, its own Core ID, and its own ZIP. The
# scaffold lives at src/<name>/ and uses scripts/new.sh under the hood.
#
# For the other path — an SDK APP bundled into the shared SDK demo
# core — use `cd src/apps && make new APP=<name>` instead.
#
# Usage:
#   ./scripts/customize.sh                 Interactive mode (prompts)
#   ./scripts/customize.sh --batch ...     Non-interactive (all flags)
#

set -e

SDK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUNTIME="$SDK_ROOT/runtime"
TMPL_DIR="$SDK_ROOT/src/sdk/platforms/pocket/templates"

# ── Defaults ────────────────────────────────────────────────────────
AUTHOR=""
VERSION="1.0.0"
DATE=$(date +%Y-%m-%d)
YEAR=$(date +%Y)
BATCH=0
ICON=""
NAME=""
SHORT=""
PLATFORM=""
TARGET="pocket"

# ── Color helpers ───────────────────────────────────────────────────
GREEN='\033[92m'
RED='\033[91m'
YELLOW='\033[93m'
CYAN='\033[96m'
RESET='\033[0m'

ok()   { echo -e "  ${GREEN}+${RESET} $1"; }
fail() { echo -e "  ${RED}x${RESET} $1"; }
warn() { echo -e "  ${YELLOW}!${RESET} $1"; }

# ── Parse command-line args ─────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --batch)       BATCH=1; shift ;;
        --name)        NAME="$2"; shift 2 ;;
        --author)      AUTHOR="$2"; shift 2 ;;
        --short)       SHORT="$2"; shift 2 ;;
        --platform)    PLATFORM="$2"; shift 2 ;;
        --version)     VERSION="$2"; shift 2 ;;
        --icon)        ICON="$2"; shift 2 ;;
        --target)      TARGET="$2"; shift 2 ;;
        --date)        DATE="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--batch] [options]"
            echo "  Interactive mode (default) prompts for parameters."
            echo "  Use --batch for scripted/CI usage."
            echo ""
            echo "Options:"
            echo "  --name NAME         App display name"
            echo "  --short SHORT       Short name, no spaces"
            echo "  --author AUTHOR     Core author"
            echo "  --version VERSION   Version string [1.0.0]"
            echo "  --icon PATH         Core icon (.bin)"
            echo "  --target TARGET     Platform target [pocket]"
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ── Helpers ─────────────────────────────────────────────────────────
derive_short() {
    echo "$1" | sed 's/[^a-zA-Z0-9]//g'
}

ask() {
    local prompt="$1" default="$2" result
    if [[ -n "$default" ]]; then
        read -rp "$prompt [$default]: " result
        echo "${result:-$default}"
    else
        read -rp "$prompt: " result
        echo "$result"
    fi
}

# ── Interactive mode ────────────────────────────────────────────────
if [[ $BATCH -eq 0 ]]; then
    echo -e "${CYAN}=== openfpgaOS — Scaffold a custom core ===${RESET}"
    echo

    NAME=$(ask "Custom core name" "$NAME")
    [[ -z "$NAME" ]] && { echo "Name is required."; exit 1; }

    default_short=$(derive_short "$NAME")
    SHORT=$(ask "Short name (no spaces)" "${SHORT:-$default_short}")

    # Detect git user as default author
    GIT_AUTHOR=$(git config user.name 2>/dev/null || echo "")
    AUTHOR=$(ask "Author" "${AUTHOR:-$GIT_AUTHOR}")
    [[ -z "$AUTHOR" ]] && { echo "Author is required."; exit 1; }

    VERSION=$(ask "Version" "$VERSION")
    ICON=$(ask "Core icon path (optional, Enter to skip)" "$ICON")

    echo
fi

# ── Validate ────────────────────────────────────────────────────────
[[ -z "$NAME" ]] && { echo "Error: --name required"; exit 1; }
[[ -z "$SHORT" ]] && SHORT=$(derive_short "$NAME")
[[ -z "$AUTHOR" ]] && { echo "Error: --author required"; exit 1; }
[[ -z "$PLATFORM" ]] && PLATFORM=$(echo "$SHORT" | tr '[:upper:]' '[:lower:]')

SNAME=$(echo "$SHORT" | tr '[:upper:]' '[:lower:]')
CORE_ID="${AUTHOR}.${SHORT}"
APP_DIR="$SDK_ROOT/src/$SNAME"

# ── Summary ─────────────────────────────────────────────────────────
if [[ $BATCH -eq 0 ]]; then
    echo -e "${CYAN}--- Summary ---${RESET}"
    echo "  Name:     $NAME"
    echo "  Short:    $SHORT"
    echo "  Author:   $AUTHOR"
    echo "  Core ID:  $CORE_ID"
    echo "  Version:  $VERSION"
    echo "  Target:   $TARGET"
    echo "  Source:   src/$SNAME/"
    echo

    read -rp "Proceed? [Y/n] " confirm
    [[ "$confirm" =~ ^[Nn] ]] && { echo "Aborted."; exit 0; }
    echo
fi

# ── Create app scaffold ────────────────────────────────────────────
if [[ -d "$APP_DIR" ]]; then
    ok "App directory exists: src/$SNAME/"
else
    "$SDK_ROOT/scripts/new.sh" "$SHORT" --target "$TARGET"
fi

# ── Generate dist/<app>/ — static part of the core ─────────────────
DIST_DIR="$SDK_ROOT/dist/$SNAME"
DIST_CORE="$DIST_DIR/Cores/$CORE_ID"
DIST_ASSETS="$DIST_DIR/Assets/$PLATFORM/$CORE_ID"
DIST_PLAT="$DIST_DIR/Platforms"

echo -e "${CYAN}Setting up core: $CORE_ID${RESET}"

if [[ -d "$DIST_DIR" ]]; then
    ok "dist/$SNAME/ already exists"
else
    mkdir -p "$DIST_CORE" "$DIST_ASSETS" "$DIST_PLAT/_images"

    fill_template() {
        local src="$1" dst="$2"
        sed "s/{{APP}}/$SNAME/g; \
             s/{{NAME}}/$NAME/g; \
             s/{{SHORT}}/$SHORT/g; \
             s/{{AUTHOR}}/$AUTHOR/g; \
             s/{{PLATFORM}}/$PLATFORM/g; \
             s/{{VERSION}}/$VERSION/g; \
             s/{{DATE}}/$DATE/g; \
             s/{{YEAR}}/$YEAR/g" \
            "$src" > "$dst"
    }

    # Core configs → dist/<app>/Cores/<CoreID>/
    for tmpl in "$TMPL_DIR"/*.json; do
        fname=$(basename "$tmpl")
        [[ "$fname" == "instance.json" || "$fname" == "platform.json" ]] && continue
        fill_template "$tmpl" "$DIST_CORE/$fname"
    done
    ok "dist/$SNAME/Cores/$CORE_ID/"

    # Platform → dist/<app>/Platforms/
    fill_template "$TMPL_DIR/platform.json" "$DIST_PLAT/${PLATFORM}.json"
    ok "dist/$SNAME/Platforms/"

    # Instance → dist/<app>/Assets/<platform>/<CoreID>/
    fill_template "$TMPL_DIR/instance.json" "$DIST_ASSETS/${SNAME}.json"
    ok "dist/$SNAME/Assets/ (instance)"
fi

# Copy icon if provided
if [[ -n "$ICON" && -f "$ICON" ]]; then
    cp "$ICON" "$DIST_CORE/icon.bin"
    ok "icon.bin"
fi

# ── Set up git remote for SDK upstream tracking ─────────────────────
if git remote get-url origin 2>/dev/null | grep -qi "openfpgaOS-SDK"; then
    git remote rename origin sdk-upstream 2>/dev/null && \
        ok "Renamed origin → sdk-upstream (for SDK updates)"
    echo "  Add your own remote: git remote add origin <your-repo-url>"
fi

# ── Done ────────────────────────────────────────────────────────────
echo
echo -e "${GREEN}Custom core ready!${RESET}"
echo
echo "  cd src/$SNAME"
echo "  make              # build the custom core"
echo "  make copy         # copy this custom core to Pocket SD"
echo "  make test         # test on desktop (SDL2)"
echo "  make package      # package this custom core into a ZIP"
echo
echo "Edit src/$SNAME/main.c to start building your custom core."
