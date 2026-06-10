#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

#
# openfpgaOS SDK — Debug (UART)
#
# Pushes a binary to the Pocket over UART, JTAG-resets the core via
# quartus_pgm, and streams console output until Ctrl+C. Always pushes
# os.bin to slot 1 alongside the user's payload so the boot ROM
# doesn't depend on the SD-card data slots — the PHDP override path
# (slot 1 stream over UART) is more reliable.
#
# Usage:
#   ./scripts/debug.sh path/to/app.elf       Push to slot 2 (Application)
#   ./scripts/debug.sh path/to/os.bin        Push to slot 1 (OS)
#   ./scripts/debug.sh --slot N path/to/file Push to explicit slot
#   ./scripts/debug.sh -v <file>             Start phdpd with -v (verbose)
#   ./scripts/debug.sh -t <file>             Start phdpd with -t (full hex trace)
#   ./scripts/debug.sh --listen              Start phdpd in listen-only mode
#                                            (no slot push, no JTAG reset —
#                                            just attach to whatever is
#                                            currently running on the core)
#
# From the top-level Makefile, pass via DEBUG=:
#   make debug APP=foo            (silent phdpd)
#   make debug APP=foo DEBUG=v    (verbose)
#   make debug APP=foo DEBUG=t    (trace)
#   make debug                    (listen-only — attach to running core)
#

set -e

SDK_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PHDPD="$SDK_DIR/src/tools/phdp/phdpd"
PHDP="$SDK_DIR/src/tools/phdp/phdp"

# ── Build host tools (always — pick up local edits) ───────────────
# `make` is incremental so this is cheap when nothing changed.
echo -e "\033[92mBuilding phdp tools...\033[0m"
if ! make -C "$SDK_DIR/src/tools/phdp" >/dev/null; then
    echo -e "\033[91mFailed to build phdp tools\033[0m"
    exit 1
fi

GREEN='\033[92m'
RED='\033[91m'
RESET='\033[0m'

# ── Parse arguments ────────────────────────────────────────────────
SLOT=""
FILE=""
PHDPD_FLAGS=""
LISTEN=0

# Strip a leading -v / -vv / -t (verbosity flag for phdpd) before
# the rest of the argument parsing. The flag is forwarded to phdpd
# as-is when it starts a fresh instance below; if no flag is given
# phdpd runs silent (only error / state-transition lines).
case "$1" in
    -v|-vv|-t)
        PHDPD_FLAGS="$1"
        shift
        ;;
esac

if [ "$1" = "--listen" ]; then
    LISTEN=1
elif [ "$1" = "--slot" ]; then
    SLOT="$2"
    FILE="$3"
else
    FILE="$1"
fi

if [ "$LISTEN" -eq 0 ]; then
    if [ -z "$FILE" ]; then
        echo -e "${RED}Usage: $0 [--slot N] <file>${RESET}"
        echo -e "       $0 --listen"
        exit 1
    fi

    if [ ! -f "$FILE" ]; then
        echo -e "${RED}File not found: $FILE${RESET}"
        exit 1
    fi
fi

# ── Auto-detect slot from filename if not specified ────────────────
if [ "$LISTEN" -eq 0 ] && [ -z "$SLOT" ]; then
    case "$(basename "$FILE")" in
        os.bin)  SLOT=1 ;;
        *)       SLOT=2 ;;
    esac
fi

# ── Ensure phdpd is running (and is the freshly built binary) ─────
# Kill any stale instance first — we just rebuilt, so a phdpd that
# was already running is by definition out of date. Otherwise the
# pgrep-skip-startup logic below would silently use the old binary
# and local edits to phdpd.c wouldn't take effect.
if pgrep -x phdpd >/dev/null 2>&1; then
    echo -e "${GREEN}Killing stale phdpd...${RESET}"
    killall phdpd >/dev/null 2>&1 || true
    # Give it a beat to release /tmp/phdp.sock so the new instance
    # can claim the same socket without an EADDRINUSE.
    for i in $(seq 1 10); do
        pgrep -x phdpd >/dev/null 2>&1 || break
        sleep 0.1
    done
fi

if [ -n "$PHDPD_FLAGS" ]; then
    echo -e "${GREEN}Starting phdpd $PHDPD_FLAGS...${RESET}"
else
    echo -e "${GREEN}Starting phdpd (silent — pass DEBUG=v for verbose)...${RESET}"
fi
"$PHDPD" $PHDPD_FLAGS &
PHDPD_PID=$!
echo -e "${GREEN}phdpd pid=$PHDPD_PID${RESET}"
for i in $(seq 1 20); do
    [ -S /tmp/phdp.sock ] && break
    if ! kill -0 "$PHDPD_PID" 2>/dev/null; then
        echo -e "${RED}phdpd exited before opening the IPC socket${RESET}"
        exit 1
    fi
    sleep 0.1
done
if ! pgrep -x phdpd >/dev/null 2>&1; then
    echo -e "${RED}Failed to start phdpd${RESET}"
    exit 1
fi

# ── Clear pending slots ───────────────────────────────────────────
# Always clear: both the push path (to start fresh) and the listen
# path (so the boot ROM falls through to the SD-card data slots
# instead of any stale override from a previous session).
"$PHDP" clear

if [ "$LISTEN" -eq 0 ]; then
    # ── Always push the OS to slot 1 (unless the user is pushing it) ──
    # The boot ROM does PHDP discovery first and then falls back to
    # SD-card slot 1 if no override is queued. The SD-card path depends
    # on the Pocket having a valid os.bin in data slot 1 (environmental
    # state); always pushing runtime/os.bin via PHDP override eliminates
    # that dependency. Skipped if the user is already pushing os.bin
    # themselves (slot 1).
    OS_BIN="$SDK_DIR/runtime/os.bin"
    if [ "$SLOT" != "1" ] && [ -f "$OS_BIN" ]; then
        "$PHDP" push --slot 1 "$OS_BIN"
    fi

    # ── Push file to slot ─────────────────────────────────────────────
    "$PHDP" push --slot "$SLOT" "$FILE"

    # ── Reset core via JTAG (quartus_pgm reload) ──────────────────────
    SOF="$SDK_DIR/runtime/ap_core.sof"
    if [ ! -f "$SOF" ]; then
        echo -e "${RED}Missing JTAG bitstream: $SOF${RESET}"
        echo "Run 'make sdk DEST=...' from the openfpgaOS source tree to populate runtime/."
        exit 1
    fi
    if ! command -v quartus_pgm >/dev/null 2>&1; then
        echo -e "${RED}quartus_pgm not on PATH${RESET}"
        exit 1
    fi
    echo -e "${GREEN}Resetting core via JTAG (quartus_pgm)...${RESET}"
    quartus_pgm -m jtag -o "p;$SOF"

    # ── Wait for OS boot ─────────────────────────────────────────────
    "$PHDP" wait
fi

# Block until the user kills the script (Ctrl+C). phdpd keeps running
# in the background and its stdout keeps flowing to the terminal.
if [ "$LISTEN" -eq 1 ]; then
    echo -e "${GREEN}Listen mode — attach to running core, Ctrl+C to exit${RESET}"
else
    echo -e "${GREEN}Streaming console — Ctrl+C to exit${RESET}"
fi
wait
