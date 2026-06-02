//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_terminal.h -- Terminal API for openfpgaOS
 *
 * 40x30 text console with 16-color support and CP437 character set.
 *
 * Use printf with ANSI escape sequences for terminal output.
 * The ACS_* character constants are provided for box drawing.
 */

#ifndef OF_TERMINAL_H
#define OF_TERMINAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ======================================================================
 * Alternate Character Set (ncurses-compatible names)
 *
 * CP437 box drawing, block elements, and special characters.
 * Use with printf("%c", ACS_VLINE).
 * ====================================================================== */

/* Helper: cast to char for printf %c (char is signed on rv32) */
#define _ACS(c) ((char)(unsigned char)(c))

/* Box drawing - single line */
#define ACS_VLINE       _ACS(179)   /* │ */
#define ACS_HLINE       _ACS(196)   /* ─ */
#define ACS_ULCORNER    _ACS(218)   /* ┌ */
#define ACS_URCORNER    _ACS(191)   /* ┐ */
#define ACS_LLCORNER    _ACS(192)   /* └ */
#define ACS_LRCORNER    _ACS(217)   /* ┘ */
#define ACS_TTEE        _ACS(194)   /* ┬ */
#define ACS_BTEE        _ACS(193)   /* ┴ */
#define ACS_LTEE        _ACS(195)   /* ├ */
#define ACS_RTEE        _ACS(180)   /* ┤ */
#define ACS_PLUS        _ACS(197)   /* ┼ */

/* Box drawing - double line */
#define ACS_D_VLINE     _ACS(186)   /* ║ */
#define ACS_D_HLINE     _ACS(205)   /* ═ */
#define ACS_D_ULCORNER  _ACS(201)   /* ╔ */
#define ACS_D_URCORNER  _ACS(187)   /* ╗ */
#define ACS_D_LLCORNER  _ACS(200)   /* ╚ */
#define ACS_D_LRCORNER  _ACS(188)   /* ╝ */
#define ACS_D_TTEE      _ACS(203)   /* ╦ */
#define ACS_D_BTEE      _ACS(202)   /* ╩ */
#define ACS_D_LTEE      _ACS(204)   /* ╠ */
#define ACS_D_RTEE      _ACS(185)   /* ╣ */
#define ACS_D_PLUS      _ACS(206)   /* ╬ */

/* Block elements */
#define ACS_BLOCK       _ACS(219)   /* █ */
#define ACS_BOARD       _ACS(177)   /* ▒ */
#define ACS_CKBOARD     _ACS(176)   /* ░ */
#define ACS_BLOCK_LO    _ACS(220)   /* ▄ */
#define ACS_BLOCK_HI    _ACS(223)   /* ▐ right half block (CP437 0xDF) */

/* Arrows */
#define ACS_UARROW      _ACS(24)    /* ↑ */
#define ACS_DARROW      _ACS(25)    /* ↓ */
#define ACS_RARROW      _ACS(26)    /* → */
#define ACS_LARROW      _ACS(27)    /* ← */

/* Symbols */
#define ACS_DIAMOND     _ACS(4)     /* ♦ */
#define ACS_BULLET      _ACS(7)     /* • */
#define ACS_DEGREE      _ACS(248)   /* ° */
#define ACS_PLMINUS     _ACS(241)   /* ± */

/* Shade characters */
#define ACS_SHADE_LIGHT  _ACS(176)  /* ░ */
#define ACS_SHADE_MEDIUM _ACS(177)  /* ▒ */
#define ACS_SHADE_DARK   _ACS(178)  /* ▓ */

#ifdef __cplusplus
}
#endif

#endif /* OF_TERMINAL_H */
