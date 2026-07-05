//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * testdemo — kernel acceptance test suite
 *
 * Canonical example of:
 *   - Multi-file SDK app (test_*.c each cover one subsystem and
 *     export a single test_<name>(void) entry point)
 *   - PASS/FAIL counter + failure log + two-column terminal layout
 *     for fitting many tests on a single 40x30 screen
 *   - Subsystem coverage you'd want from a smoke test before tagging
 *     a kernel build: syscalls, malloc, file I/O via fopen/fread,
 *     terminal, timer, cache, audio, PSRAM, MIDI, networking, save
 *     subsystem, GPU triangle/span dispatch
 *
 * Behaviour:
 *   - Tests print one row per assertion in the active column.
 *   - First failure pauses; pressing any button continues.
 *   - At the end, the summary page lists every recorded failure with
 *     its "name" and a short detail string.
 *
 * This file is the harness; the actual tests live in test_*.c.
 */

#include "test.h"

int pass_count, fail_count;
char __buf[80];

/* ================================================================
 * Failure log — records up to MAX_FAILS for the summary page
 * ================================================================ */
#define MAX_FAILS 32
#define MAX_DETAIL 32
static struct {
    const char *name;
    char detail[MAX_DETAIL];
} fail_log[MAX_FAILS];
static int fail_log_count;

/* ================================================================
 * Two-column layout state
 * ================================================================ */
#define COL1_X  1       /* left column x (0-based) */
#define COL2_X  21      /* right column x */
#define ROW_TOP 4       /* first data row */
#define ROW_MAX 28      /* max rows before overflow */

static int col_row;     /* current row within columns */
static int col_side;    /* 0 = left, 1 = right */
static int section_pass;
static int section_fail;
static const char *section_name;
static int any_fail_this_run;

static void move_cursor(int row, int col) {
    printf("\033[%d;%dH", row, col);
}

static void next_slot(void) {
    if (col_side == 0) {
        col_side = 1;
    } else {
        col_side = 0;
        col_row++;
    }
}

static void section_update(void) {
    int p = pass_count - section_pass;
    int f = fail_count - section_fail;
    int row = ROW_TOP + col_row;
    int col = col_side == 0 ? COL1_X : COL2_X;
    move_cursor(row, col);
    if (f == 0)
        printf("%-12s%3d \033[92mok\033[0m", section_name, p);
    else
        printf("%-12s%3d \033[91m%dF\033[0m", section_name, p + f, f);
}

void test_pass(const char *name) {
    (void)name;
    pass_count++;
    section_update();
}

void test_fail(const char *name, const char *detail) {
    fail_count++;
    any_fail_this_run = 1;
    section_update();

    /* Log for summary page — copy detail into per-entry storage so
     * subsequent failures don't overwrite our message via __buf. */
    if (fail_log_count < MAX_FAILS) {
        fail_log[fail_log_count].name = name;
        int dlen = strlen(detail);
        if (dlen >= MAX_DETAIL) dlen = MAX_DETAIL - 1;
        memcpy(fail_log[fail_log_count].detail, detail, dlen);
        fail_log[fail_log_count].detail[dlen] = 0;
        fail_log_count++;
    }
}

void section_start(const char *name) {
    section_name = name;
    section_pass = pass_count;
    section_fail = fail_count;
    int row = ROW_TOP + col_row;
    int col = col_side == 0 ? COL1_X : COL2_X;
    move_cursor(row, col);
    printf("%-12s...", name);
}

void section_end(void) {
    section_update();
    next_slot();
}

/* Wait for an A press, with explicit release/press phases.
 *
 * Pure level polling, no usleep, no edge detection. Always polls
 * before checking so the level read can never be stale. The release
 * phase guarantees any hold from a prior wait_press is consumed
 * before we look for a fresh press. */
static void wait_press(void) {
    /* Phase 1: wait until A is not held */
    while (1) {
        of_input_poll();
        if (!of_btn(OF_BTN_A)) break;
    }
    /* Phase 2: wait until A is held */
    while (1) {
        of_input_poll();
        if (of_btn(OF_BTN_A)) break;
    }
}

/* Show failure summary, paged. Last page footer is "Press A to retest".
 *
 * Uses absolute cursor positioning (no trailing newlines) so a full page
 * fits inside the terminal without scrolling — printing 25+ lines with
 * `\n` would scroll the header off the top. The visible area is shorter
 * than the 30-row spec in of_terminal.h, so cap entries at 18/page to
 * keep the footer well above the bottom edge. */
#define FAILS_PER_PAGE 18

static void show_fail_summary(void) {
    int total = fail_log_count;
    int pages = (total + FAILS_PER_PAGE - 1) / FAILS_PER_PAGE;
    if (pages == 0) pages = 1;

    for (int page = 0; page < pages; page++) {
        int start = page * FAILS_PER_PAGE;
        int end = start + FAILS_PER_PAGE;
        if (end > total) end = total;

        printf("\033[2J\033[H");
        move_cursor(2, 3);
        printf("\033[91mFailed Tests: %d\033[0m  page %d/%d",
               total, page + 1, pages);
        move_cursor(3, 3);
        printf("--------------------------------");

        for (int i = start; i < end; i++) {
            /* 40-col screen, indent 2, layout per row:
             *   "NN " (3) + name (14) + " " (1) + detail (20) = 38 cols.
             * Both name and detail MUST be truncated — long values
             * would wrap to the next line and push the rest of the
             * page down off-screen. */
            char nm[15];
            int nlen = strlen(fail_log[i].name);
            if (nlen > 14) nlen = 14;
            memcpy(nm, fail_log[i].name, nlen);
            nm[nlen] = 0;

            char det[21];
            int dlen = strlen(fail_log[i].detail);
            if (dlen > 20) dlen = 20;
            memcpy(det, fail_log[i].detail, dlen);
            det[dlen] = 0;

            move_cursor(4 + (i - start), 3);
            printf("\033[91m%2d\033[0m %-14s %s", i + 1, nm, det);
        }

        move_cursor(4 + FAILS_PER_PAGE + 1, 3);
        if (page + 1 < pages)
            printf("Press A for next page");
        else
            printf("Press A to retest");
        wait_press();
    }
}

/* ================================================================
 * Test sections — grouped by function
 * ================================================================ */
typedef void (*test_fn)(void);

static const test_fn tests[] = {
    test_timer,
    test_timer_edge,
    test_malloc,
    test_malloc_edge,
    test_malloc_free,
    test_memset_stack,
    test_cache_primitives,
    test_cache,
    test_file_slots,
    test_file_negative,
    test_file_io,
    test_saves,
    test_posix_saves,
    test_dma_cache,
    test_posix_file_io,
    test_lseek_readahead,
    test_lseek_large_read,
    test_oversize_read,
    test_file_limit,
    test_shutdown,
    test_mixer,
    test_mixer_adv,
    test_mixer_stress,
    /* MIDI/audio coverage. test_midi/test_midi_smp now wait via
     * midi_wait_ms() (interrupts enabled, ISR-driven) instead of calling
     * of_midi_pump() themselves, so they no longer race of_midi_play()'s
     * timer-ISR pump — the fix for the old MD.20b+ stall cascade.
     * Re-verify on hardware across all NUM_ITERATIONS passes. */
    test_midi,
    test_midi_smp,
    test_net,
    test_interact,
    test_audio,
    test_audio_stream,
    test_printf,
    test_printf_edge,
    test_string,
    test_string_edge,
    test_lzw,
    test_version,
};

#define NUM_TESTS (sizeof(tests) / sizeof(tests[0]))
#define NUM_ITERATIONS 10

int main(void) {
    for (int iteration = 0; iteration < NUM_ITERATIONS; iteration++) {
        pass_count = 0;
        fail_count = 0;
        fail_log_count = 0;
        col_row = 0;
        col_side = 0;
        any_fail_this_run = 0;

        printf("\033[2J\033[H");
        printf("\n  \033[93mopenfpgaOS Test Suite  [%d/%d]\033[0m\n",
               iteration + 1, NUM_ITERATIONS);
        printf("  --------------------------------\n");

        for (unsigned i = 0; i < NUM_TESTS; i++) {
            tests[i]();
            /* Wrap to next page if columns overflow */
            if (col_row >= ROW_MAX) {
                int row = ROW_TOP + col_row + 1;
                move_cursor(row, 1);
                printf("  ... more (A to continue)");
                wait_press();
                col_row = 0;
                col_side = 0;
                printf("\033[2J\033[H");
                printf("\n  \033[93mopenfpgaOS Test Suite  [%d/%d] cont.\033[0m\n",
                       iteration + 1, NUM_ITERATIONS);
                printf("  --------------------------------\n");
            }
        }

        /* Summary line */
        int row = ROW_TOP + col_row + 1;
        move_cursor(row, 1);
        printf("  --------------------------------\n");
        printf("  Total: %d passed", pass_count);
        if (fail_count > 0)
            printf(", \033[91m%d failed\033[0m", fail_count);
        printf("\n");

        if (any_fail_this_run) {
            printf("  \033[91mFAILED\033[0m — details in 2s\n");
            usleep(2000 * 1000);
            show_fail_summary();
            /* show_fail_summary's last page asked the user to retest —
             * restart the iteration loop from the top. */
            iteration = -1;
            continue;
        }

        if (iteration < NUM_ITERATIONS - 1) {
            printf("  \033[92mPASS\033[0m — next in 2s\n");
            usleep(2000 * 1000);
        } else {
            printf("  \033[92mALL %d ITERATIONS PASSED\033[0m\n", NUM_ITERATIONS);
        }
    }

    while (1)
        usleep(100 * 1000);
    return 0;
}
