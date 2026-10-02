#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------
#
# openfpgaOS SDK — Pocket platform packager.
#
# Zips one built Pocket bundle (an APF Cores/Assets/Platforms tree under
# build/pocket/<label>/) into a release ZIP.  Called by the generic
# scripts/package.sh dispatcher — adding a target = add a platform dir
# with its own package.sh of this same shape.
#
# Usage: package.sh <build_dir> <label> <releases_dir>
#   build_dir     a build/pocket/<x>/ directory (APF tree)
#   label         zip basename (the demo core "sdk" is renamed below)
#   releases_dir  where the .zip lands (releases/pocket)
#
set -e
INPUT="$1"; LABEL="$2"; REL="$3"
GREEN='\033[92m'; RESET='\033[0m'
EXCLUDES=("*.DS_Store" "Thumbs.db")
ENGINE_MPQS=()

[ -d "$INPUT/Cores" ] || exit 0          # not an APF tree — nothing to do
INPUT=$(cd "$INPUT" && pwd)
mkdir -p "$REL"
REL=$(cd "$REL" && pwd)
[ "$LABEL" = "sdk" ] && LABEL="openfpgaOS-SDK"

CORE_NAME=$(ls "$INPUT/Cores/" 2>/dev/null | head -1)
[ -z "$CORE_NAME" ] && exit 0

GAME_NAME=$(python3 -c "
import json
print(json.load(open('$INPUT/Cores/$CORE_NAME/core.json'))['core']['metadata']['description'])
" 2>/dev/null || echo "$LABEL")
GAME_VERSION=$(python3 -c "
import json
print(json.load(open('$INPUT/Cores/$CORE_NAME/core.json'))['core']['metadata']['version'])
" 2>/dev/null || echo "1.0.0")

OUTPUT="$REL/${LABEL}-v${GAME_VERSION}.zip"

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

case "$LABEL" in
    diablo)
        # Local installs may stage purchased game data beside the engine
        # assets. Only the redistributable engine/font MPQs belong in a ZIP.
        EXCLUDES+=("*.[mM][pP][qQ]")
        while IFS= read -r -d '' asset; do
            case "$(basename "$asset" | tr '[:upper:]' '[:lower:]')" in
                devilutionx.mpq|fonts.mpq) ENGINE_MPQS+=("${asset#"$INPUT"/}") ;;
            esac
        done < <(find "$INPUT" -type f -iname '*.mpq' -print0)
        cat >> "$INPUT/INSTALL.txt" << EOF

Diablo and Hellfire game data are not included.
Copy DIABDAT.mpq from your own Diablo copy to Assets/diablo/common/.
For Hellfire, also copy hellfire.mpq, hfmonk.mpq, hfmusic.mpq and hfvoice.mpq.
The engine requires devilutionx.mpq in the same folder; if it is absent,
obtain it from the DevilutionX 1.5.5 release. fonts.mpq is optional.
Keep your existing Saves/ folder when upgrading.
EOF
        ;;
    doom|heretic|hexen)
        EXCLUDES+=("*.[wW][aA][dD]")
        cat >> "$INPUT/INSTALL.txt" << EOF

Game WADs are not included. Copy your WAD files to Assets/$LABEL/common/.
EOF
        ;;
esac

PACKAGE_TMP=$(mktemp -d "$REL/.package.XXXXXX")
trap 'rm -rf "$PACKAGE_TMP"' EXIT
(cd "$INPUT" && \
 zip -r "$PACKAGE_TMP/bundle.zip" Cores/ Assets/ Platforms/ INSTALL.txt \
     -x "${EXCLUDES[@]}" >/dev/null)
if [ "${#ENGINE_MPQS[@]}" -gt 0 ]; then
    (cd "$INPUT" && zip "$PACKAGE_TMP/bundle.zip" "${ENGINE_MPQS[@]}" >/dev/null)
fi
mv "$PACKAGE_TMP/bundle.zip" "$OUTPUT"

echo -e "${GREEN}Package created: $OUTPUT${RESET}"
echo "  Size: $(du -h "$OUTPUT" | cut -f1)"
