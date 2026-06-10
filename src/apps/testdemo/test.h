//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * test.h — Shared test harness for openfpgaOS test suite
 */

#ifndef TEST_H
#define TEST_H

#include "of.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

extern int pass_count, fail_count;

void test_pass(const char *name);
void test_fail(const char *name, const char *detail);
void section_start(const char *name);
void section_end(void);

extern char __buf[80];

#define ASSERT(name, cond) do { \
    if (cond) test_pass(name); \
    else test_fail(name, #cond); \
} while(0)

#define ASSERT_EQ(name, a, b) do { \
    if ((a) == (b)) test_pass(name); \
    else { \
        snprintf(__buf, sizeof(__buf), "%d != %d", (int)(a), (int)(b)); \
        test_fail(name, __buf); \
    } \
} while(0)

/* Test functions */
void test_timer(void);
void test_timer_edge(void);
void test_malloc(void);
void test_malloc_edge(void);
void test_malloc_free(void);
void test_memset_stack(void);
void test_file_slots(void);
void test_file_negative(void);
void test_file_io(void);
void test_posix_file_io(void);
void test_lseek_readahead(void);
void test_lseek_large_read(void);
void test_dma_cache(void);
void test_file_limit(void);
void test_oversize_read(void);
void test_saves(void);
void test_posix_saves(void);
void test_shutdown(void);
void test_mixer(void);
void test_net(void);
void test_interact(void);
void test_audio(void);
void test_printf(void);
void test_printf_edge(void);
void test_string(void);
void test_string_edge(void);
void test_lzw(void);
void test_version(void);
void test_cache_primitives(void);
void test_cache(void);
void test_mixer_adv(void);
void test_mixer_stress(void);
void test_midi(void);
void test_midi_smp(void);
void test_audio_stream(void);

#endif /* TEST_H */
