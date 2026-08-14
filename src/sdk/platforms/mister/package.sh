#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------
#
# openfpgaOS SDK — MiSTer platform packager.
#
# Per-game (locked multi-instance) release, built from what the staging dir
# under build/mister/<label>/ carries: one or more <Inst>.mgl launchers plus
# a <Game>/boot.vhd shell.  The game is self-contained and core-agnostic; the
# ZIP unzips into games/OpenfpgaOS/ and the commercial IWADs are user-supplied
# (dropped into <Game>/wads/, injected by setup.sh).  This is the shape
# produced by mkgame.sh + mkmgl.sh.
#
# Called by the generic scripts/package.sh dispatcher.
#
# Usage: package.sh <build_dir> <label> <releases_dir>
#
set -e
shopt -s nullglob
INPUT="$1"; LABEL="$2"; REL="$3"
SDK_DIR="$(cd "$(dirname "$0")/../../../.." && pwd)"
GREEN='\033[92m'; RESET='\033[0m'

# Version from the core's dist metadata (custom cores); SDK demo apps have no
# per-app core.json — fall back to 1.0.0.
game_version() {
    python3 -c "
import json, glob
js = glob.glob('$SDK_DIR/dist/$LABEL/Cores/*/core.json')
print(json.load(open(js[0]))['core']['metadata']['version'] if js else '1.0.0')
" 2>/dev/null || echo "1.0.0"
}

# ── PER-GAME bundle (checked first: a game staging carries .mgl launchers) ──
MGLS=("$INPUT"/*.mgl)
if [ ${#MGLS[@]} -gt 0 ]; then
    # The bundle ships the gzipped shell TEMPLATE (boot*.vhd.gz — setup.sh
    # materializes the working image on-device); accept raw boot*.vhd too for
    # pre-template stages.
    BOOTVHD="$(find "$INPUT" -maxdepth 2 \( -name 'boot*.vhd' -o -name 'boot*.vhd.gz' \) 2>/dev/null | head -1)"
    [ -n "$BOOTVHD" ] || { echo "Error: $LABEL has .mgl launchers but no <Game>/boot*.vhd[.gz]"; exit 1; }
    GAME="$(basename "$(dirname "$BOOTVHD")")"
    VER="$(game_version)"
    OUTPUT="$REL/${LABEL}-v${VER}.zip"

    # Build the zip from a tree rooted at the SD CARD ROOT, not at
    # games/OpenfpgaOS/.  MiSTer's menu browses only the top-level _* trees,
    # so launchers unzipped into games/OpenfpgaOS/ are INVISIBLE and the user
    # has no way to start the game.  Rooting at /media/fat lets one unzip put:
    #   _Computer/_OpenfpgaOS/*.mgl         where the menu actually looks
    #   games/OpenfpgaOS/<Game>/...         where the .mgl expects the data
    #   Scripts/openfpgaos-<game>-setup.sh  runnable from the Scripts menu
    # Running a launcher from _Computer is safe: mkmgl.sh emits games-RELATIVE
    # <file> paths and MiSTer resolves those against the core's games dir, not
    # against the .mgl's own location (see mkmgl.sh PATH RESOLUTION).
    SDROOT="$(mktemp -d)"
    trap 'rm -rf "$SDROOT"' EXIT
    # Per-game menu folder.  Underscore-prefixed at every level: MiSTer only
    # descends into _*-named subdirectories of a _* tree (see setup.sh).
    # OF_MENU_FLAT=1 falls back to the HW-confirmed flat layout.
    if [ "${OF_MENU_FLAT:-0}" = "1" ]; then
        MENU_REL="_Computer/_OpenfpgaOS"
    else
        MENU_REL="_Computer/_OpenfpgaOS/_$GAME"
    fi
    mkdir -p "$SDROOT/games/OpenfpgaOS" "$SDROOT/$MENU_REL" "$SDROOT/Scripts"
    cp -a "$INPUT/." "$SDROOT/games/OpenfpgaOS/"
    # Launchers ALSO at the menu path.  COPIES, not moves: the originals stay
    # under games/OpenfpgaOS/ so a package update rewrites them in place and
    # setup.sh can republish.
    # Underscore prefix is load-bearing at every level; see setup.sh.
    cp -f "$INPUT"/*.mgl "$SDROOT/$MENU_REL/" 2>/dev/null || true
    if [ -f "$INPUT/$GAME/setup.sh" ]; then
        cp -f "$INPUT/$GAME/setup.sh" \
              "$SDROOT/Scripts/openfpgaos-$(printf '%s' "$GAME" | tr 'A-Z' 'a-z')-setup.sh"
    fi

    # Wording for the user-supplied data files.  The wads/ directory name is
    # fixed by mkgame.sh and setup.sh; only the prose varies per game.
    ASSET_LABEL="${ASSET_LABEL:-game data files}"
    ASSET_EG="${ASSET_EXAMPLES:+ (e.g. $ASSET_EXAMPLES)}"

    cat > "$SDROOT/INSTALL.txt" << EOF
$GAME for openfpgaOS (MiSTer) — per-game package

Version: $VER

This package is self-contained and works with the game-agnostic openfpgaOS
core.  Install the core once (openfpgaOS.rbf -> /media/fat/_Computer/,
boot.rom -> /media/fat/games/OpenfpgaOS/), then for each game:

1. Unzip this archive INTO  /media/fat/  (the SD card root).
   It places:
     $MENU_REL/*.mgl   the launchers, ready in the menu
     games/OpenfpgaOS/$GAME/boot*.vhd.gz    game image template (built in step 3)
     games/OpenfpgaOS/$GAME/$GAME.saves.vhd saves template (seeded in step 3)
     games/OpenfpgaOS/$GAME/<inst>.ini      per-instance launch config
     games/OpenfpgaOS/$GAME/wads/           <- drop your $ASSET_LABEL here
     games/OpenfpgaOS/$GAME/external_files.csv  Downloader freeware list
     Scripts/openfpgaos-<game>-setup.sh     the setup script, in the menu

2. Copy the $ASSET_LABEL you own$ASSET_EG into
   games/OpenfpgaOS/$GAME/wads/.  Freeware content can be fetched with
   MiSTer Downloader via external_files.csv.

3. Run setup once (and again after adding files or updating): MiSTer menu ->
   Scripts -> openfpgaos-<game>-setup.  It builds the working boot.vhd from
   the shipped template, seeds  saves/OpenfpgaOS/$GAME.vhd  (only if absent
   -- your saves are never overwritten) and injects your files into
   boot.vhd.  No ssh, no hand-copying.

4. Play:  MiSTer menu -> Computer -> _OpenfpgaOS -> _$GAME -> <instance>.
   The launchers are there from step 1; until step 3 has run they will start
   the core but the game will have no data.

Your saves + settings live in  /media/fat/saves/OpenfpgaOS/$GAME.vhd,
preallocated for power-cut safety and kept on a separate volume so a game
update (which only replaces the boot template) can never touch them.  Never
recreate that image with ordinary tools.  After an update, re-run the setup
script (step 3) -- it rebuilds boot.vhd from the new template and re-injects
your files from wads/ automatically.
EOF

    (cd "$SDROOT" && rm -f "$OUTPUT" 2>/dev/null; \
     zip -r "$OUTPUT" . -x '*.log' '*.mkcommon/*' >/dev/null)

    echo -e "${GREEN}Package created: $OUTPUT${RESET}"
    echo "  Game: $GAME | instances: ${#MGLS[@]} | Size: $(du -h "$OUTPUT" | cut -f1)"
    exit 0
fi

# No .mgl launchers in the staging dir — not a per-game MiSTer bundle, skip.
exit 0
