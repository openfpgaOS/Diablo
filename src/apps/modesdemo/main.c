//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * modesdemo — enumerate and switch dynamic framebuffer video modes
 *
 * Canonical example of the runtime mode API:
 *   - of_video_get_mode_count() / of_video_get_mode_info() to discover
 *     the framebuffer modes the OS advertises
 *   - of_video_set_mode() to switch the active source framebuffer
 *   - of_video_get_mode() to read back the normalised stride/colour mode
 *
 * For each mode it draws a test pattern sized to the mode's exact
 * width/height/stride — a rainbow gradient, a 1px bright border tracing
 * the full extent, and corner-to-corner diagonals — then flips. The
 * border + diagonals make the active resolution (and any stride/edge
 * error) obvious on hardware.
 *
 * The active mode is labelled on-screen with a tiny built-in font drawn
 * straight into the framebuffer and integer-scaled by the mode's width,
 * so the text grows on higher-resolution modes (and the same info is
 * logged to UART). White-on-black keeps it legible over the gradient.
 *
 * Controls:
 *   A / RIGHT   next mode
 *   B / LEFT    previous mode
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "of.h"

#define MAX_MODES 32
#define WHITE     255   /* palette index forced to white for border, diagonals, text */
#define BLACK     254   /* palette index forced to black for the text panel */

/* ── palette ───────────────────────────────────────────────────────── */

static uint32_t clip_rgb(int r, int g, int b) {
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* 256-entry rainbow ramp so the gradient reads clearly in 8-bit indexed,
 * with the top entry forced to white for the border/diagonals. */
static void setup_palette(void) {
    uint32_t pal[256];
    for (int i = 0; i < 256; i++) {
        int r, g, b;
        if (i < 43)       { r = 255;            g = i * 6;           b = 0;             }
        else if (i < 86)  { r = 255 - (i-43)*6; g = 255;             b = 0;             }
        else if (i < 128) { r = 0;              g = 255;             b = (i-86)*6;      }
        else if (i < 171) { r = 0;              g = 255 - (i-128)*6; b = 255;           }
        else if (i < 214) { r = (i-171)*6;      g = 0;               b = 255;           }
        else              { r = 255;            g = 0;               b = 255 - (i-214)*6; }
        pal[i] = clip_rgb(r, g, b);
    }
    pal[BLACK] = 0x00000000;
    pal[WHITE] = 0x00FFFFFF;
    of_video_palette_bulk(pal, 256);
}

/* ── test pattern ──────────────────────────────────────────────────── */

static void fill_gradient(uint8_t *fb, int w, int h, int stride) {
    for (int y = 0; y < h; y++) {
        uint8_t *row = fb + (uint32_t)y * stride;
        for (int x = 0; x < w; x++)
            row[x] = (uint8_t)((x * 256) / w);
    }
}

static void draw_border(uint8_t *fb, int w, int h, int stride) {
    memset(fb, WHITE, w);                              /* top    */
    memset(fb + (uint32_t)(h - 1) * stride, WHITE, w); /* bottom */
    for (int y = 0; y < h; y++) {
        uint8_t *row = fb + (uint32_t)y * stride;
        row[0]     = WHITE;                            /* left   */
        row[w - 1] = WHITE;                            /* right  */
    }
}

static void draw_diagonals(uint8_t *fb, int w, int h, int stride) {
    for (int x = 0; x < w; x++) {
        int y = x * (h - 1) / (w - 1);
        fb[(uint32_t)y * stride + x]           = WHITE;
        fb[(uint32_t)(h - 1 - y) * stride + x] = WHITE;
    }
}

/* Draw a test pattern sized to the active mode. Every mode the OS
 * enumerates today is 8-bit indexed; the per-pixel fallback keeps the
 * demo safe (no out-of-stride writes) if a non-8-bit mode ever appears. */
static void draw_pattern(const of_video_mode_t *m) {
    int w = m->width, h = m->height, stride = m->stride;

    if (m->color_mode != OF_VIDEO_MODE_8BIT) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                of_video_pixel(x, y, (uint8_t)(((x + y) >> 1) & 0xFF));
        return;
    }

    uint8_t *fb = of_video_surface();
    fill_gradient(fb, w, h, stride);    /* rainbow sweep marks the width */
    draw_border(fb, w, h, stride);      /* 1px frame traces the extent   */
    draw_diagonals(fb, w, h, stride);   /* centre cross-check / aspect   */
}

/* ── on-screen text ────────────────────────────────────────────────── */
/* A tiny 5×7 font, blitted into the framebuffer at an integer scale so
 * the label grows with the mode's resolution. */

typedef struct { char c; uint8_t row[7]; } glyph_t;

/* Only the glyphs the labels use. Each row is 5 bits (bit 4 = leftmost);
 * the set bits spell the character so the table reads as the shapes.
 * Lowercase folds to uppercase at lookup; unknown chars render blank. */
static const glyph_t FONT[] = {
    {' ', {0,0,0,0,0,0,0}},
    {'/', {0b00001,0b00010,0b00010,0b00100,0b01000,0b01000,0b10000}},
    {':', {0b00000,0b00100,0b00100,0b00000,0b00100,0b00100,0b00000}},
    {'0', {0b01110,0b10001,0b10011,0b10101,0b11001,0b10001,0b01110}},
    {'1', {0b00100,0b01100,0b00100,0b00100,0b00100,0b00100,0b01110}},
    {'2', {0b01110,0b10001,0b00001,0b00010,0b00100,0b01000,0b11111}},
    {'3', {0b11111,0b00010,0b00100,0b00010,0b00001,0b10001,0b01110}},
    {'4', {0b00010,0b00110,0b01010,0b10010,0b11111,0b00010,0b00010}},
    {'5', {0b11111,0b10000,0b11110,0b00001,0b00001,0b10001,0b01110}},
    {'6', {0b00110,0b01000,0b10000,0b11110,0b10001,0b10001,0b01110}},
    {'7', {0b11111,0b00001,0b00010,0b00100,0b01000,0b01000,0b01000}},
    {'8', {0b01110,0b10001,0b10001,0b01110,0b10001,0b10001,0b01110}},
    {'9', {0b01110,0b10001,0b10001,0b01111,0b00001,0b00010,0b01100}},
    {'A', {0b01110,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001}},
    {'B', {0b11110,0b10001,0b10001,0b11110,0b10001,0b10001,0b11110}},
    {'D', {0b11100,0b10010,0b10001,0b10001,0b10001,0b10010,0b11100}},
    {'E', {0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b11111}},
    {'G', {0b01110,0b10001,0b10000,0b10111,0b10001,0b10001,0b01111}},
    {'I', {0b01110,0b00100,0b00100,0b00100,0b00100,0b00100,0b01110}},
    {'M', {0b10001,0b11011,0b10101,0b10101,0b10001,0b10001,0b10001}},
    {'N', {0b10001,0b11001,0b10101,0b10011,0b10001,0b10001,0b10001}},
    {'O', {0b01110,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110}},
    {'P', {0b11110,0b10001,0b10001,0b11110,0b10000,0b10000,0b10000}},
    {'R', {0b11110,0b10001,0b10001,0b11110,0b10100,0b10010,0b10001}},
    {'S', {0b01111,0b10000,0b10000,0b01110,0b00001,0b00001,0b11110}},
    {'T', {0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b00100}},
    {'V', {0b10001,0b10001,0b10001,0b10001,0b10001,0b01010,0b00100}},
    {'X', {0b10001,0b10001,0b01010,0b00100,0b01010,0b10001,0b10001}},
};
#define FONT_COUNT ((int)(sizeof(FONT) / sizeof(FONT[0])))

#define GLYPH_W   5
#define GLYPH_H   7
#define GLYPH_ADV 6   /* 5px glyph + 1px gap */

static const uint8_t *glyph_rows(char c) {
    if (c >= 'a' && c <= 'z')
        c -= 'a' - 'A';
    for (int i = 0; i < FONT_COUNT; i++)
        if (FONT[i].c == c)
            return FONT[i].row;
    return NULL;
}

/* Blit one glyph as scale×scale blocks, clipped to the framebuffer. */
static void draw_char(uint8_t *fb, int stride, int w, int h,
                      int x, int y, char c, int scale, uint8_t color) {
    const uint8_t *g = glyph_rows(c);
    if (!g)
        return;
    for (int gy = 0; gy < GLYPH_H; gy++) {
        for (int gx = 0; gx < GLYPH_W; gx++) {
            if (!((g[gy] >> (GLYPH_W - 1 - gx)) & 1))
                continue;
            for (int dy = 0; dy < scale; dy++) {
                int py = y + gy * scale + dy;
                if ((unsigned)py >= (unsigned)h)
                    continue;
                uint8_t *row = fb + (uint32_t)py * stride;
                for (int dx = 0; dx < scale; dx++) {
                    int px = x + gx * scale + dx;
                    if ((unsigned)px < (unsigned)w)
                        row[px] = color;
                }
            }
        }
    }
}

static void draw_text(uint8_t *fb, int stride, int w, int h,
                      int x, int y, const char *s, int scale, uint8_t color) {
    for (; *s; s++, x += GLYPH_ADV * scale)
        draw_char(fb, stride, w, h, x, y, *s, scale, color);
}

static int text_px(const char *s, int scale) {
    return (int)strlen(s) * GLYPH_ADV * scale;
}

/* ── mode list ─────────────────────────────────────────────────────── */

static const char *color_mode_name(uint8_t cm) {
    switch (cm) {
    case OF_VIDEO_MODE_8BIT:     return "8bpp indexed";
    case OF_VIDEO_MODE_4BIT:     return "4bpp indexed";
    case OF_VIDEO_MODE_2BIT:     return "2bpp indexed";
    case OF_VIDEO_MODE_RGB565:   return "RGB565";
    case OF_VIDEO_MODE_RGB555:   return "RGB555";
    case OF_VIDEO_MODE_RGBA5551: return "RGBA5551";
    default:                     return "?";
    }
}

/* Snapshot the modes the OS advertises into `out`, keeping only those
 * that normalise cleanly (of_video_get_mode_info shares of_video_set_mode's
 * check, so anything kept here is guaranteed to set successfully).
 * Returns how many were stored. */
static int collect_modes(of_video_mode_t *out, int max) {
    int count = of_video_get_mode_count();
    if (count > max)
        count = max;

    int n = 0;
    for (int i = 0; i < count; i++)
        if (of_video_get_mode_info(i, &out[n]) == 0)
            n++;
    return n;
}

/* Index of the first width×height match, or -1 if absent. */
static int find_mode(const of_video_mode_t *modes, int n, int w, int h) {
    for (int i = 0; i < n; i++)
        if (modes[i].width == w && modes[i].height == h)
            return i;
    return -1;
}

static void print_mode_table(const of_video_mode_t *modes, int n) {
    printf("\n=== Video Modes Demo ===\n");
    printf("OS advertises %d usable mode(s):\n", n);
    for (int i = 0; i < n; i++) {
        int is_288 = (modes[i].width == 320 && modes[i].height == 288);
        printf("  [%2d] %4ux%-4u stride=%-4u %s%s\n",
               i, modes[i].width, modes[i].height, modes[i].stride,
               color_mode_name(modes[i].color_mode),
               is_288 ? "   <- 320x288" : "");
    }
    printf("Controls: A/RIGHT = next   B/LEFT = prev\n\n");
}

/* Draw the active-mode label into the framebuffer as white text on a black
 * panel, top-left. The font scale tracks the mode width (320→2, 640→4,
 * 800→5) so the text grows on higher-resolution modes. */
static void draw_label(const of_video_mode_t *m, int n, int i) {
    char lines[4][32];
    snprintf(lines[0], sizeof lines[0], "MODE %d/%d", i + 1, n);
    snprintf(lines[1], sizeof lines[1], "%uX%u", m->width, m->height);
    snprintf(lines[2], sizeof lines[2], "STRIDE %u  %s",
             m->stride, color_mode_name(m->color_mode));
    snprintf(lines[3], sizeof lines[3], "A:NEXT  B:PREV");

    int w = m->width, h = m->height, stride = m->stride;
    int scale = w / 160;
    if (scale < 1)
        scale = 1;

    int line_h = (GLYPH_H + 2) * scale;
    int pad    = 2 * scale;
    int x0     = 4 * scale;
    int y0     = 4 * scale;

    int panel_w = 0;
    for (int k = 0; k < 4; k++) {
        int tw = text_px(lines[k], scale);
        if (tw > panel_w)
            panel_w = tw;
    }

    uint8_t *fb = of_video_surface();
    of_fill_rect(x0 - pad, y0 - pad, panel_w + 2 * pad, 4 * line_h + 2 * pad, BLACK);
    for (int k = 0; k < 4; k++)
        draw_text(fb, stride, w, h, x0, y0 + k * line_h, lines[k], scale, WHITE);
}

/* Switch to modes[i], draw the pattern + scaled label at the mode's real
 * geometry, present it, and log the mode to UART. of_video_set_mode clears
 * the buffers, so we draw and flip straight away — no extra blanking. */
static void show_mode(of_video_mode_t *modes, int n, int i) {
    of_video_mode_t *m = &modes[i];

    if (of_video_set_mode(m) != 0) {
        printf("[%2d/%d] set_mode %ux%u FAILED\n", i, n, m->width, m->height);
        return;
    }
    of_video_get_mode(m);   /* read back the normalised geometry */

    draw_pattern(m);
    if (m->color_mode == OF_VIDEO_MODE_8BIT)
        draw_label(m, n, i);
    of_video_flip();

    printf("[%2d/%d] active: %ux%u stride=%u %s\n",
           i, n, m->width, m->height, m->stride, color_mode_name(m->color_mode));
}

/* One input poll → navigation step: +1 next, -1 previous, 0 no change. */
static int read_nav(void) {
    of_input_poll();
    if (of_btn_pressed(OF_BTN_A) || of_btn_pressed(OF_BTN_RIGHT)) return +1;
    if (of_btn_pressed(OF_BTN_B) || of_btn_pressed(OF_BTN_LEFT))  return -1;
    return 0;
}

/* ── main ──────────────────────────────────────────────────────────── */

int main(void) {
    of_video_init();
    setup_palette();

    of_video_mode_t modes[MAX_MODES];
    int n = collect_modes(modes, MAX_MODES);
    if (n == 0) {
        printf("modesdemo: OS reports no usable video modes\n");
        return 1;
    }
    print_mode_table(modes, n);

    /* Start on 320x288 when present so it's in view immediately. */
    int cur = find_mode(modes, n, 320, 288);
    if (cur < 0)
        cur = 0;
    show_mode(modes, n, cur);

    for (;;) {
        int nav = read_nav();
        if (nav) {
            cur = (cur + nav + n) % n;
            show_mode(modes, n, cur);
        }
        usleep(16 * 1000);
    }
    return 0;
}
