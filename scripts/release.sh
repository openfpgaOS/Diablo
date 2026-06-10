#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

#
# openfpgaOS SDK — GitHub Release Publisher
#
# Publishes a packaged core to GitHub Releases. Assumes the core has already
# been built + packaged (the `make release` target runs `make package` first),
# so releases/<core>-v<version>.zip exists.
#
#   tag    <core>-v<version>   version comes from the core's core.json
#   title  "<Core> for <Product> v<version>"
#   body   commit subjects since the previous <core>-v* tag, or, when there
#          is no previous tag, a single "first release" line
#   asset  releases/<core>-v<version>.zip
#
# Usage:   ./scripts/release.sh <core>
# Env:
#   PREV=<tag>   override the changelog baseline (e.g. PREV=v1.1.12 to bridge
#                Doom's first per-core release off the legacy v1.1.x series)
#   PUBLISH=1    publish a live release instead of a draft (default: draft)
#

set -euo pipefail

GREEN='\033[92m'
CYAN='\033[96m'
YELLOW='\033[93m'
RED='\033[91m'
RESET='\033[0m'

err() { echo -e "${RED}Error:${RESET} $*" >&2; exit 1; }

CORE="${1:-}"
PREV="${PREV:-}"
PUBLISH="${PUBLISH:-0}"

[ -n "$CORE" ] || err "usage: make release CORE=<name> [PREV=<tag>] [PUBLISH=1]"
command -v gh  >/dev/null 2>&1 || err "the GitHub CLI ('gh') is not installed."
command -v git >/dev/null 2>&1 || err "git is not installed."

SDK_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$SDK_DIR"

gh auth status >/dev/null 2>&1 || err "not logged in to GitHub — run 'gh auth login'."

# ── Locate the packaged core and read its metadata ───────────────────
BUNDLE="$SDK_DIR/build/$CORE"
[ -d "$BUNDLE/Cores" ] || err "build/$CORE/ not found — run 'make package CORE=$CORE' first."

CORE_JSON="$(ls "$BUNDLE"/Cores/*/core.json 2>/dev/null | head -1)"
[ -n "$CORE_JSON" ] || err "no core.json under build/$CORE/Cores/."

# Pull version / display name / product straight from the core.json that the
# packager used, so the tag, title and zip name can never disagree.
read_meta() {
    python3 - "$CORE_JSON" <<'PY'
import json, sys
core = json.load(open(sys.argv[1]))["core"]
m    = core["metadata"]
prod = core.get("framework", {}).get("target_product", "Analogue Pocket")
short = m.get("shortname", "")
disp  = short[:1].upper() + short[1:]      # doom -> Doom, heretic -> Heretic
print(m.get("version", ""))
print(disp)
print(prod)
PY
}

{ read -r VERSION; read -r DISPLAY; read -r PRODUCT; } < <(read_meta)
[ -n "$VERSION" ] || err "could not read version from $CORE_JSON."

TAG="$CORE-v$VERSION"
TITLE="$DISPLAY for $PRODUCT v$VERSION"
ZIP="$SDK_DIR/releases/$CORE-v$VERSION.zip"

[ -f "$ZIP" ] || err "asset $ZIP not found — run 'make package CORE=$CORE' first."

# ── Refuse to clobber an already-released version ────────────────────
if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null 2>&1; then
    err "tag $TAG already exists — bump 'version' in core.json before releasing."
fi
if gh release view "$TAG" >/dev/null 2>&1; then
    err "release $TAG already exists on GitHub — bump 'version' in core.json."
fi

# ── Build the release notes ──────────────────────────────────────────
# Baseline = explicit PREV, else the most recent <core>-v* tag. If neither
# exists this is the first release of the core, so say so.
if [ -z "$PREV" ]; then
    PREV="$(git tag --list "$CORE-v*" --sort=-version:refname | head -1)"
fi

NOTES_FILE="$(mktemp)"
trap 'rm -f "$NOTES_FILE"' EXIT

if [ -z "$PREV" ]; then
    echo "First release of the $DISPLAY core." > "$NOTES_FILE"
    RANGE_DESC="first release"
elif ! git rev-parse -q --verify "refs/tags/$PREV" >/dev/null 2>&1; then
    err "baseline tag '$PREV' does not exist (check PREV=)."
else
    git log --no-merges --pretty=format:'- %s' "$PREV..HEAD" > "$NOTES_FILE"
    if [ ! -s "$NOTES_FILE" ]; then
        echo "No changes since $PREV." > "$NOTES_FILE"
    fi
    RANGE_DESC="changes since $PREV"
fi

# ── Warnings (non-fatal) ─────────────────────────────────────────────
if [ -n "$(git status --porcelain)" ]; then
    echo -e "${YELLOW}Warning:${RESET} working tree has uncommitted changes; releasing from $(git rev-parse --short HEAD)."
fi

# ── Create the release ───────────────────────────────────────────────
MODE_FLAG="--draft"
MODE_DESC="DRAFT (review and publish from the GitHub UI)"
if [ "$PUBLISH" = "1" ]; then
    MODE_FLAG="--latest"
    MODE_DESC="LIVE"
fi

echo -e "${CYAN}── Release ─────────────────────────────────────────${RESET}"
echo -e "  core    : $CORE"
echo -e "  tag     : $TAG"
echo -e "  title   : $TITLE"
echo -e "  asset   : ${ZIP#$SDK_DIR/}"
echo -e "  notes   : $RANGE_DESC"
echo -e "  mode    : $MODE_DESC"
echo -e "${CYAN}────────────────────────────────────────────────────${RESET}"
sed 's/^/    /' "$NOTES_FILE"
echo -e "${CYAN}────────────────────────────────────────────────────${RESET}"

gh release create "$TAG" "$ZIP" \
    --target "$(git rev-parse HEAD)" \
    --title "$TITLE" \
    --notes-file "$NOTES_FILE" \
    $MODE_FLAG

echo -e "${GREEN}Release $TAG created ($MODE_DESC).${RESET}"
