//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

#include "test.h"
#include <time.h>
#include <unistd.h>

void test_timer(void) {
    section_start("Timer");

    /* of_time_ms returns increasing values */
    uint32_t t1 = of_time_ms();
    ASSERT("nonzero", t1 > 0);

    /* usleep(10000) delays ~10ms */
    usleep(50 * 1000);
    uint32_t elapsed = of_time_ms() - t1;
    ASSERT("delay", elapsed >= 40 && elapsed < 200);

    /* of_time_us has microsecond resolution */
    ASSERT("time_us", of_time_us() > 0);

    section_end();
}

void test_timer_edge(void) {
    section_start("Timer Edge");

    /* usleep(0) returns quickly */
    uint32_t t0 = of_time_us();
    usleep(0);
    uint32_t elapsed = of_time_us() - t0;
    ASSERT("usleep 0", elapsed < 100);

    /* usleep(0) via of_time_ms */
    t0 = of_time_ms();
    usleep(0);
    ASSERT("usleep 0 ms", of_time_ms() - t0 < 5);

    /* usleep(1) returns quickly */
    t0 = of_time_us();
    usleep(1);
    elapsed = of_time_us() - t0;
    ASSERT("usleep 1", elapsed < 100);

    /* Monotonicity: of_time_us never goes backward */
    uint32_t prev = of_time_us();
    int mono_ok = 1;
    for (int i = 0; i < 1000; i++) {
        uint32_t now = of_time_us();
        if (now < prev) { mono_ok = 0; break; }
        prev = now;
    }
    ASSERT("monotonic", mono_ok);

    section_end();
}
