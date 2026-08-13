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
# Optional extension->image routing, baked at package time like GAME_DEFAULT.
# Space-separated "ext:image" pairs (e.g. "wl6:boot-wl6.vhd sod:boot-sod.vhd")
# for games shipping one boot image per data variant in a single folder;
# empty = classic single boot.vhd behavior.
BOOT_IMAGE_MAP="@BOOT_IMAGE_MAP@"
case "$BOOT_IMAGE_MAP" in @*) BOOT_IMAGE_MAP="" ;; esac
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
if [ -n "$BOOT_IMAGE_MAP" ]; then
    found=0
    for pair in $BOOT_IMAGE_MAP; do
        [ -f "$GAME_DIR/${pair##*:}" ] && found=1
    done
    [ "$found" = 1 ] || die "no boot images in $GAME_DIR — unzip the game package into $ROOT first"
else
    [ -f "$VHD" ] || die "no image at $VHD — unzip the game package into $ROOT first"
fi

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


# ── 2. Publish the launchers where the MiSTer menu can actually see them ──
# MiSTer's main menu browses the top-level _* trees ONLY.  The package unzips
# its .mgl launchers into games/OpenfpgaOS/, which the menu never scans — so
# the documented last step ("pick a .mgl from the menu") was impossible as
# written, and a first-time user has no way to start the game.  Publish this
# game's launchers under the core's own _Computer entry, nested per game so
# several installed games stay navigable:
#     /media/fat/_Computer/_OpenfpgaOS/<Instance>.mgl
#
# COPY, never move: the originals stay in games/OpenfpgaOS/ so a package
# update rewrites them in place and a re-run of this script republishes.
# Running from _Computer is safe — mkmgl.sh emits games-RELATIVE <file> paths
# and MiSTer resolves those against the core's games dir, not against the
# .mgl's own location (see the PATH RESOLUTION note in mkmgl.sh); that is also
# why the S1 saves mount is written absolute.
# A launcher belongs to this game iff it mounts <Game>/boot*.vhd (plain
# boot.vhd or a per-variant boot-<ext>.vhd) — the flat
# games/OpenfpgaOS/ dir holds every installed game's launchers together.
#
# UNDERSCORE PREFIX IS LOAD-BEARING at EVERY level.  MiSTer only descends into
# subdirectories of a _* tree when they are THEMSELVES underscore-prefixed --
# the stock layout is _@Homebrew/_GAMEBOY/*.mgl.  A plain "OpenfpgaOS/" folder
# is not navigable: the menu shows the .rbf cores and nothing else, which reads
# to the user as "it just loads the core, there are no options" (HW-confirmed
# 2026-08-09).  Hence "_OpenfpgaOS", and hence "_$GAME" rather than "$GAME"
# for the per-game level.
#
# LAYOUT: per-game subfolders (_OpenfpgaOS/_Doom/, _OpenfpgaOS/_ScummVM/, ...)
# keep the menu readable once several games are installed.  The single nested
# level is the part that has NOT been confirmed on hardware; if the per-game
# folders turn out not to be navigable, set OF_MENU_FLAT=1 (env) to fall back
# to the previously HW-confirmed flat layout without touching this script.
MENU_ROOT="/media/fat/_Computer/_OpenfpgaOS"
if [ "${OF_MENU_FLAT:-0}" = "1" ]; then
    MENU_DIR="$MENU_ROOT"
else
    MENU_DIR="$MENU_ROOT/_$GAME"
fi
published=0
if mkdir -p "$MENU_DIR" 2>/dev/null; then
    for m in "$ROOT"/*.mgl; do
        [ -f "$m" ] || continue
        grep -q "\"$GAME/boot[^\"]*\.vhd\"" "$m" 2>/dev/null || continue
        cp -f "$m" "$MENU_DIR/" && published=$((published+1))
    done
    sync
    if [ "$published" -gt 0 ]; then
        ok "$published launcher(s) published to $MENU_DIR"
    else
        # No launchers staged under games/ is NORMAL for a Downloader install:
        # the custom DB delivers .mgl straight to $MENU_DIR and deliberately
        # does not duplicate them into the games volume.  Only the ZIP-install
        # path relies on the republish above, so check the destination before
        # crying wolf.
        have=0
        for m in "$MENU_DIR"/*.mgl; do [ -f "$m" ] && have=$((have+1)); done
        if [ "$have" -gt 0 ]; then
            ok "$have launcher(s) already present in $MENU_DIR (Downloader install)"
        else
            warn "no $GAME launchers found in $ROOT or $MENU_DIR — no menu entries"
        fi
    fi
else
    warn "could not create $MENU_DIR — launch a .mgl from $ROOT instead"
fi

# ── 3. Inject user wads into the read-only boot shell ───────────────────────
# Missing wads/ is NOT fatal.  It used to die() here, which meant a card
# without that directory never reached step 2 above and got NO menu entries --
# the user saw nothing to launch and no explanation.  Create it, warn, and
# skip only the injection.
if [ ! -d "$WADS" ]; then
    mkdir -p "$WADS" 2>/dev/null
    warn "no wads dir — created $WADS; drop your IWADs there and re-run"
    ok "$GAME setup done: launchers published, no wads injected"
    echo "Launch from the MiSTer menu: Computer -> OpenfpgaOS -> $GAME -> <instance>." | tee -a "$LOG"
    exit 0
fi

# Which image does a wad belong in?  With BOOT_IMAGE_MAP set, route by
# extension; unmapped extensions fall back to boot.vhd (if present).
image_for() {
    ext="$(echo "${1##*.}" | tr "[:upper:]" "[:lower:]")"
    for pair in $BOOT_IMAGE_MAP; do
        [ "$ext" = "${pair%%:*}" ] && { echo "${pair##*:}"; return; }
    done
    echo "boot.vhd"
}

copied=0; skipped=0; empty=1
IMAGES="boot.vhd"
[ -n "$BOOT_IMAGE_MAP" ] && IMAGES="$(for pair in $BOOT_IMAGE_MAP; do echo "${pair##*:}"; done | sort -u)"
mkdir -p "$MNT"
for img in $IMAGES; do
    IVHD="$GAME_DIR/$img"
    [ -f "$IVHD" ] || { warn "$img not found — skipping"; continue; }
    umount "$MNT" 2>/dev/null || true
    mount -o loop,rw "$IVHD" "$MNT" || die "loop-mount of $IVHD failed"
    DEST="$MNT/$GAME/common"
    [ -d "$DEST" ] || die "image $img has no /$GAME/common (wrong/old shell?)"
    for w in "$WADS"/*; do
        [ -f "$w" ] || continue
        b="$(basename "$w")"
        case "$b" in ._*) continue ;; esac      # AppleDouble junk
        empty=0
        [ "$(image_for "$b")" = "$img" ] || continue
        d="$DEST/$b"
        # Idempotent: skip if an identically-sized copy is already inside.
        if [ -f "$d" ] && [ "$(stat -c%s "$w")" = "$(stat -c%s "$d")" ]; then
            skipped=$((skipped+1))
            continue
        fi
        if cp "$w" "$d"; then
            ok "injected $b -> $img ($(stat -c%s "$w") B)"
            copied=$((copied+1))
        else
            warn "failed to copy $b"
        fi
    done
    sync
    umount "$MNT" 2>/dev/null
done
rmdir "$MNT" 2>/dev/null || true

[ "$empty" = 1 ] && warn "no wads found in $WADS — drop your IWADs there and re-run"


ok "$GAME setup done: $copied injected, $skipped already current"
echo "Launch from the MiSTer menu: Computer -> OpenfpgaOS -> $GAME -> <instance>." | tee -a "$LOG"
