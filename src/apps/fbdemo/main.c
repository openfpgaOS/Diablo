//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * fbdemo — load an indexed PNG and display it
 *
 * Canonical example of:
 *   - Reading a data slot by name (`fopen("slot:N", "rb")`)
 *   - Setting up a 256-entry palette in one call (`of_video_palette_bulk`)
 *   - Cycling between the three display modes the OS supports
 *     (terminal, framebuffer, overlay)
 *
 * Slot map:
 *   slot:4  PNG file decoded at startup (see Assets/.../instance.json)
 *
 * Controls:
 *   A / B / Start  cycle display mode (Terminal -> Framebuffer -> Overlay)
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "of.h"
#include "png.h"

#define SCREEN_W       320
#define SCREEN_H       240
#define MAX_PNG_SIZE   (128 * 1024)

static uint8_t  png_buf[MAX_PNG_SIZE];
static uint8_t  pixel_buf[SCREEN_W * SCREEN_H + SCREEN_H + MAX_PNG_SIZE];
static uint32_t raw_palette[256];

/* Park forever printing the message — used for unrecoverable startup
 * failures.  printf still reaches the UART even in framebuffer modes,
 * so the failure is visible from the host even if the screen is dark. */
static void halt(const char *msg) {
    printf("[fbdemo] FATAL: %s\n", msg);
    for (;;) usleep(100 * 1000);
}

int main(void) {
    of_video_init();
    of_video_set_display_mode(OF_DISPLAY_OVERLAY);

    printf("[fbdemo] Loading slot:4 ...\n");

    FILE *f = fopen("slot:4", "rb");
    if (!f) halt("cannot open slot:4");
    uint32_t file_size = (uint32_t)fread(png_buf, 1, MAX_PNG_SIZE, f);
    fclose(f);
    if (file_size == 0) halt("empty file");
    printf("[fbdemo] PNG size: %u bytes\n", (unsigned)file_size);

    int w, h;
    int rc = png_decode(png_buf, file_size, raw_palette, pixel_buf, &w, &h);
    if (rc < 0) {
        printf("[fbdemo] png_decode rc=%d\n", rc);
        halt("PNG decode failed");
    }
    printf("[fbdemo] Decoded %dx%d\n", w, h);

    /* Bulk palette upload: one syscall instead of 256. */
    of_video_palette_bulk(raw_palette, 256);

    /* Centre the image in the framebuffer.  of_video_clear handles the
     * cache-flush for the cleared bytes; the per-row memcpy below is
     * picked up by the next of_video_flip()'s implicit flush, so apps
     * don't need to call of_cache_clean_range themselves. */
    of_video_clear(0);
    uint8_t *fb = of_video_surface();
    int ox = (SCREEN_W - w) / 2;
    int oy = (SCREEN_H - h) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    int copy_w = w < SCREEN_W ? w : SCREEN_W;
    int copy_h = h < SCREEN_H ? h : SCREEN_H;
    for (int y = 0; y < copy_h; y++)
        memcpy(&fb[(oy + y) * SCREEN_W + ox], &pixel_buf[y * w], copy_w);
    of_video_flip();

    /* Overlay terminal text on top of the image.  In OVERLAY mode the
     * app's framebuffer is dimmed and the OS terminal renders in the
     * VGA-bright palette region (240..255). */
    printf("\033[2J\033[H");
    printf("OVERLAY MODE - press any button to cycle\n");

    static const char *mode_names[] = {
        [OF_DISPLAY_TERMINAL]   = "Terminal",
        [OF_DISPLAY_FRAMEBUFFER]= "Framebuffer",
        [OF_DISPLAY_OVERLAY]    = "Overlay",
    };
    int mode = OF_DISPLAY_OVERLAY;

    /* Wait for the launcher's button to be released before sampling
     * input — otherwise the press that opened the app gets re-read as
     * a mode change on frame 1. */
    for (int i = 0; i < 30; i++) { of_input_poll(); usleep(16 * 1000); }

    while (1) {
        of_input_poll();
        if (of_btn_pressed(OF_BTN_A) ||
            of_btn_pressed(OF_BTN_B) ||
            of_btn_pressed(OF_BTN_START)) {
            mode = (mode + 1) % 3;
            of_video_set_display_mode(mode);
            printf("\033[2J\033[H");
            printf("Mode: %s\n", mode_names[mode]);
        }
        usleep(16 * 1000);
    }
    return 0;
}
