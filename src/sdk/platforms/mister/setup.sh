#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------
#
# openfpgaOS MiSTer setup — inject user wads + seed the saves image.
#
# The locked per-game model ships TWO images:
#   boot.vhd (S0)         a READ-ONLY shell: the engine + shared /<Game>/common
#                         tree; the user drops IWADs into <Game>/wads/ and this
#                         script injects them.  An update may replace boot.vhd.
#   <Game>.saves.vhd (S1) the writable saves shell — every instance's
#                         preallocated config/save slots.  It is SEEDED once
#                         into the Downloader-reserved saves/ tree and NEVER
#                         overwritten, so updates can't touch your saves.
#
# This script does two independent, idempotent things:
#   1. Seeds  games/OpenfpgaOS/<Game>/<Game>.saves.vhd  ->
#      /media/fat/saves/OpenfpgaOS/<Game>.vhd  — ONLY if the destination does
#      not already exist (first install vs update).  Existing saves are safe.
#   2. Loop-mounts boot.vhd read-write and copies every wad from
#      games/OpenfpgaOS/<Game>/wads/ into its /<Game>/common tree, where the
#      firmware reads them.  Uses only `mount -o loop` + `cp` (MiSTer kernel).
#
# Idempotent: re-running never overwrites the saves image, and only re-copies
# wads whose size differs from the copy already inside boot.vhd.
#
# GAME is baked at package time by rewriting the `GAME_DEFAULT=` line below
# (the packager anchors on `^GAME_DEFAULT=`, so the @GAME@ sentinel in the
# guard further down stays literal); it can also be overridden by argument or
# environment: `setup.sh <Game>` or `GAME=<Game>`.
#------------------------------------------------------------------------------
set -u

GAME_DEFAULT="@GAME@"
GAME="${1:-${GAME:-$GAME_DEFAULT}}"

ROOT=/media/fat/games/OpenfpgaOS
GAME_DIR="$ROOT/$GAME"
WADS="$GAME_DIR/wads"
VHD="$GAME_DIR/boot.vhd"
MNT=/tmp/ofs_${GAME}_mnt
LOG="$GAME_DIR/setup.log"

# Writable per-game saves shell: shipped template (inside the game folder) and
# its device home in the Downloader-reserved saves/ tree (see mkmgl.sh S1 mount).
SAVES_SRC="$GAME_DIR/$GAME.saves.vhd"
SAVES_DIR=/media/fat/saves/OpenfpgaOS
SAVES_DEST="$SAVES_DIR/$GAME.vhd"

ok()   { echo "  [+] $1" | tee -a "$LOG"; }
warn() { echo "  [!] $1" | tee -a "$LOG"; }
die()  { echo "  [x] $1" | tee -a "$LOG"; umount "$MNT" 2>/dev/null; exit 1; }

case "$GAME" in
    ""|"@GAME@") echo "setup.sh: no game baked in — run 'setup.sh <Game>' or set GAME="; exit 1 ;;
esac

: > "$LOG"
echo "openfpgaOS setup — $GAME — $(date)" | tee -a "$LOG"
[ -f "$VHD" ]  || die "no image at $VHD — unzip the game package into $ROOT first"

# ── 1. Seed the writable saves image (first install only, NEVER overwrite) ──
if [ -f "$SAVES_SRC" ]; then
    if [ -f "$SAVES_DEST" ]; then
        ok "saves image present at $SAVES_DEST — left untouched (your saves are safe)"
    else
        mkdir -p "$SAVES_DIR"
        if cp "$SAVES_SRC" "$SAVES_DEST"; then
            ok "seeded saves image -> $SAVES_DEST ($(stat -c%s "$SAVES_SRC") B)"
        else
            warn "failed to seed saves image to $SAVES_DEST — saves will not persist"
        fi
    fi
else
    warn "no saves template at $SAVES_SRC — cannot seed saves image (old package?)"
fi

# ── 2. Inject user wads into the read-only boot shell ───────────────────────
[ -d "$WADS" ] || die "no wads dir at $WADS — create it and drop your IWADs there"

mkdir -p "$MNT"
umount "$MNT" 2>/dev/null || true
mount -o loop,rw "$VHD" "$MNT" || die "loop-mount of $VHD failed"

DEST="$MNT/$GAME/common"
[ -d "$DEST" ] || die "image has no /$GAME/common (wrong/old boot.vhd?)"

copied=0; skipped=0; empty=1
for w in "$WADS"/*; do
    [ -f "$w" ] || continue
    b="$(basename "$w")"
    case "$b" in ._*) continue ;; esac      # AppleDouble junk
    empty=0
    d="$DEST/$b"
    # Idempotent: skip if an identically-sized copy is already inside.
    if [ -f "$d" ] && [ "$(stat -c%s "$w")" = "$(stat -c%s "$d")" ]; then
        skipped=$((skipped+1))
        continue
    fi
    if cp "$w" "$d"; then
        ok "injected $b ($(stat -c%s "$w") B)"
        copied=$((copied+1))
    else
        warn "failed to copy $b"
    fi
done

sync
umount "$MNT" 2>/dev/null
rmdir "$MNT" 2>/dev/null || true

[ "$empty" = 1 ] && warn "no wads found in $WADS — drop your IWADs there and re-run"
ok "$GAME setup done: $copied injected, $skipped already current"
echo "Reload the openfpgaOS core and launch a $GAME .mgl." | tee -a "$LOG"
