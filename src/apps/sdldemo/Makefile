#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

# sdldemo -- SDL2 compatibility-layer demo / porting template.
#
# Nothing SDL-specific is needed here: sdk.mk auto-links the SDL2 shim
# (src/sdk/of_sdl2.c) into every app, and <SDL2/SDL.h> ships in the SDK
# include path. Just drop in your SDL sources.
#
#   make            build for the Pocket (app.elf via the shim)
#   make test       build for the desktop (links the real SDL2)
include ../app.mk
