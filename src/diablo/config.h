#pragma once
/*
 * config.h -- hand-written replacement for DevilutionX's CMake-generated
 * config.h.
 *
 * Upstream CMake writes ONLY the project name + version macros into this
 * header (see Source/CMakeLists.txt `file(GENERATE ... config.h)`); every
 * other build knob (NONET, NOSOUND, DEFAULT_WIDTH/HEIGHT, DEVILUTIONX_*)
 * is a -D compile definition supplied by the Makefile, or has an in-source
 * #ifndef default. We mirror that split here.
 */

#define PROJECT_NAME "DevilutionX"
#define PROJECT_VERSION "1.5.5"
#define PROJECT_VERSION_WITH_SUFFIX "1.5.5"
#define PROJECT_VERSION_MAJOR 1
#define PROJECT_VERSION_MINOR 5
#define PROJECT_VERSION_PATCH 5
