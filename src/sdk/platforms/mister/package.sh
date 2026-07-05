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
    BOOTVHD="$(find "$INPUT" -maxdepth 2 -name boot.vhd 2>/dev/null | head -1)"
    [ -n "$BOOTVHD" ] || { echo "Error: $LABEL has .mgl launchers but no <Game>/boot.vhd"; exit 1; }
    GAME="$(basename "$(dirname "$BOOTVHD")")"
    VER="$(game_version)"
    OUTPUT="$REL/${LABEL}-v${VER}.zip"

    cat > "$INPUT/INSTALL.txt" << EOF
$GAME for openfpgaOS (MiSTer) — per-game package

Version: $VER

This package is self-contained and works with the game-agnostic openfpgaOS
core.  Install the core once (openfpgaOS.rbf -> /media/fat/_Computer/,
boot.rom -> /media/fat/games/OpenfpgaOS/), then for each game:

1. Unzip this archive INTO  /media/fat/games/OpenfpgaOS/
   You get:
     $GAME.mgl, <more>.mgl        one launcher per instance
     $GAME/boot.vhd               read-only game image (shell — no IWADs yet)
     $GAME/$GAME.saves.vhd        saves template (setup.sh seeds it, see below)
     $GAME/<inst>.ini             per-instance launch config
     $GAME/wads/                  <- drop your IWADs here
     $GAME/setup.sh               seeds saves + injects your wads
     $GAME/external_files.csv     MiSTer-Downloader freeware wad list

2. Copy the commercial IWADs you own (e.g. DOOM2.WAD, PLUTONIA.WAD, TNT.WAD)
   into  games/OpenfpgaOS/$GAME/wads/.  Freeware wads can be fetched with
   MiSTer Downloader via external_files.csv.

3. Run setup once (and again after adding wads):
     copy  $GAME/setup.sh  to  /media/fat/Scripts/  and run it from the
     Scripts menu, or over ssh:  bash $GAME/setup.sh $GAME
   It seeds  saves/OpenfpgaOS/$GAME.vhd  (only if absent — your saves are
   never overwritten) and injects your wads into boot.vhd.

4. Pick a $GAME .mgl from the MiSTer menu to play.

Your saves + settings live in  /media/fat/saves/OpenfpgaOS/$GAME.vhd,
preallocated for power-cut safety and kept on a separate volume so a game
update (which only replaces boot.vhd) can never touch them.  Never recreate
that image with ordinary tools.
EOF

    (cd "$INPUT" && rm -f "$OUTPUT" 2>/dev/null; \
     zip -r "$OUTPUT" . -x '*.log' '*.mkcommon/*' >/dev/null)

    echo -e "${GREEN}Package created: $OUTPUT${RESET}"
    echo "  Game: $GAME | instances: ${#MGLS[@]} | Size: $(du -h "$OUTPUT" | cut -f1)"
    exit 0
fi

# No .mgl launchers in the staging dir — not a per-game MiSTer bundle, skip.
exit 0
