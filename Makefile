#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

# openfpgaOS SDK Makefile
#
# Two ways to ship code with this SDK:
#
#   1. CUSTOM CORE — a standalone openFPGA core wrapping a single
#      application (your game / demo / tool). Lives at src/<name>/,
#      ships its own dist/<name>/ and packages to its own ZIP. Created
#      with `make core`.
#
#   2. SDK APP — an app bundled into the shared "openfpgaOS SDK" demo
#      core alongside the other examples. Lives at src/apps/<name>/,
#      shares dist/sdk/ with every other SDK app, and ships in one ZIP.
#      Created with `cd src/apps && make new APP=<name>`.
#
# Either way the app .elf is platform-neutral: `make copy` deploys it
# to a Pocket SD card, `make copy TARGET=mister` pushes the same build
# to a MiSTer over the network (see src/sdk/platforms/mister/).
#
# Quick start:
#   make setup          Install RISC-V toolchain
#   make core           Scaffold a custom core

# ── Platform target ──────────────────────────────────────────────────
# Every build/package/copy/release verb honors TARGET.  Each target is a
# directory under src/sdk/platforms/<target>/ providing the contract
# scripts (image.sh, copy.sh, package.sh, platform.conf) — so adding a
# target = add that directory plus a runtime/<target>/ (see
# docs/adding-a-target.md).  The shared Makefile + scripts name no
# target.  Exported so the scripts and sub-makes resolve TARGET alike.
TARGET ?= pocket
export TARGET
# Available platforms, discovered from the directory layout (dirs only —
# the trailing-slash glob skips README.md and other stray files).
empty :=
space := $(empty) $(empty)
PLATFORMS := $(notdir $(patsubst %/,%,$(wildcard src/sdk/platforms/*/)))
PLATFORMS_BAR := $(subst $(space),|,$(PLATFORMS))

# ── Paths ────────────────────────────────────────────────────────────
RUNTIME = runtime

# ── Detect custom cores in src/<name>/ ───────────────────────────────
# Any src/<name>/ directory with a Makefile that isn't apps/, sdk/, or
# tools/ is a custom core (scaffolded by `make core`).  Auto-discovered,
# just like the OS repo's targets — no registry to edit.
APP_NAME := $(shell for d in src/*/; do \
	[ "$$d" = "src/apps/" ] || [ "$$d" = "src/sdk/" ] || [ "$$d" = "src/tools/" ] && continue; \
	[ -f "$$d/Makefile" ] && basename "$$d"; \
done)

# When a custom core exists, it becomes the DEFAULT subject of the bare
# verbs (build/debug/test/copy/package) — so after `make core` you just
# run `make build`, `make copy`, … and they act on your core with no
# CORE= needed.  First core wins if several exist; pick another with
# CORE=<name>, or CORE=sdk for the bundled demo core.  No custom core →
# bare verbs fall back to the SDK demo core + any cores found.
DEFAULT_CORE := $(firstword $(APP_NAME))
# Capture explicitness (command line / env) BEFORE defaulting, so `make
# clean` still full-wipes when CORE is merely the discovered default.
CORE_EXPLICIT := $(filter command line environment,$(origin CORE))
# Default CORE to the discovered custom core with ifndef + := (NOT ?=):
# `?= $(DEFAULT_CORE)` leaves the LITERAL "$(DEFAULT_CORE)" as CORE's
# unexpanded value, and `ifdef CORE` (which does not expand) then sees it
# as non-empty even with no custom core — firing the core branch as the
# broken `make -C src/`.  := collapses it to a real (possibly empty) value.
ifndef CORE
CORE := $(DEFAULT_CORE)
endif

# ── Colors (auto-detect terminal) ────────────────────────────────────
ifneq ($(shell tput colors 2>/dev/null),)
C_LOGO  := \033[96m
C_HEAD  := \033[1m
C_CMD   := \033[93m
C_VERB  := \033[1;93m
C_ARG   := \033[3;93m
C_RESET := \033[0m
else
C_LOGO  :=
C_HEAD  :=
C_CMD   :=
C_VERB  :=
C_ARG   :=
C_RESET :=
endif

# ── Display name (detected custom core or <core> placeholder) ───────
# Truncate to 10 chars with ... if too long, to keep help aligned
ifneq ($(APP_NAME),)
A := $(shell n="$(APP_NAME)"; [ $${#n} -gt 10 ] && echo "$${n:0:7}..." || echo "$$n")
else
A := <core>
endif

# ── Default target ───────────────────────────────────────────────────
all: help

# ── Help ─────────────────────────────────────────────────────────────
help:
	@printf "$(C_LOGO)"
	@echo "           ___  ___  ___ ___"
	@echo "          / _ \\/ _ \\/ -_) _ \\"
	@echo "          \\___/ .__/\\__/_//_/"
	@echo "         ____/_/  ________"
	@echo "        / __/ _ \\/ ___/ _ |"
	@echo "       / _// ___/ (_ / __ |"
	@echo "      /_/_/_/___\\___/_/ |_|"
	@echo "     / __ \\/ __/"
	@echo "    / /_/ /\\ \\"
	@printf '    \\____/___/  '
	@printf "$(C_CMD)v0.6$(C_RESET) SDK\n"
	@printf "$(C_RESET)\n"
ifneq ($(DEFAULT_CORE),)
	@printf "  $(C_HEAD)Core:$(C_RESET) $(C_CMD)$(DEFAULT_CORE)$(C_RESET) $(C_DIM)(default — bare $(C_RESET)$(C_VERB)make build/copy/package/test/debug$(C_RESET)$(C_DIM) act on it;\n"
	@printf "        override with $(C_RESET)$(C_ARG)CORE=<name>$(C_RESET)$(C_DIM), or $(C_RESET)$(C_ARG)CORE=sdk$(C_RESET)$(C_DIM) for the bundled demo core)$(C_RESET)\n"
	@printf "  $(C_HEAD)Platform:$(C_RESET) $(C_CMD)$(TARGET)$(C_RESET) $(C_DIM)(TARGET=$(PLATFORMS_BAR))$(C_RESET)\n\n"
endif
	@printf "  $(C_HEAD)Getting started:$(C_RESET)\n"
	@printf "    $(C_CMD)make $(C_VERB)setup$(C_RESET)                    Install RISC-V toolchain\n"
	@printf "    $(C_CMD)make $(C_VERB)core$(C_RESET)                     Scaffold a custom core (src/<name>/)\n"
	@echo ""
	@printf "  $(C_HEAD)Custom core (work from src/$(A)/):$(C_RESET)\n"
	@printf "    $(C_CMD)cd src/$(A)$(C_RESET)\n"
	@printf "    $(C_CMD)make$(C_RESET)                          Build the custom core\n"
	@printf "    $(C_CMD)make $(C_VERB)debug$(C_RESET)                    Build, push via UART, stream console (Pocket)\n"
	@printf "    $(C_CMD)make $(C_VERB)copy$(C_RESET)                     Copy this custom core to Pocket SD\n"
	@printf "    $(C_CMD)make $(C_VERB)copy$(C_RESET) $(C_ARG)TARGET=mister$(C_RESET)       Push this custom core to a MiSTer\n"
	@printf "    $(C_CMD)make $(C_VERB)package$(C_RESET)                  Package this custom core into a ZIP\n"
	@printf "    $(C_CMD)make $(C_VERB)test$(C_RESET)                     Test on desktop (SDL2)\n"
	@printf "    $(C_CMD)make $(C_VERB)clean$(C_RESET)                    Remove build artifacts\n"
	@echo ""
	@printf "  $(C_HEAD)SDK apps (bundled into the SDK demo core):$(C_RESET)\n"
	@printf "    $(C_CMD)cd src/apps$(C_RESET)\n"
	@printf "    $(C_CMD)make$(C_RESET)                          Build all SDK apps\n"
	@printf "    $(C_CMD)make $(C_VERB)new$(C_RESET) $(C_ARG)APP=app$(C_RESET)              Scaffold a new SDK app\n"
	@printf "    $(C_CMD)make $(C_VERB)debug$(C_RESET) $(C_ARG)APP=app$(C_RESET)            Build, push via UART, stream console\n"
	@printf "    $(C_CMD)make $(C_VERB)copy$(C_RESET)                     Copy SDK core to Pocket SD\n"
	@printf "    $(C_CMD)make $(C_VERB)package$(C_RESET)                  Package SDK core into a ZIP\n"
	@printf "    $(C_CMD)make $(C_VERB)test$(C_RESET) $(C_ARG)APP=app$(C_RESET)             Test on desktop (SDL2)\n"
	@printf "    $(C_CMD)make $(C_VERB)clean$(C_RESET)                    Remove build artifacts\n"
	@echo ""
	@printf "  $(C_HEAD)From the root (drives both paths):$(C_RESET)\n"
	@printf "    Every verb takes $(C_ARG)TARGET=$(PLATFORMS_BAR)$(C_RESET) $(C_DIM)(default $(TARGET)):$(C_RESET)\n"
	@printf "    $(C_DIM)  pocket → build/pocket/… APF tree,  copy → SD card\n"
	@printf "      mister → build/mister/… disk image, copy → network (MISTER_IP=…)\n"
	@printf "      zips → releases/<target>/.  SDK demo core is Pocket-only (MiSTer = per-app: APP=<x>).$(C_RESET)\n"
	@printf "    $(C_CMD)make $(C_VERB)build$(C_RESET)                    Build SDK core + every custom core\n"
	@printf "    $(C_CMD)make $(C_VERB)build$(C_RESET) $(C_ARG)CORE=<core|sdk>$(C_RESET)    Build core or sdk only\n"
	@printf "    $(C_CMD)make $(C_VERB)build$(C_RESET) $(C_ARG)CORE=<core>$(C_RESET)        Build the <core> custom core only\n"
	@printf "    $(C_CMD)make $(C_VERB)build$(C_RESET) $(C_ARG)APP=<app>$(C_RESET)          Build the <app> SDK app only\n"
	@printf "    $(C_CMD)make $(C_VERB)debug$(C_RESET)                    Attach to running core (phdpd only, no push)\n"
	@printf "    $(C_CMD)make $(C_VERB)debug$(C_RESET) $(C_ARG)CORE=<core>$(C_RESET)        Build + UART push + stream a custom core\n"
	@printf "    $(C_CMD)make $(C_VERB)debug$(C_RESET) $(C_ARG)APP=<app>$(C_RESET)          Build + UART push + stream a single SDK app\n"
	@printf "    $(C_CMD)make $(C_VERB)test$(C_RESET) $(C_ARG)CORE=<core>$(C_RESET)         Test a custom core on desktop (SDL2)\n"
	@printf "    $(C_CMD)make $(C_VERB)test$(C_RESET) $(C_ARG)APP=<app>$(C_RESET)           Test a single SDK app on desktop (SDL2)\n"
	@printf "    $(C_CMD)make $(C_VERB)copy$(C_RESET)                     Copy SDK demo core + custom cores\n"
	@printf "    $(C_CMD)make $(C_VERB)copy$(C_RESET) $(C_ARG)CORE=sdk$(C_RESET)            Copy SDK demo core only\n"
	@printf "    $(C_CMD)make $(C_VERB)copy$(C_RESET) $(C_ARG)CORE=<core>$(C_RESET)         Copy the <core> custom core only\n"
	@printf "    $(C_CMD)make $(C_VERB)package$(C_RESET)                  Package SDK demo core + every custom core\n"
	@printf "    $(C_CMD)make $(C_VERB)release$(C_RESET) $(C_ARG)CORE=<core>$(C_RESET)      Package + draft a GitHub release (tag <core>-v<ver>)\n"
	@printf "    $(C_CMD)make $(C_VERB)tools$(C_RESET)                    Build PHDP host tools\n"
	@printf "    $(C_CMD)make $(C_VERB)push$(C_RESET) $(C_ARG)DEST=\"path/to/sdk\"$(C_RESET)  Mirror src + os.bin into another SDK (keeps its core)\n"
	@printf "    $(C_CMD)make $(C_VERB)clean$(C_RESET)                    Remove build artifacts (keeps releases/)\n"
	@printf "    $(C_CMD)make $(C_VERB)distclean$(C_RESET)                Remove build artifacts AND packaged releases\n"

# ── Setup ────────────────────────────────────────────────────────────
setup:
	@./scripts/setup.sh

# ── Scaffold a custom core ───────────────────────────────────────────
core:
	@./scripts/customize.sh

# ── Build ────────────────────────────────────────────────────────────
# `make build`              → SDK demo core + every custom core under src/<name>/
# `make build CORE=sdk`     → SDK demo core only (src/apps/)
# `make build CORE=<name>`  → custom core src/<name>/ only
# `make build APP=<name>`   → single SDK app src/apps/<name>/ only
build:
ifdef APP
	$(MAKE) -C src/apps/$(APP)
	@# Per-app deliverable for targets that build one (mister = disk image);
	@# pocket has no per-app image.sh effect for SDK apps (they bundle into
	@# the demo core via `make build CORE=sdk`).  Generic dispatch — adding
	@# a target = add platforms/<target>/image.sh.
	@if [ -f src/sdk/platforms/$(TARGET)/image.sh ]; then \
		src/sdk/platforms/$(TARGET)/image.sh "$(APP)" ".obj/sdk/$(APP)/app.elf" "$(CURDIR)" "" \
			$(wildcard src/apps/$(APP)/*.mid src/apps/$(APP)/*.mod \
			           src/apps/$(APP)/*.wav src/apps/$(APP)/*.dat \
			           src/apps/$(APP)/*.png src/apps/$(APP)/*.bin \
			           src/apps/$(APP)/*.cfg src/apps/$(APP)/*.iso); \
	fi
else ifdef CORE
ifeq ($(CORE),sdk)
	$(MAKE) -C src/apps
else
	$(MAKE) -C src/$(CORE)
endif
else
	$(MAKE) -C src/apps
	@for d in src/*/; do \
		[ "$$d" = "src/apps/" ] || [ "$$d" = "src/sdk/" ] || [ "$$d" = "src/tools/" ] && continue; \
		[ -f "$$d/Makefile" ] || continue; \
		name=$$(basename "$$d"); \
		echo "Building custom core: $$name..."; \
		$(MAKE) -C "$$d" || exit 1; \
	done
endif

# ── Debug (UART push + console) ─────────────────────────────────────
# `make debug`              → listen-only: start phdpd and stream the console
#                             of whatever is already running on the core. No
#                             slot push, no JTAG reset.
# `make debug CORE=<name>`  → custom core src/<name>/ — pushes its release ELF
# `make debug APP=<name>`   → SDK app src/apps/<name>/ — pushes that single
#                             app's ELF over UART (for iterating on one app
#                             without rebuilding the whole SDK demo core).
# CORE=sdk is intentionally rejected: the SDK demo core is the runtime,
# not a single ELF, so there's nothing for the loader to push.
#
# The UART-push branch fires only when CORE is EXPLICITLY set (command line /
# environment), NOT when it is merely the auto-discovered default custom core
# — otherwise, in a game-port checkout where DEFAULT_CORE is non-empty, bare
# `make debug` would silently push that core's ELF instead of just attaching.
# Same CORE_EXPLICIT gate the `clean` target uses.
debug:
ifdef APP
	$(MAKE) -C src/apps debug APP=$(APP)
else ifneq ($(CORE_EXPLICIT),)
ifeq ($(CORE),sdk)
	@echo "make debug CORE=sdk is not supported — the SDK demo core is not a single ELF."
	@echo "Use 'make debug APP=<sdk-app>' to push a single SDK app over UART instead."
	@exit 1
else
	$(MAKE) -C src/$(CORE) debug
endif
else
	@./scripts/debug.sh --listen
endif

# ── Test (desktop SDL2 build) ───────────────────────────────────────
# `make test CORE=<name>`  → build the custom core's app_pc
# `make test APP=<name>`   → build a single SDK app's app_pc
test:
ifdef APP
	$(MAKE) -C src/apps test APP=$(APP)
else ifdef CORE
ifeq ($(CORE),sdk)
	@echo "make test CORE=sdk is not supported — pick a single SDK app instead."
	@echo "Use 'make test APP=<sdk-app>' to build that app for desktop."
	@exit 1
else
	$(MAKE) -C src/$(CORE) test
endif
else
ifneq ($(APP_NAME),)
	$(MAKE) -C src/$(APP_NAME) test
else
	@echo "Usage: make test CORE=<custom-core>"
	@echo "       make test APP=<sdk-app>"
	@exit 1
endif
endif

# ── Copy to SD card ──────────────────────────────────────────────────
# `make copy`              → SDK demo core + every custom core
# `make copy CORE=sdk`     → SDK demo core only
# `make copy CORE=<name>`  → custom core src/<name>/ only
copy:
ifdef CORE
ifeq ($(CORE),sdk)
	$(MAKE) -C src/apps copy
else
	$(MAKE) -C src/$(CORE) copy
endif
else
	$(MAKE) -C src/apps copy
	@for d in src/*/; do \
		[ "$$d" = "src/apps/" ] || [ "$$d" = "src/sdk/" ] || [ "$$d" = "src/tools/" ] && continue; \
		[ -f "$$d/Makefile" ] || continue; \
		$(MAKE) -C "$$d" copy || true; \
	done
endif

# ── Package distributable ZIPs ───────────────────────────────────────
# `make package`              → SDK demo core + every custom core
# `make package CORE=sdk`     → SDK demo core only
# `make package CORE=<name>`  → custom core src/<name>/ only
package:
ifdef CORE
ifeq ($(CORE),sdk)
	$(MAKE) -C src/apps package
else
	$(MAKE) -C src/$(CORE) package
endif
else
	./scripts/package.sh
endif

# ── Publish a core to GitHub Releases ────────────────────────────────
# `make release CORE=<name>`  → build + package the core, then create a DRAFT
#                               GitHub release tagged <core>-v<version> (version
#                               read from the core's core.json). Notes are the
#                               commit subjects since the previous <core>-v* tag,
#                               or "first release" when there is no previous tag.
#   PREV=<tag>    override the changelog baseline (e.g. PREV=v1.1.12 to bridge
#                 Doom's first per-core release off the legacy v1.1.x series)
#   PUBLISH=1     publish a live release instead of a draft
release:
	@test -n "$(CORE)" || { \
		printf "Usage: make release CORE=<name> [PREV=<tag>] [PUBLISH=1]\n"; \
		exit 1; \
	}
	$(MAKE) package CORE=$(CORE)
	@PREV="$(PREV)" PUBLISH="$(PUBLISH)" ./scripts/release.sh "$(CORE)"

# ── Push SDK to another SDK checkout ────────────────────────────────
# Mirrors source + os.bin + bank.ofsf into another SDK at $(DEST).
# Leaves the FPGA core (os25/os30.rbf_r, ap_core.sof, loader.bin)
# untouched so the destination keeps its own core build.  Existing
# files in DEST that aren't in this tree are left alone — no --delete.
push:
	@test -n "$(DEST)" || { \
		printf "Usage: make push DEST=\"path/to/other/sdk\"\n"; \
		exit 1; \
	}
	@test -d "$(DEST)/src/sdk" || { \
		printf "Not an openfpgaOS SDK at $(DEST)\n"; \
		exit 1; \
	}
	@printf "$(C_HEAD)[push]$(C_RESET) → $(DEST) (FPGA core left intact)\n"
	@rsync -a --exclude='.obj/' --exclude='build/' --exclude='dist/' \
	          --exclude='releases/' --exclude='._*' \
	          src/ "$(DEST)/src/"
	@rsync -a --exclude='._*' scripts/ "$(DEST)/scripts/"
	@cp -f Makefile  "$(DEST)/Makefile"
	@# Never clobber the destination's own docs (a game repo keeps its
	@# game README/GETTING_STARTED) — only seed them if absent.
	@[ -f "$(DEST)/README.md" ] || cp -f README.md "$(DEST)/README.md"
	@[ -f "$(DEST)/GETTING_STARTED.md" ] || { [ -f GETTING_STARTED.md ] && cp -f GETTING_STARTED.md "$(DEST)/GETTING_STARTED.md"; } || true
	@mkdir -p "$(DEST)/runtime/pocket"
	@cp -f runtime/pocket/os.bin "$(DEST)/runtime/pocket/os.bin"
	@# loader.bin is the target-generic chip32 variant selector (NOT a
	@# per-core bitstream), so it must propagate — otherwise game repos keep a
	@# stale loader that ignores VARIANT=os30 and always boots os25.
	@cp -f runtime/pocket/loader.bin "$(DEST)/runtime/pocket/loader.bin"
	@cp -f runtime/bank.ofsf     "$(DEST)/runtime/bank.ofsf"
	@printf "  skipped: runtime/pocket/{os25.rbf_r, os30.rbf_r, ap_core.sof}\n"

# ── Build host tools ────────────────────────────────────────────────
tools:
	$(MAKE) -C src/tools/phdp

# ── Clean ────────────────────────────────────────────────────────────
# `clean` removes build intermediates (build/, .obj/) but PRESERVES
# releases/ — packaged ZIPs are deliverables, not intermediates, and
# wiping them on a routine clean is a data-loss surprise.  Use
# `distclean` to also remove releases/.
# Bare `make clean` always full-wipes — only an EXPLICIT CORE= narrows it
# to one core (the defaulted CORE from a discovered custom core does not).
clean:
ifneq ($(CORE_EXPLICIT),)
ifeq ($(CORE),sdk)
	$(MAKE) -C src/apps clean
else
	$(MAKE) -C src/$(CORE) clean
endif
else
	$(MAKE) -C src/apps clean
	$(MAKE) -C src/tools/phdp clean
	@for d in src/*/; do \
		[ "$$d" = "src/apps/" ] || [ "$$d" = "src/sdk/" ] || [ "$$d" = "src/tools/" ] && continue; \
		[ -f "$$d/Makefile" ] && $(MAKE) -C "$$d" clean; \
	done
	rm -rf build .obj
endif

# Full wipe including packaged release ZIPs.
distclean: clean
	rm -rf releases

.PHONY: all help setup core build debug test copy package release push tools clean distclean
