//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_fastram.h -- Fast memory section attributes
 *
 * Place performance-critical code/data in fast RAM (BRAM on Pocket,
 * SRAM on MiSTer, no-op on PC). Provides zero-latency access without
 * cache misses.
 *
 * Usage:
 *   OF_FASTTEXT static int hot_function(int x) { ... }
 *   OF_FASTDATA static int lookup_table[256] = { ... };
 *   OF_FASTRODATA static const int constants[16] = { ... };
 *
 * Build with the BRAM-enabled linker script (app.ld already supports this).
 * Apps that don't use these attributes are unaffected.
 */

#ifndef OF_FASTRAM_H
#define OF_FASTRAM_H

/* Place function in fast RAM. noinline prevents the compiler from
 * inlining the function body back into slow memory callers.
 *
 * align-functions/align-loops are forced DOWN to 4 here: the SDK builds
 * with -falign-functions=64 -falign-loops=64 so SDRAM-resident code
 * starts on I$-line boundaries, but fast RAM is an UNCACHED region (PMA
 * main=0 -- fetches bypass the I$), so 64-byte alignment inside
 * .app_fasttext is pure NOP padding.  With every hot function padded to
 * 64 bytes the 14 KB APP_BRAM region loses hundreds of bytes to
 * alignment alone (Duke3D overflowed the region on padding, not code).
 * Placement-only: zero performance effect in an uncached region. */
#define OF_FASTTEXT   __attribute__((section(".app_fasttext"), noinline, \
                                     optimize("align-functions=4", "align-loops=4")))

/* Place initialized data in fast RAM */
#define OF_FASTDATA   __attribute__((section(".app_fastdata")))

/* Place read-only data in fast RAM */
#define OF_FASTRODATA __attribute__((section(".app_fastrodata")))

#endif /* OF_FASTRAM_H */
