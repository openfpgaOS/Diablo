#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

#
# openfpgaOS SDK — Emit the per-instance .mgl launchers for a game.
#
# In the MiSTer locked distribution model each library entry launches from a
# .mgl (MiSTer Game Launcher) in games/OpenfpgaOS/.  One .mgl per instance,
# with FOUR <file> actions in a delay-ORDERED sequence:
#   S0 (index 0)  mount the read-only boot.vhd (shared common tree: bank+wads)
#   S1 (index 1)  mount the writable per-game saves shell (config + saves)
#   F  (index 2)  F-load the loose engine ELF over ioctl — the RTL DMAs it to
#                 the ELF staging window and sets HPS_STATUS_ELF_LOADED; the OS
#                 then serves the app (FatFs slot 3 = app.elf) from that staging
#                 instead of the mounted boot.vhd.  The ELF is a plain loose
#                 file the Downloader keeps in sync on its own, so an engine
#                 update is a single loose-file swap (boot.vhd never changes).
#   F  (index 1)  F-load the instance's loose .ini over ioctl — the OS reads
#                 its [os] GAME/INSTANCE to pick common_root (reads on S0) and
#                 instance_root (writes pinned to S1):
#
#   <mistergamedescription>
#       <rbf>_Computer/OpenfpgaOS</rbf>
#       <file delay="1" type="s" index="0" path="<Game>/boot.vhd"/>
#       <file delay="2" type="s" index="1" path="/media/fat/saves/OpenfpgaOS/<Game>.vhd"/>
#       <file delay="3" type="f" index="2" path="<Game>/<elf>"/>
#       <file delay="4" type="f" index="1" path="<Game>/<inst>.ini"/>
#   </mistergamedescription>
#
# ORDER IS CORRECTNESS-CRITICAL — the ELF F-load (index 2) MUST come BEFORE the
# ini F-load (index 1) in delay order.  The index-1 ini F-load triggers a SoC
# reset (ini_reset); on the rebooted boot the OS waits for the ini and then
# immediately loads the app (slot 3) from ELF staging.  If the ELF were not yet
# staged (HPS_STATUS_ELF_LOADED clear) the slot-3 read falls back to a boot.vhd
# that — in this model — no longer carries the ELF at all, so the launch fails.
# The index-2 ELF F-load does NOT reset; staging the ELF first, then the ini,
# guarantees ELF_LOADED is set by the time the app load runs.  Do NOT reorder.
#
# PATH RESOLUTION (MiSTer Main menu.cpp): a <file> path is used VERBATIM when
# it starts with '/', else it is prefixed with HomeDir() == the core's games
# dir (/media/fat/games/OpenfpgaOS).  So the S0 boot.vhd and the F-loaded ini
# use games-relative paths, but the saves shell lives in the Downloader-
# reserved /media/fat/saves/ tree (which an update never overwrites) — a
# games-relative "saves/OpenfpgaOS/<Game>.vhd" would wrongly resolve under
# games/OpenfpgaOS/, so the S1 mount MUST be an ABSOLUTE path.
#
# The .mgl filename is the Capitalized instance stem (the [os] INSTANCE=
# value, e.g. Plutonia.mgl); the F-loaded ini keeps its lowercase on-card
# filename (plutonia.ini).
#
# Usage:
#   mkmgl.sh <Game> <inis_dir> <out_dir>
#
# Platform: MiSTer (DE10-Nano / SuperStation One)
#

set -e
export LC_ALL=C
shopt -s nullglob

GREEN='\033[92m'
YELLOW='\033[93m'
RED='\033[91m'
RESET='\033[0m'
ok()   { echo -e "  ${GREEN}+${RESET} $1"; }
warn() { echo -e "  ${YELLOW}!${RESET} $1"; }
fail() { echo -e "  ${RED}x${RESET} $1"; exit 1; }

RBF="_Computer/OpenfpgaOS"    # the game-agnostic core, per the locked model
# The writable per-game saves shell lives in the Downloader-reserved saves/
# tree; the S1 mount references it by ABSOLUTE path (see header — a games-
# relative path would resolve under games/OpenfpgaOS/, the wrong tree).
SAVES_DIR="/media/fat/saves/OpenfpgaOS"

# --single: emit ONE "<Game>.mgl" bootstrap launcher (boots a default instance;
# the user switches wads/mods in-OS via the core's "Load Instance" browser)
# instead of one launcher per instance.  All instances share the same boot.vhd
# + engine, so a single launcher + in-OS switching reaches every wad.
SINGLE=0
[[ "$1" == "--single" ]] && { SINGLE=1; shift; }

GAME="$1"
INIS_DIR="$2"
OUT_DIR="$3"

[[ -z "$GAME" || -z "$INIS_DIR" || -z "$OUT_DIR" ]] && {
    echo "Usage: $0 [--single] <Game> <inis_dir> <out_dir>"
    exit 1
}
[[ -d "$INIS_DIR" ]] || fail "inis dir not found: $INIS_DIR"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=validate-ini.sh
source "$SCRIPT_DIR/validate-ini.sh"

mkdir -p "$OUT_DIR"

# emit_mgl <mgl_stem> <elf> <ini_basename> — write one launcher.  The four
# <file> lines stay in strict delay order (ELF index-2 BEFORE ini index-1; only
# the ini F-load resets the SoC and the OS loads the app from ELF staging on
# that reboot — see header, "ORDER IS CORRECTNESS-CRITICAL").
emit_mgl() {
    local stem_="$1" elf_="$2" base_="$3"
    {
        printf '<mistergamedescription>\n'
        printf '\t<rbf>%s</rbf>\n' "$RBF"
        printf '\t<file delay="1" type="s" index="0" path="%s/boot.vhd"/>\n' "$GAME"
        printf '\t<file delay="2" type="s" index="1" path="%s/%s.vhd"/>\n' "$SAVES_DIR" "$GAME"
        printf '\t<file delay="3" type="f" index="2" path="%s/%s"/>\n' "$GAME" "$elf_"
        printf '\t<file delay="4" type="f" index="1" path="%s/%s"/>\n' "$GAME" "$base_"
        printf '</mistergamedescription>\n'
    } > "$OUT_DIR/$stem_.mgl"
    ok "$stem_.mgl -> S0 $GAME/boot.vhd + S1 $SAVES_DIR/$GAME.vhd + F2(elf) $GAME/$elf_ + F1(ini) $GAME/$base_"
}

INIS=("$INIS_DIR"/*.ini)
[[ ${#INIS[@]} -gt 0 ]] || fail "no *.ini in $INIS_DIR"

COUNT=0
if [[ "$SINGLE" == 1 ]]; then
    # BOOTSTRAP launcher "<Game>.mgl": mount the disks + stage the engine but
    # load NO ini.  The OS parks at "Select an instance from the OSD" and the
    # user picks the game in-core via "Load Instance" (all wads live in the
    # shared boot.vhd; the engine stays staged across the ini_reset).  Any
    # validated instance is used only to resolve the ELF name.
    default_ini=""
    for cand in "$INIS_DIR/${GAME,,}.ini" "${INIS[@]}"; do
        [[ -f "$cand" && "$(basename "$cand")" != ._* ]] && { default_ini="$cand"; break; }
    done
    [[ -n "$default_ini" ]] || fail "no usable ini in $INIS_DIR"
    base="$(basename "$default_ini")"
    triple="$(validate_ini "$default_ini" "$GAME" "${base%.ini}")" || fail "$base failed validation"
    elf="$(printf '%s' "$triple" | cut -f3)"
    {
        printf '<mistergamedescription>\n'
        printf '\t<rbf>%s</rbf>\n' "$RBF"
        printf '\t<file delay="1" type="s" index="0" path="%s/boot.vhd"/>\n' "$GAME"
        printf '\t<file delay="2" type="s" index="1" path="%s/%s.vhd"/>\n' "$SAVES_DIR" "$GAME"
        printf '\t<file delay="3" type="f" index="2" path="%s/%s"/>\n' "$GAME" "$elf"
    } > "$OUT_DIR/$GAME.mgl"
    ok "$GAME.mgl (bootstrap: mount + stage $elf, NO ini — OS parks at Select Instance)"
    COUNT=1
else
    for ini in "${INIS[@]}"; do
        base="$(basename "$ini")"            # lowercase on-card ini (plutonia.ini)
        [[ "$base" == ._* ]] && continue
        triple="$(validate_ini "$ini" "$GAME" "${base%.ini}")" || fail "$base failed validation"
        inst="$(printf '%s' "$triple" | cut -f2)"   # Capitalized instance (Plutonia)
        elf="$(printf '%s'  "$triple" | cut -f3)"   # loose engine ELF name (doom.elf)
        emit_mgl "$inst" "$elf" "$base"
        COUNT=$(( COUNT + 1 ))
    done
fi

echo "mkmgl: $COUNT launcher(s) → $OUT_DIR"
[[ $COUNT -gt 0 ]] || fail "no launchers emitted"
