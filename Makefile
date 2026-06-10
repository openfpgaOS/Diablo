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
# Quick start:
#   make setup          Install RISC-V toolchain
#   make core           Scaffold a custom core

# ── Paths ────────────────────────────────────────────────────────────
RUNTIME = runtime

# ── Detect a custom core in src/<name>/ ──────────────────────────────
# Any src/<name>/ directory with a Makefile that isn't apps/, sdk/, or
# tools/ is treated as a custom core. Used to drive the help text and
# the no-argument default for `make debug`.
APP_NAME := $(shell for d in src/*/; do \
	[ "$$d" = "src/apps/" ] || [ "$$d" = "src/sdk/" ] || [ "$$d" = "src/tools/" ] && continue; \
	[ -f "$$d/Makefile" ] && basename "$$d"; \
done)

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
	@echo "    \\____/___/  \033[93mv0.3\033[0m SDK"
	@printf "$(C_RESET)\n"
	@printf "  $(C_HEAD)Getting started:$(C_RESET)\n"
	@printf "    $(C_CMD)make $(C_VERB)setup$(C_RESET)                    Install RISC-V toolchain\n"
	@printf "    $(C_CMD)make $(C_VERB)core$(C_RESET)                     Scaffold a custom core (src/<name>/)\n"
	@echo ""
	@printf "  $(C_HEAD)Custom core (work from src/$(A)/):$(C_RESET)\n"
	@printf "    $(C_CMD)cd src/$(A)$(C_RESET)\n"
	@printf "    $(C_CMD)make$(C_RESET)                          Build the custom core\n"
	@printf "    $(C_CMD)make $(C_VERB)debug$(C_RESET)                    Build, push via UART, stream console\n"
	@printf "    $(C_CMD)make $(C_VERB)copy$(C_RESET)                     Copy this custom core to Pocket SD\n"
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
	@printf "    $(C_CMD)make $(C_VERB)push$(C_RESET) $(C_ARG)DEST=\"path/to/sdk\"$(C_RESET)   Mirror src + os.bin into another SDK (keeps its core)\n"
	@printf "    $(C_CMD)make $(C_VERB)clean$(C_RESET)                    Remove all build artifacts\n"

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
debug:
ifdef APP
	$(MAKE) -C src/apps debug APP=$(APP)
else ifdef CORE
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
# Leaves the FPGA core (bitstream.rbf_r, ap_core.sof, loader.bin)
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
	@cp -f README.md "$(DEST)/README.md"
	@[ -f GETTING_STARTED.md ] && cp -f GETTING_STARTED.md "$(DEST)/GETTING_STARTED.md" || true
	@mkdir -p "$(DEST)/runtime"
	@cp -f runtime/os.bin    "$(DEST)/runtime/os.bin"
	@cp -f runtime/bank.ofsf "$(DEST)/runtime/bank.ofsf"
	@printf "  skipped: runtime/{bitstream.rbf_r, ap_core.sof, loader.bin}\n"

# ── Build host tools ────────────────────────────────────────────────
tools:
	$(MAKE) -C src/tools/phdp

# ── Clean ────────────────────────────────────────────────────────────
clean:
ifdef CORE
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
	rm -rf build .obj releases
endif

.PHONY: all help setup core build debug test copy package release push tools clean
