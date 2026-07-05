#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

#
# openfpgaOS SDK — Deploy to a MiSTer over the network.
#
#   copy.sh game <Game> <GameElf> <ELF> [MISTER_IP]
#                                          per-game engine update: scp the
#                                          built ELF to the LOOSE
#                                          games/OpenfpgaOS/<Game>/<GameElf>
#                                          (F-loaded at boot) — boot.vhd and
#                                          the saves volume are untouched
#   copy.sh core [MISTER_IP]               core-only bring-up: push the
#                                          runtime/mister/ artifacts (os.bin
#                                          + rbf, synced with `make sdk`)
#
# MISTER_IP also honors the environment (default mister.local).
#
# Platform: MiSTer (DE10-Nano / SuperStation One)
#

set -e

APP="$1"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SDK_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
RUNTIME="$SDK_ROOT/runtime/mister"

CORE_DIR=/media/fat/_Computer
GAMES_DIR=/media/fat/games/openfpgaOS

GREEN='\033[92m'
RED='\033[91m'
RESET='\033[0m'
ok()   { echo -e "  ${GREEN}+${RESET} $1"; }
fail() { echo -e "  ${RED}x${RESET} $1"; exit 1; }

[[ -z "$APP" ]] && { echo "Usage: $0 core [mister_ip]  |  $0 game <Game> <GameElf> <elf_path> [mister_ip]"; exit 1; }

# ── game mode: update the LOOSE per-game engine ELF (per-game model) ──
# The per-game / update-safe layout F-loads a LOOSE
# games/OpenfpgaOS/<Game>/<GameElf> (e.g. doom.elf) at mgl index 2 — it is
# NOT inside boot.vhd.  So an engine update is a single scp of that one
# file: boot.vhd (which holds the user's injected wads) and the saves
# volume are never touched, and no loop-mount / Main-stop is needed (the
# loose file isn't a mounted block device).  Written atomically (.new + mv)
# so a half-copied ELF is never F-loaded.  Reload the core to run it.
if [[ "$APP" == "game" ]]; then
    GAME="$2"; GELF="$3"; SRC="$4"
    MISTER_IP="${5:-${MISTER_IP:-mister.local}}"
    [[ -n "$GAME" && -n "$GELF" ]] || fail "usage: $0 game <Game> <GameElf> <elf_path> [mister_ip]"
    [[ -f "$SRC" ]] || fail "ELF not found: $SRC"
    GDIR="/media/fat/games/OpenfpgaOS/$GAME"
    DEST="$GDIR/$GELF"
    LOCAL_MD5=$(md5sum "$SRC" | cut -d' ' -f1)

    echo "Updating $GAME engine on root@$MISTER_IP → $DEST"
    ssh "root@$MISTER_IP" "mkdir -p '$GDIR'" || fail "ssh failed (host reachable?)"
    scp "$SRC" "root@$MISTER_IP:$DEST.new" || fail "scp failed"
    GOT=$(ssh "root@$MISTER_IP" "mv -f '$DEST.new' '$DEST' && sync && md5sum '$DEST' | cut -d' ' -f1") \
        || fail "remote install failed"
    [[ "$GOT" == "$LOCAL_MD5" ]] || fail "verify mismatch: $GOT != $LOCAL_MD5"
    ok "$GAME/$GELF updated (md5 $GOT)"
    echo "Done. Reload the core (relaunch a $GAME .mgl) to run the new engine."
    exit 0
fi

# ── core mode: push the game-agnostic runtime artifacts (dev bring-up) ─
# Installs the core the same way the release zip does: openfpgaOS.rbf ->
# _Computer/, boot.rom (os.bin) -> games/openfpgaOS/.  Sync the artifacts
# first with `make sdk DEST=...` from the openfpgaOS repo.
[[ "$APP" == "core" ]] || fail "unknown mode '$APP' — use: core [ip]  |  game <Game> <GameElf> <elf> [ip]"
MISTER_IP="${2:-${MISTER_IP:-mister.local}}"

PUSH_BOOT=""
PUSH_RBF=""
[[ -f "$RUNTIME/os.bin" ]]         && PUSH_BOOT="$RUNTIME/os.bin"
[[ -f "$RUNTIME/openfpgaOS.rbf" ]] && PUSH_RBF="$RUNTIME/openfpgaOS.rbf"
[[ -z "$PUSH_BOOT$PUSH_RBF" ]] && \
    fail "nothing to push — sync core artifacts first (openfpgaOS: make sdk DEST=...)"

echo "Deploying core to root@$MISTER_IP"
ssh "root@$MISTER_IP" "mkdir -p $CORE_DIR $GAMES_DIR"

if [[ -n "$PUSH_RBF" ]]; then
    scp "$PUSH_RBF" "root@$MISTER_IP:$CORE_DIR/OpenfpgaOS.rbf"
    ok "OpenfpgaOS.rbf → $CORE_DIR"
fi
if [[ -n "$PUSH_BOOT" ]]; then
    scp "$PUSH_BOOT" "root@$MISTER_IP:$GAMES_DIR/boot.rom"
    ok "boot.rom → $GAMES_DIR"
fi
echo "Done. Load the core from the MiSTer menu."
