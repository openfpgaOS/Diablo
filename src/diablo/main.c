/*
 * Diablo — openfpgaOS app
 *
 * Build:   make
 * Copy:  make copy
 * Test: make test
 */

#include "of.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    of_video_init();

    /* Set up a gradient palette */
    for (int i = 0; i < 256; i++)
        of_video_palette(i, (i << 16) | ((255 - i) << 8) | 128);

    /* Draw something */
    uint8_t *fb = of_video_surface();
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++)
            fb[y * 320 + x] = x ^ y;

    of_video_flip();

    printf("Hello from Diablo!\n");

    /* Main loop */
    while (1) {
        of_input_poll();

        if (of_btn_pressed(OF_BTN_A)) {
            /* A button pressed */
        }

        usleep(16000);  /* ~60 fps */
    }
}
