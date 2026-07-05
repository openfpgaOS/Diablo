//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * memdemo — characterise SDRAM bandwidth
 *
 * Canonical example of:
 *   - Comparing cached vs uncached SDRAM throughput by aliasing the
 *     same physical bytes through of_uncached() (the D-cache-bypass
 *     alias, derived from the caps descriptor — never hardcoded) — the
 *     diff measures the L1 D-cache + writeback path
 *   - of_time_us-driven microbenchmarks with a `volatile sink` to
 *     stop the compiler eliding the loop
 *
 * Output is a table of MB/s for sequential read/write, memset, memcpy,
 * and random access at four sizes (256 B, 1 KB, 16 KB, 256 KB).  Run
 * after kernel changes to the cache policy or SDRAM controller and
 * compare to the previous numbers — large regressions mean a path
 * dropped its burst capability or fell off the FIFO.
 *
 * Controls: any button to exit after the table prints.
 */

#include "of.h"
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

/* Aligned source buffer in SDRAM (used as memcpy source) */
static uint8_t src_buf[1024 * 1024] __attribute__((aligned(64)));
/* SDRAM destination buffer */
static uint8_t dst_buf[1024 * 1024] __attribute__((aligned(64)));
/* Random-access index table in SDRAM */
static uint32_t idx_buf[262144 / 4] __attribute__((aligned(64)));

/* Prevent the compiler from optimizing away the operations */
static volatile uint8_t sink;

/* Test sizes: 256, 1K, 16K, 256K */
static const uint32_t sizes[]  = { 256, 1024, 16384, 262144 };
static const int      reps[]   = { 1000, 1000, 200, 10 };
#define NUM_SIZES 4

static void fmt_mbps(char *out, int len, uint32_t size, uint32_t us, int r) {
    uint32_t total = size * (uint32_t)r;
    uint32_t safe = us > 0 ? us : 1;
    uint32_t x10 = (uint64_t)total * 10 / safe;
    snprintf(out, len, "%3u.%u", x10 / 10, x10 % 10);
}

/* Simple PRNG for random access (xorshift32) */
static uint32_t xor_state = 0x12345678;
static inline uint32_t xorshift32(void) {
    uint32_t x = xor_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    xor_state = x;
    return x;
}

/* ---- Benchmark primitives ---- */

static uint32_t bench_seq_read(const void *buf, uint32_t size, int r) {
    volatile uint32_t *p = (volatile uint32_t *)buf;
    uint32_t n_words = size / 4;
    uint32_t t0 = of_time_us();
    for (int i = 0; i < r; i++) {
        uint32_t acc = 0;
        for (uint32_t j = 0; j < n_words; j++)
            acc += p[j];
        sink = (uint8_t)acc;
    }
    uint32_t t1 = of_time_us();
    return t1 - t0;
}

static uint32_t bench_seq_write(void *buf, uint32_t size, int r) {
    volatile uint32_t *p = (volatile uint32_t *)buf;
    uint32_t n_words = size / 4;
    uint32_t t0 = of_time_us();
    for (int i = 0; i < r; i++) {
        for (uint32_t j = 0; j < n_words; j++)
            p[j] = j;
    }
    uint32_t t1 = of_time_us();
    sink = *(volatile uint8_t *)buf;
    return t1 - t0;
}

static uint32_t bench_memset(void *dst, uint32_t size, int r) {
    uint32_t t0 = of_time_us();
    for (int i = 0; i < r; i++)
        memset(dst, i & 0xFF, size);
    uint32_t t1 = of_time_us();
    sink = *(volatile uint8_t *)dst;
    return t1 - t0;
}

static uint32_t bench_memcpy(void *dst, const void *src, uint32_t size, int r) {
    memset((void *)src, 0xAA, size);
    uint32_t t0 = of_time_us();
    for (int i = 0; i < r; i++)
        memcpy(dst, src, size);
    uint32_t t1 = of_time_us();
    sink = *(volatile uint8_t *)dst;
    return t1 - t0;
}

static uint32_t bench_random(void *buf, uint32_t size, int r) {
    volatile uint32_t *p = (volatile uint32_t *)buf;
    uint32_t n_words = size / 4;
    /* Pre-calculate random indices into a separate SDRAM buffer. */
    xor_state = 0x12345678;
    for (uint32_t j = 0; j < n_words; j++)
        idx_buf[j] = xorshift32() % n_words;
    uint32_t t0 = of_time_us();
    for (int i = 0; i < r; i++) {
        for (uint32_t j = 0; j < n_words; j++)
            p[idx_buf[j]] = j;
    }
    uint32_t t1 = of_time_us();
    sink = *(volatile uint8_t *)buf;
    return t1 - t0;
}

/* ---- Output helpers ---- */

#define SEP "---------------------------------------"

static void print_header(const char *label) {
    printf("\n%-8s %5s   %5s   %5s   %5s\n", label, "256", "1K", "16K", "256K");
    printf(SEP "\n");
}

static void print_row(const char *name, char results[][16], int n) {
    printf("%-8s", name);
    for (int i = 0; i < n; i++)
        printf(i < n - 1 ? "%7s " : "%7s", results[i]);
    printf("\n");
}

/* ---- Per-region suites ---- */

static void run_sdram(void) {
    void *dst = dst_buf;
    void *src = src_buf;
    char r[NUM_SIZES][16];

    print_header("SDRAM");

    for (int i = 0; i < NUM_SIZES; i++)
        fmt_mbps(r[i], 16, sizes[i], bench_memset(dst, sizes[i], reps[i]), reps[i]);
    print_row("memset", r, NUM_SIZES);

    for (int i = 0; i < NUM_SIZES; i++)
        fmt_mbps(r[i], 16, sizes[i], bench_memcpy(dst, src, sizes[i], reps[i]), reps[i]);
    print_row("memcpy", r, NUM_SIZES);

    for (int i = 0; i < NUM_SIZES; i++)
        fmt_mbps(r[i], 16, sizes[i], bench_seq_read(dst, sizes[i], reps[i]), reps[i]);
    print_row("seq_rd", r, NUM_SIZES);

    for (int i = 0; i < NUM_SIZES; i++)
        fmt_mbps(r[i], 16, sizes[i], bench_seq_write(dst, sizes[i], reps[i]), reps[i]);
    print_row("seq_wr", r, NUM_SIZES);

    /* Bypass the D-cache by reading/writing the same physical bytes through
     * their uncached alias. of_uncached() forms it from caps->sdram_base /
     * sdram_uncached_base, so there are no target addresses compiled in. */
    void *udst = (void *)of_uncached(dst);

    for (int i = 0; i < NUM_SIZES; i++)
        fmt_mbps(r[i], 16, sizes[i], bench_seq_read(udst, sizes[i], reps[i]), reps[i]);
    print_row("srd/u", r, NUM_SIZES);

    for (int i = 0; i < NUM_SIZES; i++)
        fmt_mbps(r[i], 16, sizes[i], bench_seq_write(udst, sizes[i], reps[i]), reps[i]);
    print_row("swr/u", r, NUM_SIZES);

    for (int i = 0; i < NUM_SIZES; i++)
        fmt_mbps(r[i], 16, sizes[i], bench_random(dst, sizes[i], reps[i]), reps[i]);
    print_row("random", r, NUM_SIZES);

    for (int i = 0; i < NUM_SIZES; i++)
        fmt_mbps(r[i], 16, sizes[i], bench_random(udst, sizes[i], reps[i]), reps[i]);
    print_row("rand/u", r, NUM_SIZES);

}

int main(void) {
    printf("\033[2J\033[H");
    printf("SDRAM Benchmark (MB/s)\n");

    run_sdram();

    printf("\n Done. Press any button.\n");

    while (1) {
        of_input_poll();
        of_input_state_t st;
        of_input_state(0, &st);
        if (st.buttons_pressed)
            break;
        usleep(16 * 1000);
    }

    return 0;
}
