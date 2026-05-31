/*
 * of_sdl2.c -- SDL2 backend for openfpgaOS Application API
 *
 * Implements the full of.h API using SDL2 so games can be built
 * and tested on a PC (Linux/macOS/Windows).
 *
 * Build: cc -DOF_PC app.c of_sdl2.c $(sdl2-config --cflags --libs) -lm
 */

#ifndef OF_PC
#define OF_PC
#endif
#define OF_NO_COMPAT
#include "of.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ======================================================================
 * Internal state
 * ====================================================================== */

static SDL_Window   *g_window;
static SDL_Renderer *g_renderer;
static SDL_Texture  *g_texture;
static int           g_texture_w;
static int           g_texture_h;

/* Double-buffered framebuffers sized for the largest supported app mode. */
static uint8_t  g_fb[2][OF_VIDEO_MAX_FRAME_BYTES];
static int      g_draw_buf;    /* index of current draw buffer */
static int      g_color_mode;
static of_video_mode_t g_mode = {
    OF_SCREEN_W, OF_SCREEN_H, OF_SCREEN_W, OF_VIDEO_MODE_8BIT, 0
};
static size_t   g_frame_bytes = OF_FB_SIZE_8BIT;
static uint32_t g_present_count;
static uint64_t g_last_present_us;

/* Palette: 256 entries, 0x00RRGGBB */
static uint32_t g_palette[256];

/* Composited ARGB output (uploaded to texture) */
static uint32_t g_pixels[OF_VIDEO_MAX_WIDTH * OF_VIDEO_MAX_HEIGHT];

static const of_video_mode_t g_video_modes[] = {
    {256, 224, 0, OF_VIDEO_MODE_8BIT, 0},
    {256, 240, 0, OF_VIDEO_MODE_8BIT, 0},
    {320, 200, 0, OF_VIDEO_MODE_8BIT, 0},
    {320, 224, 0, OF_VIDEO_MODE_8BIT, 0},
    {320, 240, 0, OF_VIDEO_MODE_8BIT, 0},
    {320, 256, 0, OF_VIDEO_MODE_8BIT, 0},
    {320, 288, 0, OF_VIDEO_MODE_8BIT, 0},
    {400, 300, 0, OF_VIDEO_MODE_8BIT, 0},
    {512, 384, 0, OF_VIDEO_MODE_8BIT, 0},
    {640, 360, 0, OF_VIDEO_MODE_8BIT, 0},
    {640, 400, 0, OF_VIDEO_MODE_8BIT, 0},
    {640, 480, 0, OF_VIDEO_MODE_8BIT, 0},
    {800, 600, 0, OF_VIDEO_MODE_8BIT, 0},
};

/* ---- Input state ---- */
static of_input_state_t g_input[2];
static uint32_t g_prev_buttons[2];

/* ---- Audio state ---- */
static SDL_AudioDeviceID g_audio_dev;
#define AUDIO_BUF_SIZE 8192
static int16_t  g_audio_ring[AUDIO_BUF_SIZE];
static int      g_audio_read_pos;
static int      g_audio_write_pos;
static SDL_mutex *g_audio_mutex;

/* ---- Timer ---- */
static uint64_t g_start_us;

static const struct of_capabilities g_caps = {
    .magic = OF_CAPS_MAGIC,
    .version = OF_CAPS_VERSION,
    .fb_width = OF_SCREEN_W,
    .fb_height = OF_SCREEN_H,
    .fb_stride = OF_SCREEN_W,
    .fb_size = OF_FB_SIZE_8BIT,
    .hw_features = OF_HW_MIXER | OF_HW_SAVE_SLOTS,
    .mixer_voices = OF_MIXER_MAX_VOICES,
    .mixer_rate = OF_AUDIO_RATE,
    .platform_id = OF_PLATFORM_SIM,
    .cpu_freq_hz = 100000000u,
};

static uint64_t get_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

const struct of_capabilities *of_get_caps(void) {
    return &g_caps;
}

int of_has_feature(uint32_t feature) {
    return (g_caps.hw_features & feature) != 0;
}

/* ======================================================================
 * Video
 * ====================================================================== */

static uint32_t video_line_bytes(uint16_t width, uint8_t color_mode) {
    switch (color_mode) {
    case OF_VIDEO_MODE_4BIT:
        return ((uint32_t)width + 1u) >> 1;
    case OF_VIDEO_MODE_2BIT:
        return ((uint32_t)width + 3u) >> 2;
    case OF_VIDEO_MODE_RGB565:
    case OF_VIDEO_MODE_RGB555:
    case OF_VIDEO_MODE_RGBA5551:
        return (uint32_t)width * 2u;
    default:
        return width;
    }
}

static int normalize_mode(const of_video_mode_t *in, of_video_mode_t *out,
                          size_t *frame_bytes_out) {
    if (!in || in->width == 0 || in->height == 0)
        return -1;
    if (in->width > OF_VIDEO_MAX_WIDTH || in->height > OF_VIDEO_MAX_HEIGHT)
        return -1;
    if (in->color_mode > OF_VIDEO_MODE_RGBA5551)
        return -1;

    uint32_t line = (video_line_bytes(in->width, in->color_mode) + 1u) & ~1u;
    uint32_t stride = in->stride ? ((uint32_t)in->stride + 1u) & ~1u : line;
    if (stride < line || stride > OF_VIDEO_MAX_STRIDE)
        return -1;

    uint32_t frame_bytes = stride * (uint32_t)in->height;
    if (frame_bytes == 0 || frame_bytes > OF_VIDEO_MAX_FRAME_BYTES)
        return -1;

    if (out) {
        *out = *in;
        out->stride = (uint16_t)stride;
        out->reserved = 0;
    }
    if (frame_bytes_out)
        *frame_bytes_out = frame_bytes;
    return 0;
}

static int window_scale_for_mode(int w, int h) {
    if (w <= 400 && h <= 300)
        return 3;
    if (w <= 640 && h <= 480)
        return 2;
    return 1;
}

static void ensure_texture_for_mode(void) {
    if (!g_renderer)
        return;
    if (g_texture && g_texture_w == g_mode.width && g_texture_h == g_mode.height)
        return;

    if (g_texture)
        SDL_DestroyTexture(g_texture);
    g_texture = SDL_CreateTexture(g_renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        g_mode.width, g_mode.height);
    g_texture_w = g_mode.width;
    g_texture_h = g_mode.height;
    SDL_RenderSetLogicalSize(g_renderer, g_mode.width, g_mode.height);

    if (g_window) {
        int scale = window_scale_for_mode(g_mode.width, g_mode.height);
        SDL_SetWindowSize(g_window, g_mode.width * scale, g_mode.height * scale);
    }
}

static uint32_t rgb565_to_argb(uint16_t v) {
    uint32_t r = (uint32_t)((v >> 11) & 0x1F);
    uint32_t g = (uint32_t)((v >> 5) & 0x3F);
    uint32_t b = (uint32_t)(v & 0x1F);
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static uint32_t rgb555_to_argb(uint16_t v) {
    uint32_t r = (uint32_t)((v >> 10) & 0x1F);
    uint32_t g = (uint32_t)((v >> 5) & 0x1F);
    uint32_t b = (uint32_t)(v & 0x1F);
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static uint32_t rgba5551_to_argb(uint16_t v) {
    uint32_t r = (uint32_t)((v >> 11) & 0x1F);
    uint32_t g = (uint32_t)((v >> 6) & 0x1F);
    uint32_t b = (uint32_t)((v >> 1) & 0x1F);
    uint32_t a = (v & 1) ? 0xFF000000u : 0x00000000u;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return a | (r << 16) | (g << 8) | b;
}

static uint8_t framebuffer_index_at(const uint8_t *fb, int x, int y) {
    switch (g_color_mode) {
    case OF_VIDEO_MODE_4BIT: {
        uint8_t packed = fb[(uint32_t)y * g_mode.stride + (x >> 1)];
        return (x & 1) ? (packed >> 4) : (packed & 0x0F);
    }
    case OF_VIDEO_MODE_2BIT: {
        uint8_t packed = fb[(uint32_t)y * g_mode.stride + (x >> 2)];
        return (packed >> ((x & 3) * 2)) & 0x03;
    }
    default:
        return fb[(uint32_t)y * g_mode.stride + x];
    }
}

/* Composite the framebuffer and upload to texture */
static void composite_and_present(void) {
    int disp = g_draw_buf ^ 1;  /* display buffer is the one we just flipped from */
    const uint8_t *fb = g_fb[disp];
    const uint16_t *fb16 = (const uint16_t *)fb;
    int w = (int)g_mode.width;
    int h = (int)g_mode.height;
    int stride = (int)g_mode.stride;

    ensure_texture_for_mode();

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t color = 0xFF000000;  /* opaque black */

            if (g_color_mode == OF_VIDEO_MODE_RGB565) {
                color = rgb565_to_argb(fb16[(uint32_t)y * (stride >> 1) + x]);
            } else if (g_color_mode == OF_VIDEO_MODE_RGB555) {
                color = rgb555_to_argb(fb16[(uint32_t)y * (stride >> 1) + x]);
            } else if (g_color_mode == OF_VIDEO_MODE_RGBA5551) {
                color = rgba5551_to_argb(fb16[(uint32_t)y * (stride >> 1) + x]);
            } else {
                uint8_t fb_idx = framebuffer_index_at(fb, x, y);
                if (fb_idx && g_palette[fb_idx])
                    color = g_palette[fb_idx] | 0xFF000000;
            }

            g_pixels[(uint32_t)y * w + x] = color;
        }
    }

    SDL_UpdateTexture(g_texture, NULL, g_pixels, w * 4);
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
    SDL_RenderPresent(g_renderer);
    g_present_count++;
    g_last_present_us = get_us();
}

void of_video_init(void) {
    if (!g_window) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
            fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
            exit(1);
        }
        g_start_us = get_us();

        g_window = SDL_CreateWindow("openfpgaOS",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            OF_SCREEN_W * 3, OF_SCREEN_H * 3,
            SDL_WINDOW_RESIZABLE);
        g_renderer = SDL_CreateRenderer(g_window, -1,
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    }

    of_video_mode_t default_mode = {
        OF_SCREEN_W, OF_SCREEN_H, 0, OF_VIDEO_MODE_8BIT, 0
    };
    normalize_mode(&default_mode, &g_mode, &g_frame_bytes);
    ensure_texture_for_mode();

    memset(g_fb[0], 0, g_frame_bytes);
    memset(g_fb[1], 0, g_frame_bytes);
    g_draw_buf = 0;
    g_color_mode = OF_VIDEO_MODE_8BIT;
    memset(g_palette, 0, sizeof(g_palette));
}

uint8_t *of_video_surface(void) {
    return g_fb[g_draw_buf];
}

void of_video_flip(void) {
    composite_and_present();
    g_draw_buf ^= 1;
}

void of_video_wait_flip(void) {
    /* vsync is handled by SDL_RENDERER_PRESENTVSYNC */
}

int of_video_acquire_next(int just_flipped_idx, uint32_t fence_token) {
    (void)just_flipped_idx;
    (void)fence_token;
    return g_draw_buf;
}

uint8_t *of_video_buffer_addr(int idx) {
    if (idx < 0)
        idx = g_draw_buf;
    return g_fb[idx & 1];
}

int of_video_set_mode(const of_video_mode_t *mode) {
    of_video_mode_t normalized;
    size_t frame_bytes;
    if (normalize_mode(mode, &normalized, &frame_bytes) < 0)
        return -1;

    if (!g_window)
        of_video_init();

    g_mode = normalized;
    g_color_mode = normalized.color_mode;
    g_frame_bytes = frame_bytes;
    g_draw_buf = 0;
    memset(g_fb[0], 0, g_frame_bytes);
    memset(g_fb[1], 0, g_frame_bytes);
    ensure_texture_for_mode();
    return 0;
}

void of_video_get_mode(of_video_mode_t *out) {
    if (out)
        *out = g_mode;
}

int of_video_get_mode_count(void) {
    return (int)(sizeof(g_video_modes) / sizeof(g_video_modes[0]));
}

int of_video_get_mode_info(int index, of_video_mode_t *out) {
    if (!out || index < 0 || index >= of_video_get_mode_count())
        return -1;
    return normalize_mode(&g_video_modes[index], out, NULL);
}

void of_video_get_caps(of_video_caps_t *out) {
    __of_video_default_caps(out);
}

int of_video_check_mode(const of_video_mode_t *mode,
                        of_video_mode_t *normalized) {
    return normalize_mode(mode, normalized, NULL);
}

void of_video_clear(uint8_t color) {
    memset(g_fb[0], color, g_frame_bytes);
    memset(g_fb[1], color, g_frame_bytes);
}

void of_video_palette(uint8_t index, uint32_t rgb) {
    g_palette[index] = rgb;
}

void of_video_palette_bulk(const uint32_t *pal, int count) {
    if (count > 256) count = 256;
    memcpy(g_palette, pal, count * sizeof(uint32_t));
}

void of_video_flush(void) {
    /* no-op on PC */
}

void of_video_set_display_mode(int mode) {
    (void)mode;
}

void of_video_set_color_mode(int mode) {
    if (mode < OF_VIDEO_MODE_8BIT || mode > OF_VIDEO_MODE_RGBA5551)
        mode = OF_VIDEO_MODE_8BIT;
    of_video_mode_t next = g_mode;
    next.color_mode = (uint8_t)mode;
    next.stride = 0;
    (void)of_video_set_mode(&next);
}

void of_video_get_timing(of_video_timing_t *out) {
    if (!out) return;
    out->vblank_count = g_present_count;
    out->present_count = g_present_count;
    out->last_presented_idx = (uint32_t)(g_draw_buf ^ 1);
    out->reserved = 0;
    out->last_vblank_us = g_last_present_us;
    out->last_flip_presented_us = g_last_present_us;
}

uint64_t of_video_last_vblank_us(void) {
    return g_last_present_us;
}

uint64_t of_video_last_flip_presented_us(void) {
    return g_last_present_us;
}

uint32_t of_video_vblank_count(void) {
    return g_present_count;
}

/* ======================================================================
 * Timer
 *
 * of_timer.h declares of_time_us / of_time_ms as plain externs in the
 * OF_PC build (no inline syscalls). The PC backend implements them
 * here on top of SDL_GetPerformanceCounter / SDL_GetTicks. Both are
 * monotonic and free-running, matching the on-target semantics.
 * ====================================================================== */

unsigned int of_time_us(void) {
    static Uint64 freq;
    if (!freq) freq = SDL_GetPerformanceFrequency();
    Uint64 ticks = SDL_GetPerformanceCounter();
    /* Scale to microseconds. Wraps in ~71 minutes (uint32 us), same
     * cadence as the hardware free-running timer. */
    return (unsigned int)((ticks * 1000000ULL) / freq);
}

unsigned int of_time_ms(void) {
    return (unsigned int)SDL_GetTicks();
}

/* ======================================================================
 * Input
 * ====================================================================== */

/* SDL scancode -> button mask mapping */
static uint32_t key_to_btn(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_UP:     return OF_BTN_UP;
        case SDL_SCANCODE_DOWN:   return OF_BTN_DOWN;
        case SDL_SCANCODE_LEFT:   return OF_BTN_LEFT;
        case SDL_SCANCODE_RIGHT:  return OF_BTN_RIGHT;
        case SDL_SCANCODE_Z:      return OF_BTN_A;
        case SDL_SCANCODE_X:      return OF_BTN_B;
        case SDL_SCANCODE_A:      return OF_BTN_X;
        case SDL_SCANCODE_S:      return OF_BTN_Y;
        case SDL_SCANCODE_Q:      return OF_BTN_L1;
        case SDL_SCANCODE_W:      return OF_BTN_R1;
        case SDL_SCANCODE_1:      return OF_BTN_L2;
        case SDL_SCANCODE_2:      return OF_BTN_R2;
        case SDL_SCANCODE_RSHIFT: return OF_BTN_SELECT;
        case SDL_SCANCODE_RETURN: return OF_BTN_START;
        default: return 0;
    }
}

void of_input_poll(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            SDL_Quit();
            exit(0);
            break;
        case SDL_KEYDOWN:
            if (!ev.key.repeat)
                g_input[0].buttons |= key_to_btn(ev.key.keysym.scancode);
            break;
        case SDL_KEYUP:
            g_input[0].buttons &= ~key_to_btn(ev.key.keysym.scancode);
            break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            /* PC backend folds all SDL controllers into player 0; the
             * P2 button query stubs above always read empty state. */
            int p = 0;
            uint32_t mask = 0;
            switch (ev.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP:    mask = OF_BTN_UP; break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  mask = OF_BTN_DOWN; break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  mask = OF_BTN_LEFT; break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: mask = OF_BTN_RIGHT; break;
                case SDL_CONTROLLER_BUTTON_A:          mask = OF_BTN_A; break;
                case SDL_CONTROLLER_BUTTON_B:          mask = OF_BTN_B; break;
                case SDL_CONTROLLER_BUTTON_X:          mask = OF_BTN_X; break;
                case SDL_CONTROLLER_BUTTON_Y:          mask = OF_BTN_Y; break;
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  mask = OF_BTN_L1; break;
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: mask = OF_BTN_R1; break;
                case SDL_CONTROLLER_BUTTON_BACK:       mask = OF_BTN_SELECT; break;
                case SDL_CONTROLLER_BUTTON_START:      mask = OF_BTN_START; break;
                default: break;
            }
            if (ev.type == SDL_CONTROLLERBUTTONDOWN)
                g_input[p].buttons |= mask;
            else
                g_input[p].buttons &= ~mask;
            break;
        }
        case SDL_CONTROLLERDEVICEADDED:
            SDL_GameControllerOpen(ev.cdevice.which);
            break;
        }
    }

    /* Compute pressed/released edges */
    for (int p = 0; p < 2; p++) {
        g_input[p].buttons_pressed  = g_input[p].buttons & ~g_prev_buttons[p];
        g_input[p].buttons_released = ~g_input[p].buttons & g_prev_buttons[p];
        g_prev_buttons[p] = g_input[p].buttons;
    }
}

int of_btn(uint32_t mask)          { return (g_input[0].buttons & mask) != 0; }
int of_btn_pressed(uint32_t mask)  { return (g_input[0].buttons_pressed & mask) != 0; }
int of_btn_released(uint32_t mask) { return (g_input[0].buttons_released & mask) != 0; }
int of_btn_p2(uint32_t mask)           { return (g_input[1].buttons & mask) != 0; }
int of_btn_pressed_p2(uint32_t mask)   { return (g_input[1].buttons_pressed & mask) != 0; }
int of_btn_released_p2(uint32_t mask)  { return (g_input[1].buttons_released & mask) != 0; }

uint32_t of_input_state(int player, of_input_state_t *state) {
    if (player >= 0 && player < 2 && state)
        *state = g_input[player];
    return 0;
}

/* Keyboard/mouse/deadzone stubs — declared as plain externs in
 * of_input.h's OF_PC branch.  The PC backend doesn't expose dock
 * peripherals through SDL, so return empty state and accept the
 * deadzone for API compatibility. */
void of_input_keyboard_state(of_keyboard_state_t *state) {
    if (state) memset(state, 0, sizeof(*state));
}

void of_input_mouse_state(of_mouse_state_t *state) {
    if (state) memset(state, 0, sizeof(*state));
}

void of_input_set_deadzone(int16_t deadzone) {
    (void)deadzone;
}

/* ======================================================================
 * Audio
 * ====================================================================== */

static void audio_callback(void *userdata, uint8_t *stream, int len) {
    (void)userdata;
    int16_t *out = (int16_t *)stream;
    int samples = len / 2;

    SDL_LockMutex(g_audio_mutex);
    for (int i = 0; i < samples; i++) {
        if (g_audio_read_pos != g_audio_write_pos) {
            out[i] = g_audio_ring[g_audio_read_pos];
            g_audio_read_pos = (g_audio_read_pos + 1) % AUDIO_BUF_SIZE;
        } else {
            out[i] = 0;
        }
    }
    SDL_UnlockMutex(g_audio_mutex);
}

void of_audio_init(void) {
    if (g_audio_dev) return;

    g_audio_mutex = SDL_CreateMutex();

    SDL_AudioSpec want = {0}, have;
    want.freq = OF_AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_callback;

    g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_audio_dev)
        SDL_PauseAudioDevice(g_audio_dev, 0);
}

int of_audio_write(const int16_t *samples, int count) {
    int written = 0;
    SDL_LockMutex(g_audio_mutex);
    for (int i = 0; i < count * 2; i++) {
        int next = (g_audio_write_pos + 1) % AUDIO_BUF_SIZE;
        if (next == g_audio_read_pos) break;
        g_audio_ring[g_audio_write_pos] = samples[i];
        g_audio_write_pos = next;
        if (i & 1) written++;
    }
    SDL_UnlockMutex(g_audio_mutex);
    return written;
}

int of_audio_free(void) {
    SDL_LockMutex(g_audio_mutex);
    int used = (g_audio_write_pos - g_audio_read_pos + AUDIO_BUF_SIZE) % AUDIO_BUF_SIZE;
    SDL_UnlockMutex(g_audio_mutex);
    return (AUDIO_BUF_SIZE - 1 - used) / 2;
}

/* ======================================================================
 * Timer
 * ====================================================================== */


/* ======================================================================
 * File I/O
 *
 * On PC, data slot files are read from ./data/<slot_id>.bin
 * Set OF_DATA_DIR env var to override.
 * ====================================================================== */

int of_file_read(uint32_t slot_id, uint32_t offset, void *dest, uint32_t length) {
    char path[512];
    const char *dir = getenv("OF_DATA_DIR");
    if (!dir) dir = "data";
    snprintf(path, sizeof(path), "%s/%u.bin", dir, slot_id);

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, offset, SEEK_SET);
    int n = fread(dest, 1, length, f);
    fclose(f);
    return n == (int)length ? 0 : -1;
}

long of_file_size(uint32_t slot_id) {
    char path[512];
    const char *dir = getenv("OF_DATA_DIR");
    if (!dir) dir = "data";
    snprintf(path, sizeof(path), "%s/%u.bin", dir, slot_id);

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return sz;
}

/* ======================================================================
 * Link Cable (stubs)
 * ====================================================================== */

int      of_link_send(uint32_t data)   { (void)data; return -1; }
int      of_link_recv(uint32_t *data)  { (void)data; return -1; }
uint32_t of_link_status(void)          { return 0; }


/* ======================================================================
 * Analogizer (stubs)
 * ====================================================================== */

int of_analogizer_enabled(void)                     { return 0; }
int of_analogizer_state(of_analogizer_state_t *state) {
    if (state) memset(state, 0, sizeof(*state));
    return 0;
}

/* ======================================================================
 * System
 * ====================================================================== */

void of_exit(void) {
    if (g_audio_dev) SDL_CloseAudioDevice(g_audio_dev);
    if (g_texture)  SDL_DestroyTexture(g_texture);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window)   SDL_DestroyWindow(g_window);
    SDL_Quit();
    exit(0);
}
