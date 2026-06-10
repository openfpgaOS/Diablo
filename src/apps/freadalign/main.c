//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * freadalign — regression reproducer for fread() destination alignment
 *
 * Canonical example of:
 *   - Acceptance test for a kernel bug fix (run before/after the kernel
 *     change to confirm the fix takes hold)
 *   - opendir/readdir/stat for "find any file" without depending on
 *     a specific slot mapping
 *   - Why long-lived stack buffers are a footgun on this target
 *
 * The bug:
 *   fread() of an SDK-opened FILE* used to silently return 0 bytes when
 *   the destination pointer was not 512-byte aligned.  Root cause was
 *   a kernel DMA path that latched the destination's low bits to zero
 *   and dropped the read.
 *
 * What this app does:
 *   1. opendir("/"), pick the first non-app file >= 512 bytes
 *   2. fread the first 16 bytes into a 512-aligned static buffer
 *   3. fread the first 16 bytes into a stack-local buffer (4- or
 *      8-byte aligned, never 512-aligned)
 *   4. Print both byte counts and the first 16 bytes of each dest, and
 *      compare
 *
 * PASS  — both reads return 16 bytes with identical contents.
 * FAIL  — aligned returns 16, unaligned returns 0 (classic dropped DMA),
 *         or any other mismatch.
 *
 * Controls: none — runs once and parks on the verdict line.
 */

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "of.h"

/* 512-aligned static buffer — known-good destination for the bug. */
static uint8_t aligned_buf[512] __attribute__((aligned(512)));

static void hexdump16(const uint8_t *b, int got) {
    for (int i = 0; i < 16 && i < got; i++) printf("%02x ", b[i]);
    for (int i = got; i < 16; i++)          printf(".. ");
}

/* Find any readable file on the card with size >= 512.  Avoids hard-
 * coding "slot:N" so this app runs against whatever instance.json the
 * launcher gave us. */
static const char *pick_file(char *out, size_t outsz) {
    DIR *d = opendir("/");
    if (!d) return NULL;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        if (stat(e->d_name, &st) != 0) continue;
        if (st.st_size < 512) continue;
        size_t name_len = strlen(e->d_name);
        if (name_len >= outsz) continue;
        memcpy(out, e->d_name, name_len + 1);
        closedir(d);
        return out;
    }
    closedir(d);
    return NULL;
}

static void park(void) {
    for (;;) usleep(1000 * 1000);
}

int main(void) {
    of_video_init();
    of_video_set_display_mode(OF_DISPLAY_TERMINAL);
    printf("\033[2J\033[H");
    printf("freadalign — fread destination-alignment regression test\n\n");

    char fname[256];
    if (!pick_file(fname, sizeof(fname))) {
        printf("No suitable file found on card (need >= 512 B).\n");
        park();
    }
    printf("Using file: %s\n\n", fname);

    /* Test 1: aligned destination — must always work. */
    FILE *f = fopen(fname, "rb");
    if (!f) { printf("fopen('%s') failed\n", fname); park(); }
    memset(aligned_buf, 0, sizeof(aligned_buf));
    int g1 = (int)fread(aligned_buf, 1, 16, f);
    fclose(f);
    printf("aligned   (dst=%p): got=%2d buf=", (void *)aligned_buf, g1);
    hexdump16(aligned_buf, g1);
    printf("\n");

    /* Test 2: unaligned (stack) destination — what the bug used to break. */
    uint8_t stack_buf[16];
    memset(stack_buf, 0, sizeof(stack_buf));
    f = fopen(fname, "rb");
    if (!f) { printf("fopen('%s') failed (re-open)\n", fname); park(); }
    int g2 = (int)fread(stack_buf, 1, 16, f);
    fclose(f);
    printf("unaligned (dst=%p): got=%2d buf=", (void *)stack_buf, g2);
    hexdump16(stack_buf, g2);
    printf("\n\n");

    if (g1 == 16 && g2 == 16 && memcmp(aligned_buf, stack_buf, 16) == 0) {
        printf("PASS — kernel handles unaligned fread correctly.\n");
    } else if (g1 == 16 && g2 == 0) {
        printf("FAIL — unaligned fread returned 0 bytes (classic alignment bug).\n");
        printf("       Update the kernel fread DMA path before rerunning this test.\n");
    } else {
        printf("FAIL — unexpected mismatch (g1=%d g2=%d).\n", g1, g2);
    }

    park();
    return 0;
}
