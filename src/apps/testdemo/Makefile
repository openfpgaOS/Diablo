#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

# testdemo exercises MIDI; pull in the MIDI engine + sample voice engine.
SDK_DIR = ../../sdk
SRCS = $(wildcard *.c) \
       $(SDK_DIR)/of_midi.c \
       $(SDK_DIR)/of_smp_bank.c \
       $(SDK_DIR)/of_smp_voice.c \
       $(SDK_DIR)/of_smp_tables.c
include ../app.mk
