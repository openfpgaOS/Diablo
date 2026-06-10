#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

#
# openfpgaOS SDK — Scaffold a new app
#
# Two scaffold flavors:
#
#   Custom core (default)
#     ./scripts/new.sh mygame
#     → src/mygame/ with its own standalone Makefile that drives a
#       full custom openFPGA core (own dist/, own Core ID, own ZIP).
#       Called by scripts/customize.sh (make core).
#
#   SDK app
#     ./scripts/new.sh --sdk-app mydemo
#     → src/apps/mydemo/ with a thin Makefile that includes ../app.mk
#       and gets bundled into the shared SDK demo core.
#       Called by `cd src/apps && make new APP=mydemo`.
#
# Both flavors take an optional --target (default: pocket).
#

set -e

SDK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PLATFORMS_DIR="$SDK_ROOT/src/sdk/platforms"

GREEN='\033[92m'
RED='\033[91m'
CYAN='\033[96m'
RESET='\033[0m'

ok()   { echo -e "  ${GREEN}+${RESET} $1"; }
fail() { echo -e "  ${RED}x${RESET} $1"; exit 1; }

# ── Parse args ───────────────────────────────────────────────────────
APP=""
TARGET="pocket"
KIND="custom-core"   # or "sdk-app"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sdk-app)   KIND="sdk-app"; shift ;;
        --target|-t) TARGET="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 [--sdk-app] <app_name> [--target pocket|mister]"
            echo ""
            echo "  Default (custom core)"
            echo "    Creates src/<app_name>/ with a standalone Makefile that"
            echo "    drives a full openFPGA custom core."
            echo ""
            echo "  --sdk-app"
            echo "    Creates src/apps/<app_name>/ with a thin Makefile that"
            echo "    plugs into the SDK demo core."
            exit 0 ;;
        -*) echo "Unknown option: $1"; exit 1 ;;
        *)  APP="$1"; shift ;;
    esac
done

[[ -z "$APP" ]] && { echo "Usage: $0 [--sdk-app] <app_name> [--target pocket|mister]"; exit 1; }

# Sanitize: lowercase, no spaces
APP_LOWER=$(echo "$APP" | tr '[:upper:]' '[:lower:]' | sed 's/[^a-z0-9_]//g')
[[ -z "$APP_LOWER" ]] && fail "Invalid app name: $APP"

if [[ "$KIND" == "sdk-app" ]]; then
    APP_DIR="$SDK_ROOT/src/apps/$APP_LOWER"
else
    APP_DIR="$SDK_ROOT/src/$APP_LOWER"
fi

# ── Validate target platform ────────────────────────────────────────
if [[ ! -d "$PLATFORMS_DIR/$TARGET" ]]; then
    fail "Unknown target: $TARGET"
    echo "  Available targets:"
    for d in "$PLATFORMS_DIR"/*/; do
        echo "    $(basename "$d")"
    done
    exit 1
fi

TEMPLATES="$PLATFORMS_DIR/$TARGET/templates"

# ── Check if app already exists ──────────────────────────────────────
if [[ -d "$APP_DIR" ]]; then
    if [[ "$KIND" == "sdk-app" ]]; then
        fail "Directory already exists: src/apps/$APP_LOWER/"
    else
        fail "Directory already exists: src/$APP_LOWER/"
    fi
fi

if [[ "$KIND" == "sdk-app" ]]; then
    echo -e "${CYAN}Creating SDK app: $APP_LOWER${RESET} (target: $TARGET)"
else
    echo -e "${CYAN}Creating custom core: $APP_LOWER${RESET} (target: $TARGET)"
fi
echo

# ── Create app directory ─────────────────────────────────────────────
mkdir -p "$APP_DIR"

# ── Generate Makefile ────────────────────────────────────────────────
if [[ "$KIND" == "sdk-app" ]]; then
    # SDK app: thin wrapper that includes ../app.mk. The shared
    # src/apps/Makefile already handles building, packaging, and
    # release assembly for every SDK app at once.
    cat > "$APP_DIR/Makefile" << 'MKEOF'
# {{APP_DISPLAY}} — openfpgaOS SDK app
#
# This is an SDK app, bundled into the shared SDK demo core. The
# parent src/apps/Makefile builds and packages every SDK app together;
# build/sdk/ is the resulting deployable.
#
# Per-app overrides go above the include line, e.g.:
#   SRCS = $(wildcard *.c) $(SDK_DIR)/of_midi.c

SDK_DIR = ../../sdk
SRCS    = $(wildcard *.c)

include ../app.mk
MKEOF
else
    # Custom core: full standalone Makefile that drives its own
    # dist/<app>/, its own build/<app>/, and its own ZIP.
    cat > "$APP_DIR/Makefile" << 'MKEOF'
# {{APP_DISPLAY}} — openfpgaOS custom core
#
# This Makefile builds a CUSTOM CORE: a standalone openFPGA core
# wrapping a single app. It owns its own dist/, build/, and ZIP.
#
# Targets:
#   make              Build the custom core
#   make debug        Build, push via UART, stream console
#   make copy         Copy this custom core to Pocket SD
#   make package      Package this custom core into a ZIP
#   make test         Test on desktop (SDL2)
#   make clean        Remove build artifacts
#

APP       = {{APP}}
TARGET    = {{TARGET}}
SDK_DIR   = ../sdk
ROOT      = $(realpath $(CURDIR)/../..)
OBJ_DIR   = $(ROOT)/.obj/$(APP)
BUILD_DIR = $(OBJ_DIR)
DIST      = $(ROOT)/dist/$(APP)
OUT       = $(ROOT)/build/$(APP)
RUNTIME   = $(ROOT)/runtime

SRCS     = $(wildcard *.c)
SRCS_CXX = $(wildcard *.cpp)

# Declare default target before sdk.mk's rules
.DEFAULT_GOAL := all

include $(SDK_DIR)/sdk.mk

$(CRT_DIR)/start.o: $(CRT_DIR)/start.S
	$(AS) $(ASFLAGS) -c -o $@ $<

# Build ELF, then assemble SD card image in build/<app>/
all: $(OBJ_DIR)/app.elf release
	@$(SIZE) $<

release: $(OBJ_DIR)/app.elf
	@rm -rf $(OUT)
	@mkdir -p $(dir $(OUT))
	@cp -r $(DIST) $(OUT)
	@cp $(RUNTIME)/bitstream.rbf_r $(RUNTIME)/loader.bin $$(ls -d $(OUT)/Cores/*/)/
	@mkdir -p $$(ls -d $(OUT)/Assets/*/)/common
	@cp $(RUNTIME)/os.bin $$(ls -d $(OUT)/Assets/*/)/common/
	@cp $< $$(ls -d $(OUT)/Assets/*/)/common/$(APP).elf
	@for f in *.mid *.mod *.wav *.dat *.png; do [ -f "$$f" ] && cp "$$f" $$(ls -d $(OUT)/Assets/*/)/common/; done 2>/dev/null; true
	@echo "Ready: build/$(APP)/"

COMMON = $(shell ls -d $(OUT)/Assets/*/common 2>/dev/null)

debug: all
	@$(ROOT)/scripts/debug.sh $$(ls -d $(OUT)/Assets/*/common)/$(APP).elf

copy: all
	@$(SDK_DIR)/platforms/$(TARGET)/copy.sh "$(APP)" "$$(ls -d $(OUT)/Assets/*/common)/$(APP).elf"

package: all
	@$(ROOT)/scripts/package.sh $(APP)

test: app_pc

clean: sdk-clean
	rm -rf $(OUT)

.PHONY: all release debug copy package test clean
MKEOF
fi

# Fill in placeholders
sed -i "s/{{APP}}/$APP_LOWER/g; s/{{APP_DISPLAY}}/$APP/g; s/{{TARGET}}/$TARGET/g" "$APP_DIR/Makefile"
ok "Makefile"

# ── Generate main.c ──────────────────────────────────────────────────
cat > "$APP_DIR/main.c" << 'CEOF'
/*
 * {{APP_DISPLAY}} — openfpgaOS app
 *
 * Build:   make
 * Copy:  make copy
 * Test: make test
 */

#include "of.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    of_video_init();

    /* Set up a gradient palette */
    for (int i = 0; i < 256; i++)
        of_video_palette(i, (i << 16) | ((255 - i) << 8) | 128);

    /* Draw something */
    uint8_t *fb = of_video_surface();
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++)
            fb[y * 320 + x] = x ^ y;

    of_video_flip();

    printf("Hello from {{APP_DISPLAY}}!\n");

    /* Main loop */
    while (1) {
        of_input_poll();

        if (of_btn_pressed(OF_BTN_A)) {
            /* A button pressed */
        }

        usleep(16000);  /* ~60 fps */
    }
}
CEOF

sed -i "s/{{APP_DISPLAY}}/$APP/g" "$APP_DIR/main.c"
ok "main.c"

# ── Summary ──────────────────────────────────────────────────────────
echo
if [[ "$KIND" == "sdk-app" ]]; then
    echo -e "${GREEN}Done!${RESET} SDK app at: src/apps/$APP_LOWER/"
    echo
    echo "  cd src/apps/$APP_LOWER"
    echo "  make              # build this SDK app"
    echo "  cd .. && make     # rebuild SDK demo core (build/sdk/)"
    echo
    echo "Edit src/apps/$APP_LOWER/main.c to start building your SDK app."
else
    echo -e "${GREEN}Done!${RESET} Custom core at: src/$APP_LOWER/"
    echo
    echo "  cd src/$APP_LOWER"
    echo "  make              # build the custom core"
    echo "  make copy         # copy this custom core to Pocket SD"
    echo "  make test         # test on desktop (SDL2)"
    echo
    echo "Edit src/$APP_LOWER/main.c to start building your custom core."
fi
