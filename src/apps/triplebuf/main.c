//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * triplebuf — triple-buffered framebuffer + flip self-test
 *
 * Canonical example of:
 *   - of_video_surface() / of_video_flip() / of_video_wait_flip()
 *     contract: three rotating buffers, vsync-paced or unpaced
 *   - data retention across flips (the buffer you wrote returns intact
 *     after the rotation completes)
 *   - of_video_clear() filling all three buffers in one call
 *
 * The first half is an assertion-driven self-test that prints PASS/FAIL
 * to UART; the second half is a rolling RGB visual demo that lets you
 * eyeball the rotation cadence.
 *
 * Controls:
 *   none — runs continuously after the test prints.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "of.h"

static int pass, fail;

#define ASSERT(name, cond) do { \
    if (cond) { pass++; printf("  PASS %s\n", name); } \
    else      { fail++; printf("  FAIL %s\n", name); } \
} while (0)

static void fill_screen(uint8_t *fb, uint8_t color) {
    memset(fb, color, OF_SCREEN_W * OF_SCREEN_H);
}

int main(void) {
    of_video_init();

    /* Five named palette entries used by the visual demo + assertions. */
    of_video_palette(1, 0x00FF4444);  /* red   */
    of_video_palette(2, 0x0044FF44);  /* green */
    of_video_palette(3, 0x004444FF);  /* blue  */
    of_video_palette(4, 0x00FFFFFF);  /* white */
    of_video_palette(5, 0x00FF0000);  /* solid red — used by tests below */

    /* ---- Buffer-rotation tests --------------------------------- */

    ASSERT("surface non-NULL", of_video_surface() != NULL);

    /* After three vsync-paced flips we should be back at the buffer
     * we started with — the OS hands them out 0,1,2,0,1,2,... and
     * each of_video_wait_flip lets the swap actually consume. */
    uint8_t *bufs[4];
    bufs[0] = of_video_surface(); of_video_flip(); of_video_wait_flip();
    bufs[1] = of_video_surface(); of_video_flip(); of_video_wait_flip();
    bufs[2] = of_video_surface(); of_video_flip(); of_video_wait_flip();
    bufs[3] = of_video_surface();
    ASSERT("3-cycle returns to start", bufs[3] == bufs[0]);
    ASSERT("buf 0 != 1",               bufs[0] != bufs[1]);
    ASSERT("buf 1 != 2",               bufs[1] != bufs[2]);
    ASSERT("buf 0 != 2",               bufs[0] != bufs[2]);

    /* Two flips back-to-back without waiting for vsync: the OS should
     * swallow the second swap onto the latest queued buffer rather
     * than dropping a flip entirely.  We just check the surface
     * pointer is still legal. */
    uint8_t *r0 = of_video_surface();
    of_video_flip(); of_video_flip();
    uint8_t *r1 = of_video_surface();
    ASSERT("rapid flip returns sane buffer", r0 != NULL && r1 != NULL);

    /* Data retention: write to a buffer, rotate fully, the byte should
     * still be there when we cycle back to that physical buffer. */
    bufs[0] = of_video_surface();   bufs[0][0] = 0xAA;
    of_video_flip(); of_video_wait_flip();
    bufs[1] = of_video_surface();   bufs[1][0] = 0xBB;
    of_video_flip(); of_video_wait_flip();
    bufs[2] = of_video_surface();   bufs[2][0] = 0xCC;
    of_video_flip(); of_video_wait_flip();
    uint8_t *back = of_video_surface();
    ASSERT("retain across rotation", back == bufs[0] && back[0] == 0xAA);

    /* of_video_clear fills every buffer (all three) with the given
     * index in one syscall; this is the cheap way to blank the screen
     * before a fresh frame. */
    of_video_clear(0x42);
    uint8_t *fb = of_video_surface();
    ASSERT("clear fills FB",
           fb[0] == 0x42 && fb[OF_SCREEN_W * OF_SCREEN_H - 1] == 0x42);

    /* Stress: 60 unpaced flips should not deadlock. */
    for (int i = 0; i < 60; i++) of_video_flip();
    ASSERT("60 unpaced flips", 1);

    printf("\n  %d passed, %d failed\n", pass, fail);

    /* ---- Visual demo: red/green/blue cycle --------------------- */

    static const uint8_t colors[] = { 1, 2, 3 };
    int frame = 0;
    while (1) {
        fill_screen(of_video_surface(), colors[frame++ % 3]);
        of_video_flip();
        usleep(500 * 1000);
    }
}
