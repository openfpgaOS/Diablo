#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

# openfpgaOS SDK — Per SDK app build rules
#
# Included by each src/apps/<name>/Makefile. Drives the SDK app build
# path: intermediate objects under .obj/sdk/<name>/, final ELF picked
# up by src/apps/Makefile's release step and dropped into build/sdk/.
#
# For the custom-core path (a standalone openFPGA core wrapping a
# single app under src/<name>/) see scripts/customize.sh and the
# Makefile that script generates.
#

SDK_DIR   = ../../sdk
ROOT      = $(realpath $(CURDIR)/../../..)
APP_NAME  = $(notdir $(CURDIR))
OBJ_DIR   = $(ROOT)/.obj/sdk/$(APP_NAME)
BUILD_DIR = $(OBJ_DIR)

# Auto-discover sources unless the per-app Makefile already set SRCS /
# SRCS_CXX before including this file (e.g. to pull in $(OF_MIDI_SRC)).
SRCS     ?= $(wildcard *.c)
SRCS_CXX ?= $(wildcard *.cpp)

.DEFAULT_GOAL := all

include $(SDK_DIR)/sdk.mk

all: $(OBJ_DIR)/app.elf
	@$(SIZE) $<

# UART push of this single SDK app's ELF (no SDK release/assembly step
# needed — the bare app.elf is what the loader expects).
#
# Optionally forward verbosity to phdpd:
#   make debug             -- silent phdpd
#   make debug DEBUG=v     -- start phdpd with -v (state + packet headers)
#   make debug DEBUG=t     -- start phdpd with -t (full hex trace)
DEBUG ?=
DEBUG_FLAGS = $(if $(DEBUG),-$(DEBUG))

debug: $(OBJ_DIR)/app.elf
	@$(ROOT)/scripts/debug.sh $(DEBUG_FLAGS) $(OBJ_DIR)/app.elf

# Desktop test build via SDL2 — sdk.mk already provides the app_pc rule.
test: app_pc

clean: sdk-clean

.PHONY: all debug test clean
