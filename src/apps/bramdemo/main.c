//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * bramdemo — measure the cost of running hot code from BRAM vs SDRAM
 *
 * Canonical example of:
 *   - OF_FASTTEXT for placing performance-critical code in on-chip BRAM
 *     (zero I-cache miss latency, no SDRAM bus contention)
 *   - of_time_us() for microsecond-resolution wall-clock measurement
 *
 * Two identical 16.16 fixed-point multiply functions are benchmarked.
 * The BRAM version typically runs ~2x faster on hardware because the
 * SDRAM version is gated by I-cache fills competing with the GPU/mixer
 * AXI traffic.  The win shrinks under cold-cache or low-contention
 * conditions, so this is a useful sanity check after kernel changes
 * that touch instruction fetch or cache policy.
 *
 * Controls:
 *   any button   exit (apps that hang the launcher are annoying)
 */

#include <stdio.h>
#include <unistd.h>

#include "of.h"

typedef int32_t fixed_t;
#define FRACBITS    16
#define ITERATIONS  1000000

/* Identical bodies, different placement.  __attribute__((noinline))
 * on the SDRAM version prevents the compiler from inlining it into
 * the benchmark loop and erasing the comparison. */

OF_FASTTEXT static fixed_t
fixed_mul_bram(fixed_t a, fixed_t b) {
    return (fixed_t)(((int64_t)a * (int64_t)b) >> FRACBITS);
}

static fixed_t __attribute__((noinline))
fixed_mul_sdram(fixed_t a, fixed_t b) {
    return (fixed_t)(((int64_t)a * (int64_t)b) >> FRACBITS);
}

/* Without `volatile`, the compiler proves the loop has no side effects
 * and deletes it.  Sinking the result keeps the loop honest. */
static volatile fixed_t sink;

static uint32_t bench(fixed_t (*fn)(fixed_t, fixed_t)) {
    fixed_t a = 0x00018000;  /* 1.5  */
    fixed_t b = 0x00028000;  /* 2.5  */
    uint32_t t0 = of_time_us();
    for (int i = 0; i < ITERATIONS; i++) {
        a = fn(a, b);
        b = fn(b, a);
    }
    uint32_t t1 = of_time_us();
    sink = a + b;
    return t1 - t0;
}

int main(void) {
    printf("\033[2J\033[H");
    printf("  openfpgaOS BRAM Benchmark\n");
    printf("  =========================\n\n");
    printf("  OF_FASTTEXT fn at: 0x%08X (BRAM)\n",  (unsigned)(uintptr_t)fixed_mul_bram);
    printf("  SDRAM fn at:       0x%08X (SDRAM)\n", (unsigned)(uintptr_t)fixed_mul_sdram);
    printf("  Iterations: %d\n\n", ITERATIONS);

    printf("  Running SDRAM benchmark...\n");
    uint32_t sdram_us = bench(fixed_mul_sdram);
    printf("  Running BRAM benchmark...\n");
    uint32_t bram_us  = bench(fixed_mul_bram);

    printf("\n  Results:\n");
    printf("    SDRAM: %u us\n", sdram_us);
    printf("    BRAM:  %u us\n", bram_us);
    if (bram_us > 0) {
        /* Print speedup with one decimal.  If BRAM is slower (rare —
         * means the SDRAM I-cache was hot the whole run), the speedup
         * comes out < 1.0x and the comparison is still meaningful. */
        uint32_t speedup_x10 = (sdram_us * 10) / bram_us;
        printf("    Speedup: %u.%ux\n", speedup_x10 / 10, speedup_x10 % 10);
    }

    printf("\n  Done. Press any button to exit.\n");

    /* Simplest "any button pressed" wait: drop into the polled state
     * and check the buttons_pressed bitmask.  of_btn_pressed() requires
     * a specific mask, which is wrong here — we want any of them. */
    while (1) {
        of_input_poll();
        of_input_state_t st;
        of_input_state(0, &st);
        if (st.buttons_pressed != 0) break;
        usleep(16 * 1000);
    }
    return 0;
}
