#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

PC_EXTRA_CFLAGS := $(shell pkg-config --cflags SDL2_mixer 2>/dev/null)
PC_EXTRA_LIBS := $(shell pkg-config --libs SDL2_mixer 2>/dev/null || echo -lSDL2_mixer)

include ../app.mk
