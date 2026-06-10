#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

# openfpgaOS SDK — SDK apps
#
# This Makefile drives the "SDK app" build path: every app under
# src/apps/<name>/ is compiled and bundled into the shared SDK demo
# core (dist/sdk/ → build/sdk/), which packages to a single ZIP.
#
# For the other path — a CUSTOM CORE that ships its own standalone
# openFPGA core wrapping a single app — use `make core` from the
# SDK root instead.
#
# Targets:
#   make                  Build every SDK app → build/sdk/
#   make new APP=demo     Scaffold a new SDK app under src/apps/demo/
#   make debug APP=demo   Push a single SDK app's ELF over UART
#   make test APP=demo    Build a single SDK app for desktop (SDL2)
#   make copy             Copy build/sdk/ to the Pocket SD card
#   make package          Package build/sdk/ as a single ZIP
#   make clean            Remove build artifacts
#

SDK_ROOT  = $(realpath $(CURDIR)/../..)
RUNTIME   = $(SDK_ROOT)/runtime
DIST      = $(SDK_ROOT)/dist/sdk
CORE_ID   = ThinkElastic.openfpgaOS
PLATFORM  = openfpgaos
BUILD     = $(SDK_ROOT)/build/sdk

# Discover SDK apps: every src/apps/<name>/ that has its own Makefile.
APPS = $(sort $(patsubst %/,%,$(dir $(wildcard */Makefile))))

all: build-apps release

build-apps:
	@for app in $(APPS); do \
		echo "Building SDK app: $$app..."; \
		$(MAKE) -C $$app || exit 1; \
	done

# Assemble: dist/ (static) + runtime + ELFs + data files = deployable
release:
	@echo "Assembling build/sdk/..."
	@rm -rf $(BUILD)
	@mkdir -p $(SDK_ROOT)/build
	@cp -r $(DIST)/ $(BUILD)/
	@# Runtime binaries
	@cp $(RUNTIME)/bitstream.rbf_r $(RUNTIME)/loader.bin $(BUILD)/Cores/$(CORE_ID)/
	@mkdir -p $(BUILD)/Assets/$(PLATFORM)/common
	@cp $(RUNTIME)/os.bin $(BUILD)/Assets/$(PLATFORM)/common/
	@# Sound bank (SC-55 GM, shared by all MIDI apps)
	@cp $(RUNTIME)/bank.ofsf $(BUILD)/Assets/$(PLATFORM)/common/ 2>/dev/null || true
	@# Demo ELFs + data files
	@for app in $(APPS); do \
		[ -f "$(SDK_ROOT)/.obj/sdk/$$app/app.elf" ] && \
			cp "$(SDK_ROOT)/.obj/sdk/$$app/app.elf" "$(BUILD)/Assets/$(PLATFORM)/common/$$app.elf"; \
		for f in $$app/*.mid $$app/*.mod $$app/*.wav $$app/*.dat $$app/*.png \
		         $$app/*.bin $$app/*.cfg $$app/*.iso; do \
			[ -f "$$f" ] && cp "$$f" "$(BUILD)/Assets/$(PLATFORM)/common/"; \
		done; \
	done 2>/dev/null; true
	@echo "Ready: build/sdk/"

new:
	@test -n "$(APP)" || { echo "Usage: make new APP=myapp"; exit 1; }
	@$(SDK_ROOT)/scripts/new.sh --sdk-app "$(APP)"

# Single-app debug / test. Both delegate to the per-app Makefile so
# they pick up that app's exact SRCS / CFLAGS, including the special
# cases like mididemo (pulls of_midi.c) and cxxdemo (C++ ABI shim).
debug:
	@test -n "$(APP)" || { echo "Usage: make debug APP=<sdk-app>"; exit 1; }
	@test -d "$(APP)" || { echo "No such SDK app: src/apps/$(APP)"; exit 1; }
	$(MAKE) -C $(APP) debug

test:
	@test -n "$(APP)" || { echo "Usage: make test APP=<sdk-app>"; exit 1; }
	@test -d "$(APP)" || { echo "No such SDK app: src/apps/$(APP)"; exit 1; }
	$(MAKE) -C $(APP) test

copy: all
	@$(SDK_ROOT)/scripts/copy.sh

package: all
	@$(SDK_ROOT)/scripts/package.sh

clean:
	@for app in $(APPS); do \
		$(MAKE) -C $$app clean; \
	done
	rm -rf $(BUILD)

.PHONY: all build-apps release new debug test copy package clean
