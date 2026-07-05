#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

#
# openfpgaOS SDK — Build the PER-GAME image pair for the MiSTer locked
# distribution model: a READ-ONLY boot.vhd (S0) + a writable saves shell (S1).
#
# The core (openfpgaOS.rbf + os.bin) is GAME-AGNOSTIC.  Each game ships two
# FAT32 images.  The firmware's multi-vhd model (targets/mister/file.c) mounts
# the shared read tree on S0 and pins every nonvolatile write (config + saves)
# to the per-instance tree on S1 — so an update can replace boot.vhd without
# ever touching the player's saves, which live on a separate volume:
#
#   S0  boot.vhd                    READ-ONLY shell (update-replaceable):
#     /<Game>/common/               shared: the ELF (app.elf + its ELF= name)
#                                   UNLESS --no-elf, bank.ofsf, and — with
#                                   --with-wads — every *.wad/*.deh/*.pk3 (ONE
#                                   copy for all instances).  NO nonvolatile
#                                   slots.  With --no-elf the engine is instead
#                                   shipped as a loose file the .mgl F-loads at
#                                   ioctl index 2 (RTL → ELF staging → slot 3),
#                                   so boot.vhd carries only bank.ofsf + wads and
#                                   an engine update never touches boot.vhd.
#
#   S1  <Game>.saves.vhd            WRITABLE saves shell (seeded once on-device,
#                                   never overwritten by an update):
#     /<Game>/<Instance>/           per-instance, FLAT: <inst>.cfg, shared.cfg,
#                                   <family>.cfg, duke3d.cfg, and slot_0.sav ..
#                                   slot_9.sav — all preallocated CONTIGUOUS
#                                   256 KB (f_expand) for the power-cut-safe
#                                   save contract (the firmware refuses writes
#                                   to files that are not full size).  One
#                                   <Instance>/ subfolder per instance ini.
#
# Both images omit the legacy root-level /saves + /config slots
# (mkimage --no-default-nv): S0 is read-only, and S1 carries only the
# per-instance layout the firmware writes to in instance mode.
#
# One <Instance>/ folder is created per validated instance .ini in
# <inis_dir>; the [os] INSTANCE= value names the folder, GAME= must match
# <Game>, and the -config/-extraconfig names in ARGS become that instance's
# writable cfg slots.
#
# Two build profiles (affect boot.vhd only; the saves shell never holds wads):
#   (default)     a SHELL — no wads.  This is what ships: small, legal to
#                 redistribute, the user drops IWADs into games/OpenfpgaOS/
#                 <Game>/wads/ and setup.sh injects them into /<Game>/common.
#   --with-wads   bake every *.wad/*.deh/*.pk3 from <common_assets_dir> into
#                 /<Game>/common — a self-contained dev/test image.
#
# Usage:
#   mkgame.sh [--with-wads] [--no-elf] [--saves-out <saves.vhd>] \
#             <Game> <inis_dir> <common_assets_dir> <boot.vhd> [size_mb]
#
#   --no-elf           omit the engine ELF from boot.vhd (it ships as a loose
#                      F-loaded file at ioctl index 2 instead — see S0 above).
#
#   Game               game name (folder + [os] GAME= to enforce), e.g. Doom
#   inis_dir           dir of per-instance *.ini (each with [os] GAME/INSTANCE/ELF)
#   common_assets_dir  dir holding the ELF (<elf>.elf / app.elf) + bank.ofsf
#                      (+ the *.wad/*.deh/*.pk3 library when --with-wads)
#   boot.vhd           output path for the read-only S0 boot shell
#   size_mb            optional; sizes boot.vhd only (payload + 20% slack,
#                      16 MB steps, min 48 MB — mkimage's FAT32 floor; 4095 cap)
#   --saves-out <p>    output path for the S1 saves shell; default is
#                      <dir of boot.vhd>/<Game>.saves.vhd.  Always emitted.
#
# Platform: MiSTer (DE10-Nano / SuperStation One)
#

set -e
export LC_ALL=C          # deterministic glob/sort order → reproducible images
shopt -s nullglob

GREEN='\033[92m'
YELLOW='\033[93m'
RED='\033[91m'
RESET='\033[0m'
ok()   { echo -e "  ${GREEN}+${RESET} $1"; }
warn() { echo -e "  ${YELLOW}!${RESET} $1"; }
fail() { echo -e "  ${RED}x${RESET} $1"; exit 1; }

NAME_MAX=23           # firmware registry cap (FILE_SLOT_NAME_MAX - 1)
NV_SAVE_SLOTS=10      # slot_0..slot_9.sav (must match mkimage.c NV_SAVE_SLOTS)

# ── Options ──────────────────────────────────────────────────────────
WITH_WADS=0
NO_ELF=0
SAVES_OUT=""
while [[ "$1" == --* ]]; do
    case "$1" in
        --with-wads) WITH_WADS=1; shift ;;
        --no-elf)    NO_ELF=1; shift ;;
        --saves-out) SAVES_OUT="$2"; shift 2 ;;
        *)           fail "unknown option: $1" ;;
    esac
done

GAME="$1"
INIS_DIR="$2"
COMMON_DIR="$3"
IMAGE="$4"
SIZE_MB="${5:-}"

[[ -z "$GAME" || -z "$INIS_DIR" || -z "$COMMON_DIR" || -z "$IMAGE" ]] && {
    echo "Usage: $0 [--with-wads] [--no-elf] [--saves-out <saves.vhd>] <Game> <inis_dir> <common_assets_dir> <boot.vhd> [size_mb]"
    exit 1
}

# The S1 saves shell is always emitted; default it alongside boot.vhd.
[[ -z "$SAVES_OUT" ]] && SAVES_OUT="$(dirname "$IMAGE")/$GAME.saves.vhd"
[[ -d "$INIS_DIR" ]]   || fail "inis dir not found: $INIS_DIR"
[[ -d "$COMMON_DIR" ]] || fail "common assets dir not found: $COMMON_DIR"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MKIMAGE="$SCRIPT_DIR/.mkimage"
# shellcheck source=validate-ini.sh
source "$SCRIPT_DIR/validate-ini.sh"

# ── Build the image tool on demand (same as the sibling builders) ────
if [[ ! -x "$MKIMAGE" || "$SCRIPT_DIR/mkimage.c" -nt "$MKIMAGE" ]]; then
    cc -O2 -I"$SCRIPT_DIR" -o "$MKIMAGE" \
        "$SCRIPT_DIR/mkimage.c" \
        "$SCRIPT_DIR/fatfs/ff.c" \
        "$SCRIPT_DIR/fatfs/ffunicode.c" || fail "mkimage build failed"
    ok "built mkimage tool"
fi

name_warn() {   # $1 = in-image basename
    (( ${#1} > NAME_MAX )) && warn "name >${NAME_MAX} chars — firmware registry cannot map it: $1 (${#1})"
    return 0
}

# ── Pass 1: validate every instance ini, collect INSTANCE + cfg names ─
INIS=("$INIS_DIR"/*.ini)
[[ ${#INIS[@]} -gt 0 ]] || fail "no *.ini in $INIS_DIR"

ELF_NAME=""              # ELF= value (consistent across the game), e.g. doom.elf
INSTANCES=()             # parallel arrays, one entry per instance
declare -a INST_CFGS     # ';'-joined cfg name list per instance
declare -A INST_SEEN=()  # instance-name collision guard (FAT case-insensitive)
NV_TOTAL=0               # count of nv slots (for sizing)

for ini in "${INIS[@]}"; do
    base="$(basename "$ini")"
    [[ "$base" == ._* ]] && continue           # AppleDouble junk
    stem="${base%.ini}"

    # Two-root contract: GAME must match, INSTANCE + ELF must be present.
    triple="$(validate_ini "$ini" "$GAME" "$stem")" || fail "$base failed validation"
    inst="$(printf '%s' "$triple" | cut -f2)"
    elf="$(printf '%s'  "$triple" | cut -f3)"

    if [[ -z "$ELF_NAME" ]]; then
        ELF_NAME="$elf"
    elif [[ "${elf,,}" != "${ELF_NAME,,}" ]]; then
        warn "$base: ELF=\"$elf\" differs from \"$ELF_NAME\" (first ini wins for the common ELF)"
    fi

    lc="${inst,,}"
    [[ -n "${INST_SEEN[$lc]:-}" ]] && fail "duplicate INSTANCE \"$inst\" (from $base and ${INST_SEEN[$lc]})"
    INST_SEEN["$lc"]="$base"
    name_warn "$inst"

    # cfg names for this instance: the -config/-extraconfig names in ARGS,
    # plus the always-present shared.cfg / <family>.cfg / duke3d.cfg trio.
    family="${ELF_NAME%.*}"; family="${family,,}"
    cfgs=("shared.cfg" "$family.cfg" "duke3d.cfg")
    while read -r c; do
        [[ -n "$c" ]] || continue
        c="$(basename "$c")"
        skip=0
        for e in "${cfgs[@]}"; do [[ "${e,,}" == "${c,,}" ]] && { skip=1; break; }; done
        (( skip )) || { name_warn "$c"; cfgs+=("$c"); }
    done < <(grep -oiE -- '-(config|extraconfig)[[:space:]]*"[^"]*"' "$ini" \
             | sed 's/.*"\(.*\)"/\1/')

    INSTANCES+=("$inst")
    joined="$(printf '%s;' "${cfgs[@]}")"
    INST_CFGS+=("$joined")
    NV_TOTAL=$(( NV_TOTAL + ${#cfgs[@]} + NV_SAVE_SLOTS ))
done

[[ ${#INSTANCES[@]} -gt 0 ]] || fail "no valid instance inis in $INIS_DIR"

# ── Resolve the common ELF source (<elf>.elf preferred, then app.elf) ─
# Skipped with --no-elf: in the F-loaded-ELF model the engine ships as a loose
# file the .mgl F-loads at ioctl index 2 (RTL → ELF staging → slot 3), so
# boot.vhd carries NO ELF and mkgame needs no ELF source at all.
ELF_SRC=""
if [[ "$NO_ELF" != 1 ]]; then
    for cand in "$COMMON_DIR/$ELF_NAME" "$COMMON_DIR/app.elf"; do
        [[ -f "$cand" ]] && { ELF_SRC="$cand"; break; }
    done
    if [[ -z "$ELF_SRC" ]]; then
        elfs=("$COMMON_DIR"/*.elf)
        [[ ${#elfs[@]} -eq 1 ]] && ELF_SRC="${elfs[0]}"
    fi
    [[ -n "$ELF_SRC" && -f "$ELF_SRC" ]] || fail "ELF not found in $COMMON_DIR (looked for $ELF_NAME / app.elf / a single *.elf)"
fi

# ── Assemble common/ specs (→ S0 boot.vhd) ───────────────────────────
COMMON_SPECS=()
COMMON_BYTES=0
add_common() {   # $1 host-src  $2 in-image basename
    COMMON_SPECS+=("$1=/$GAME/common/$2")
    COMMON_BYTES=$(( COMMON_BYTES + $(stat -c %s "$1") ))
}

# app.elf (fixed slot 3, READ under common_root) + the ELF= name so os.ini's
# ELF=<name> resolves by name on the common tree.  Skipped with --no-elf: the
# engine is F-loaded loose (index 2 → ELF staging) and served as slot 3 from
# there, so boot.vhd holds only bank.ofsf + wads.
if [[ "$NO_ELF" != 1 ]]; then
    add_common "$ELF_SRC" "app.elf"
    [[ "${ELF_NAME,,}" != "app.elf" ]] && add_common "$ELF_SRC" "$ELF_NAME"
fi

# Soundfont(s): every *.ofsf in the common dir → /<Game>/common/<name>.
OFSF=("$COMMON_DIR"/*.ofsf)
if [[ ${#OFSF[@]} -gt 0 ]]; then
    for s in "${OFSF[@]}"; do add_common "$s" "$(basename "$s")"; done
else
    warn "no *.ofsf in $COMMON_DIR — MIDI music will be silent until bank.ofsf is provided"
fi

# Wads/dehs/pk3s: only baked into a --with-wads dev/test image; the shipped
# shell leaves /<Game>/common empty of wads for setup.sh to fill on-device.
WAD_COUNT=0
declare -A WAD_SEEN=()
if [[ "$WITH_WADS" == 1 ]]; then
    for f in "$COMMON_DIR"/*; do
        [[ -f "$f" ]] || continue
        b="$(basename "$f")"; [[ "$b" == ._* ]] && continue
        case "${b,,}" in
            *.wad|*.deh|*.pk3) : ;;
            *) continue ;;
        esac
        lc="${b,,}"
        [[ -n "${WAD_SEEN[$lc]:-}" ]] && fail "case-insensitive wad collision: $b vs ${WAD_SEEN[$lc]}"
        WAD_SEEN["$lc"]="$b"
        name_warn "$b"
        add_common "$f" "$b"
        WAD_COUNT=$(( WAD_COUNT + 1 ))
    done
    (( WAD_COUNT > 0 )) || warn "--with-wads but no *.wad/*.deh/*.pk3 in $COMMON_DIR"
fi

# ── Assemble per-instance nv specs (→ S1 saves shell) ────────────────
NV_SPECS=()
for idx in "${!INSTANCES[@]}"; do
    inst="${INSTANCES[$idx]}"
    IFS=';' read -ra cfgs <<< "${INST_CFGS[$idx]}"
    for c in "${cfgs[@]}"; do
        [[ -n "$c" ]] && NV_SPECS+=("nv=/$GAME/$inst/$c")
    done
    for n in $(seq 0 $((NV_SAVE_SLOTS-1))); do
        NV_SPECS+=("nv=/$GAME/$inst/slot_$n.sav")
    done
done

# ── Size each image: payload + 20% slack, 16 MB steps, min 48 MB ─────
size_mb_for() {   # $1 payload bytes → MB (payload+20%, 16 MB steps, min 48)
    local mb; mb=$(( ($1 * 120 / 100 + 16*1024*1024 - 1) / (16*1024*1024) * 16 ))
    (( mb < 48 )) && mb=48
    echo "$mb"
}

NV_BYTES=$(( NV_TOTAL * 256 * 1024 ))

# boot.vhd size: explicit size_mb overrides (it's the wad-carrying image);
# otherwise auto from the common payload.
if [[ -z "$SIZE_MB" ]]; then
    BOOT_MB="$(size_mb_for "$COMMON_BYTES")"
else
    BOOT_MB="$SIZE_MB"
    need_mb=$(( COMMON_BYTES / (1024*1024) + 1 ))
    (( BOOT_MB < need_mb )) && warn "size_mb=$BOOT_MB < common payload ~${need_mb} MB — mkimage will fail if the volume fills"
fi
SAVES_MB="$(size_mb_for "$NV_BYTES")"
(( BOOT_MB  > 4095 )) && fail "boot payload needs ${BOOT_MB} MB > 4095 MB FAT32 cap — build without --with-wads (shell) or split the game"
(( SAVES_MB > 4095 )) && fail "saves payload needs ${SAVES_MB} MB > 4095 MB FAT32 cap — too many instances for one saves shell"

# ── Report + build both images ───────────────────────────────────────
mode="shell (no wads)"; (( WITH_WADS )) && mode="with wads ($WAD_COUNT)"
elfmode="baked $ELF_NAME"; (( NO_ELF )) && elfmode="loose $ELF_NAME (F-load index 2)"
echo "mkgame: $GAME — ${#INSTANCES[@]} instance(s), $NV_TOTAL nv slots, ELF=$elfmode, $mode"
if [[ -n "${MKGAME_DEBUG:-}" ]]; then
    printf '  common: %s\n' "${COMMON_SPECS[@]}"
    printf '  nv:     %s\n' "${NV_SPECS[@]}"
fi

# S0 boot shell — /<Game>/common only, read-only, no nonvolatile slots.
mkdir -p "$(dirname "$IMAGE")"
"$MKIMAGE" --no-default-nv "$IMAGE" "$BOOT_MB" "${COMMON_SPECS[@]}"
ok "boot shell (S0): $IMAGE (${BOOT_MB} MB, common only, $mode)"

# S1 saves shell — per-instance /<Game>/<Instance>/{cfg,slot_N.sav} only.
mkdir -p "$(dirname "$SAVES_OUT")"
"$MKIMAGE" --no-default-nv "$SAVES_OUT" "$SAVES_MB" "${NV_SPECS[@]}"
ok "saves shell (S1): $SAVES_OUT (${SAVES_MB} MB, ${#INSTANCES[@]} instances, $NV_TOTAL slots)"
