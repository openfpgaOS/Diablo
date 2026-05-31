/*
 * SDL2 shim for openfpgaOS
 *
 * Minimal SDL2 implementation using of_* syscalls.
 * On PC builds, this header is never used — the real SDL2 is linked.
 *
 * Video model: requested SDL window sizes are first offered to the OS as
 * native framebuffer modes. If unsupported, the shim keeps a logical
 * software surface and scales it to the active framebuffer on present.
 *
 * Covers: video (8-bit indexed surface), input, audio, timer.
 */

#ifndef _OF_SDL2_SHIM_H
#define _OF_SDL2_SHIM_H

#ifdef OF_PC
#include_next <SDL2/SDL.h>
#else

#include "of.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

/* ======================================================================
 * Version / Constants
 * ====================================================================== */

#define SDL_MAJOR_VERSION 2
#define SDL_MINOR_VERSION 0
#define SDL_PATCHLEVEL    0
#define SDL_VERSIONNUM(X, Y, Z) (((X) * 1000) + ((Y) * 100) + (Z))
#define SDL_VERSION_ATLEAST(X, Y, Z) \
    (SDL_VERSIONNUM(SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL) >= SDL_VERSIONNUM((X), (Y), (Z)))

#define SDL_INIT_VIDEO          0x00000020
#define SDL_INIT_AUDIO          0x00000010
#define SDL_INIT_TIMER          0x00000001
#define SDL_INIT_JOYSTICK       0x00000200
#define SDL_INIT_EVENTS         0x00004000
#define SDL_INIT_GAMECONTROLLER 0x00002000
#define SDL_INIT_EVERYTHING     0x0000FFFF

#define SDL_SWSURFACE    0x00000000
#define SDL_HWSURFACE    0x00000001
#define SDL_HWPALETTE    0x00000008
#define SDL_DOUBLEBUF    0x40000000
#define SDL_SRCCOLORKEY  0x00001000
#define SDL_PHYSPAL      1
#define SDL_LOGPAL       2

#define SDL_WINDOW_SHOWN        0x00000004
#define SDL_WINDOW_RESIZABLE    0x00000020
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x00001001
#define SDL_WINDOWPOS_CENTERED  0x2FFF0000
#define SDL_WINDOWPOS_UNDEFINED 0x1FFF0000
#define SDL_WINDOWPOS_UNDEFINED_DISPLAY(X) SDL_WINDOWPOS_UNDEFINED

#define SDL_RENDERER_ACCELERATED    0x00000002
#define SDL_RENDERER_PRESENTVSYNC   0x00000004
#define SDL_RENDERER_TARGETTEXTURE  0x00000008

#define SDL_PIXELFORMAT_ARGB8888    0x16362004
#define SDL_PIXELFORMAT_ARGB2101010 0x16372004
#define SDL_PIXELFORMAT_ARGB1555    0x15331002
#define SDL_PIXELFORMAT_RGBA32      0x16362004
#define SDL_PIXELFORMAT_RGB888      0x16161804
#define SDL_PIXELFORMAT_RGB565      0x15151002
#define SDL_PIXELFORMAT_INDEX8      0x13000001
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_TEXTUREACCESS_TARGET    2

#define SDL_QUIT            0x100
#define SDL_KEYDOWN         0x300
#define SDL_KEYUP           0x301
#define SDL_TEXTINPUT       0x303
#define SDL_MOUSEMOTION     0x400
#define SDL_MOUSEBUTTONDOWN 0x401
#define SDL_MOUSEBUTTONUP   0x402
#define SDL_MOUSEWHEEL      0x403
#define SDL_CONTROLLERBUTTONDOWN  0x650
#define SDL_CONTROLLERBUTTONUP    0x651
#define SDL_CONTROLLERDEVICEADDED 0x654

#define SDL_DISABLE 0
#define SDL_ENABLE  1
#define SDL_IGNORE  0

#define SDL_BUTTON_LEFT      1
#define SDL_BUTTON_MIDDLE    2
#define SDL_BUTTON_RIGHT     3
#define SDL_BUTTON_WHEELUP   4
#define SDL_BUTTON_WHEELDOWN 5
#define SDL_BUTTON(X)        (1u << ((X) - 1))

#define SDL_HAT_CENTERED 0x00
#define SDL_HAT_UP       0x01
#define SDL_HAT_RIGHT    0x02
#define SDL_HAT_DOWN     0x04
#define SDL_HAT_LEFT     0x08

#define KMOD_NONE 0x0000
#define KMOD_NUM  0x1000
#define KMOD_CAPS 0x2000

#define AUDIO_S16SYS    0x8010
#define AUDIO_S16       0x8010
#define AUDIO_F32SYS    0x8120
#define AUDIO_U8        0x0008

#define SDL_LIL_ENDIAN  1234
#define SDL_BIG_ENDIAN  4321
#define SDL_BYTEORDER   SDL_LIL_ENDIAN

#ifndef SDL_bool
#define SDL_bool int
#define SDL_FALSE 0
#define SDL_TRUE 1
#endif

typedef uint8_t  Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef int16_t  Sint16;
typedef int32_t  Sint32;
typedef int64_t  Sint64;
typedef uint16_t SDL_Keymod;

#define SDL_malloc  malloc
#define SDL_free    free
#define SDL_memset  memset
#define SDL_memcpy  memcpy

/* ======================================================================
 * Types
 * ====================================================================== */

typedef struct { int x, y, w, h; } SDL_Rect;
typedef struct { uint8_t r, g, b, a; } SDL_Color;
typedef struct { uint8_t major, minor, patch; } SDL_version;

typedef struct {
    int ncolors;
    SDL_Color colors[256];
} SDL_Palette;

typedef struct {
    uint32_t format;
    SDL_Palette *palette;
    uint8_t BitsPerPixel;
    uint8_t BytesPerPixel;
    uint32_t Rmask;
    uint32_t Gmask;
    uint32_t Bmask;
    uint32_t Amask;
} SDL_PixelFormat;

typedef struct {
    uint32_t flags;
    SDL_PixelFormat *format;
    int w, h;
    int pitch;
    void *pixels;
    SDL_Rect clip_rect;
    int locked;
} SDL_Surface;

typedef struct { int w, h; uint32_t flags; } SDL_Window;
typedef struct { int unused; } SDL_Renderer;
typedef struct { int w, h, pitch; void *pixels; } SDL_Texture;
typedef struct {
    uint32_t format;
    int w;
    int h;
    int refresh_rate;
    void *driverdata;
} SDL_DisplayMode;

typedef enum {
    SDL_SCANCODE_UNKNOWN = 0,
    SDL_SCANCODE_A = 4, SDL_SCANCODE_B, SDL_SCANCODE_C, SDL_SCANCODE_D,
    SDL_SCANCODE_E, SDL_SCANCODE_F, SDL_SCANCODE_G, SDL_SCANCODE_H,
    SDL_SCANCODE_I, SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L,
    SDL_SCANCODE_M, SDL_SCANCODE_N, SDL_SCANCODE_O, SDL_SCANCODE_P,
    SDL_SCANCODE_Q, SDL_SCANCODE_R, SDL_SCANCODE_S, SDL_SCANCODE_T,
    SDL_SCANCODE_U, SDL_SCANCODE_V, SDL_SCANCODE_W, SDL_SCANCODE_X,
    SDL_SCANCODE_Y, SDL_SCANCODE_Z,
    SDL_SCANCODE_1 = 30, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4,
    SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8,
    SDL_SCANCODE_9, SDL_SCANCODE_0,
    SDL_SCANCODE_RETURN = 40,
    SDL_SCANCODE_ESCAPE = 41,
    SDL_SCANCODE_BACKSPACE = 42,
    SDL_SCANCODE_TAB = 43,
    SDL_SCANCODE_SPACE = 44,
    SDL_SCANCODE_CLEAR = 156,
    SDL_SCANCODE_MINUS = 45,
    SDL_SCANCODE_EQUALS = 46,
    SDL_SCANCODE_LEFTBRACKET = 47,
    SDL_SCANCODE_RIGHTBRACKET = 48,
    SDL_SCANCODE_BACKSLASH = 49,
    SDL_SCANCODE_SEMICOLON = 51,
    SDL_SCANCODE_APOSTROPHE = 52,
    SDL_SCANCODE_GRAVE = 53,
    SDL_SCANCODE_COMMA = 54,
    SDL_SCANCODE_PERIOD = 55,
    SDL_SCANCODE_SLASH = 56,
    SDL_SCANCODE_CAPSLOCK = 57,
    SDL_SCANCODE_F1 = 58, SDL_SCANCODE_F2, SDL_SCANCODE_F3, SDL_SCANCODE_F4,
    SDL_SCANCODE_F5, SDL_SCANCODE_F6, SDL_SCANCODE_F7, SDL_SCANCODE_F8,
    SDL_SCANCODE_F9, SDL_SCANCODE_F10, SDL_SCANCODE_F11, SDL_SCANCODE_F12,
    SDL_SCANCODE_PRINTSCREEN = 70,
    SDL_SCANCODE_SCROLLLOCK = 71,
    SDL_SCANCODE_PAUSE = 72,
    SDL_SCANCODE_INSERT = 73,
    SDL_SCANCODE_HOME = 74,
    SDL_SCANCODE_PAGEUP = 75,
    SDL_SCANCODE_DELETE = 76,
    SDL_SCANCODE_END = 77,
    SDL_SCANCODE_PAGEDOWN = 78,
    SDL_SCANCODE_RIGHT = 79,
    SDL_SCANCODE_LEFT = 80,
    SDL_SCANCODE_DOWN = 81,
    SDL_SCANCODE_UP = 82,
    SDL_SCANCODE_NUMLOCKCLEAR = 83,
    SDL_SCANCODE_KP_DIVIDE = 84,
    SDL_SCANCODE_KP_MULTIPLY = 85,
    SDL_SCANCODE_KP_MINUS = 86,
    SDL_SCANCODE_KP_PLUS = 87,
    SDL_SCANCODE_KP_ENTER = 88,
    SDL_SCANCODE_KP_1 = 89,
    SDL_SCANCODE_KP_2 = 90,
    SDL_SCANCODE_KP_3 = 91,
    SDL_SCANCODE_KP_4 = 92,
    SDL_SCANCODE_KP_5 = 93,
    SDL_SCANCODE_KP_6 = 94,
    SDL_SCANCODE_KP_7 = 95,
    SDL_SCANCODE_KP_8 = 96,
    SDL_SCANCODE_KP_9 = 97,
    SDL_SCANCODE_KP_0 = 98,
    SDL_SCANCODE_KP_PERIOD = 99,
    SDL_SCANCODE_APPLICATION = 101,
    SDL_SCANCODE_POWER = 102,
    SDL_SCANCODE_KP_EQUALS = 103,
    SDL_SCANCODE_F13 = 104,
    SDL_SCANCODE_F14 = 105,
    SDL_SCANCODE_F15 = 106,
    SDL_SCANCODE_HELP = 117,
    SDL_SCANCODE_MENU = 118,
    SDL_SCANCODE_UNDO = 122,
    SDL_SCANCODE_SYSREQ = 154,
    SDL_SCANCODE_AC_BACK = 270,
    SDL_SCANCODE_MODE = 257,
    SDL_SCANCODE_LCTRL = 224,
    SDL_SCANCODE_LSHIFT = 225,
    SDL_SCANCODE_LALT = 226,
    SDL_SCANCODE_LGUI = 227,
    SDL_SCANCODE_RCTRL = 228,
    SDL_SCANCODE_RSHIFT = 229,
    SDL_SCANCODE_RALT = 230,
    SDL_SCANCODE_RGUI = 231,
    SDL_NUM_SCANCODES = 512,
} SDL_Scancode;

#define SDLK_LAST SDL_NUM_SCANCODES

typedef struct {
    SDL_Scancode scancode;
    int sym;
    uint16_t mod;
} SDL_Keysym;

typedef union {
    uint32_t type;
    struct { uint32_t type; uint8_t repeat; SDL_Keysym keysym; } key;
    struct { uint32_t type; char text[32]; } text;
    struct { uint32_t type; int x, y; } wheel;
    struct { uint32_t type; uint8_t button; uint8_t state; int x, y; } button;
} SDL_Event;

typedef uint32_t SDL_AudioDeviceID;
typedef void (*SDL_AudioCallback)(void *userdata, uint8_t *stream, int len);

typedef struct {
    int freq;
    uint16_t format;
    uint8_t channels;
    uint8_t silence;
    uint16_t samples;
    uint32_t size;
    SDL_AudioCallback callback;
    void *userdata;
} SDL_AudioSpec;

typedef struct {
    int needed;
    uint16_t src_format;
    uint16_t dst_format;
    double rate_incr;
    uint8_t *buf;
    int len;
    int len_cvt;
    int len_mult;
    double len_ratio;
} SDL_AudioCVT;

#define RW_SEEK_SET 0
#define RW_SEEK_CUR 1
#define RW_SEEK_END 2

typedef struct SDL_RWops {
    Sint64 (*size)(struct SDL_RWops *context);
    Sint64 (*seek)(struct SDL_RWops *context, Sint64 offset, int whence);
    size_t (*read)(struct SDL_RWops *context, void *ptr, size_t size, size_t maxnum);
    size_t (*write)(struct SDL_RWops *context, const void *ptr, size_t size, size_t num);
    int (*close)(struct SDL_RWops *context);
    uint32_t type;
    union {
        struct { void *data1; void *data2; } unknown;
        struct { uint8_t *base; uint8_t *here; uint8_t *stop; } mem;
    } hidden;
} SDL_RWops;

typedef void *SDL_mutex;
typedef struct { int __idx; } SDL_Joystick;

/* Game controller */
typedef enum {
    SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B,
    SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
    SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_GUIDE,
    SDL_CONTROLLER_BUTTON_START,
    SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
    SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    SDL_CONTROLLER_BUTTON_MAX,
    SDL_CONTROLLER_BUTTON_INVALID = -1,
} SDL_GameControllerButton;

typedef enum {
    SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY,
    SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY,
    SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
    SDL_CONTROLLER_AXIS_MAX,
} SDL_GameControllerAxis;

typedef struct { int __idx; } SDL_GameController;

/* ======================================================================
 * Internal state
 * ====================================================================== */

static SDL_Window       __sdl_win;
static SDL_Renderer     __sdl_ren;
static SDL_Texture      __sdl_tex;
static SDL_Palette      __sdl_palette;
static SDL_PixelFormat  __sdl_pixfmt;
static SDL_Surface      __sdl_surface;
static uint8_t         *__sdl_surface_storage;
static size_t           __sdl_surface_storage_size;
static int              __sdl_surface_uses_hw_fb;
static int              __sdl_inited;

static of_input_state_t __sdl_prev_input;
static of_input_state_t __sdl_curr_input;
static int              __sdl_events_pending;
static uint32_t         __sdl_pressed;
static uint32_t         __sdl_released;
static int              __sdl_event_bit;
static uint8_t          __sdl_keystate[SDL_NUM_SCANCODES];
static int              __sdl_polled;

static SDL_AudioCallback __sdl_audio_cb;
static void             *__sdl_audio_userdata;

static SDL_GameController __sdl_gc;
static SDL_Joystick       __sdl_joy;
static SDL_Keymod         __sdl_mod_state;

/* ======================================================================
 * Init / Quit
 * ====================================================================== */

static inline int SDL_Init(uint32_t flags) {
    (void)flags;
    memset(__sdl_keystate, 0, sizeof(__sdl_keystate));
    return 0;
}
static inline int SDL_InitSubSystem(uint32_t flags) { (void)flags; return 0; }
static inline void SDL_QuitSubSystem(uint32_t flags) { (void)flags; }
static inline void SDL_Quit(void) {}
static inline const char *SDL_GetError(void) { return ""; }
static inline void SDL_GetVersion(SDL_version *v) {
    if (!v) return;
    v->major = SDL_MAJOR_VERSION;
    v->minor = SDL_MINOR_VERSION;
    v->patch = SDL_PATCHLEVEL;
}

/* ======================================================================
 * Video — direct HW framebuffer when sizes match, software window surface
 * otherwise.  This matches SDL's model: the app-requested window surface can
 * be a logical drawable size, while the backend presents it to the current
 * display framebuffer.
 *
 * Proven working sequence used by all SDK apps:
 *   of_video_clear(0) → of_video_surface() → writes → of_video_flip()
 * ====================================================================== */

static inline void __sdl_setup_surface(void) {
    if (__sdl_inited) return;
    __sdl_palette.ncolors = 256;
    memset(__sdl_palette.colors, 0, sizeof(__sdl_palette.colors));

    __sdl_pixfmt.format      = SDL_PIXELFORMAT_INDEX8;
    __sdl_pixfmt.palette     = &__sdl_palette;
    __sdl_pixfmt.BitsPerPixel  = 8;
    __sdl_pixfmt.BytesPerPixel = 1;
    __sdl_pixfmt.Rmask = 0;
    __sdl_pixfmt.Gmask = 0;
    __sdl_pixfmt.Bmask = 0;
    __sdl_pixfmt.Amask = 0;

    __sdl_surface.format     = &__sdl_pixfmt;
    __sdl_surface.w          = OF_SCREEN_W;
    __sdl_surface.h          = OF_SCREEN_H;
    __sdl_surface.pitch      = OF_SCREEN_W;
    __sdl_surface.clip_rect  = (SDL_Rect){0, 0, OF_SCREEN_W, OF_SCREEN_H};
    __sdl_surface.locked     = 0;
    __sdl_inited = 1;
}

static inline void __sdl_fb_dims(int *w, int *h, int *stride) {
    of_video_mode_t mode;
    of_video_get_mode(&mode);
    int fw = mode.width ? (int)mode.width : OF_SCREEN_W;
    int fh = mode.height ? (int)mode.height : OF_SCREEN_H;
    int fs = mode.stride ? (int)mode.stride : fw;
    if (w) *w = fw;
    if (h) *h = fh;
    if (stride) *stride = fs;
}

static inline int __sdl_try_hw_fb_mode(int w, int h) {
    if (w <= 0 || h <= 0)
        return 0;
    if (w > OF_VIDEO_MAX_WIDTH || h > OF_VIDEO_MAX_HEIGHT)
        return 0;

    of_video_mode_t mode;
    of_video_get_mode(&mode);
    if (mode.width == (uint16_t)w &&
        mode.height == (uint16_t)h &&
        mode.color_mode == OF_VIDEO_MODE_8BIT) {
        return 1;
    }

    mode.width = (uint16_t)w;
    mode.height = (uint16_t)h;
    mode.stride = 0;
    mode.color_mode = OF_VIDEO_MODE_8BIT;
    mode.reserved = 0;
    if (of_video_check_mode(&mode, NULL) < 0)
        return 0;
    return of_video_set_mode(&mode) == 0;
}

static inline int __sdl_configure_window_surface(int w, int h) {
    __sdl_setup_surface();
    if (w <= 0) w = OF_SCREEN_W;
    if (h <= 0) h = OF_SCREEN_H;

    (void)__sdl_try_hw_fb_mode(w, h);

    int fw, fh, fs;
    __sdl_fb_dims(&fw, &fh, &fs);
    (void)fs;

    __sdl_surface.w = w;
    __sdl_surface.h = h;
    __sdl_surface.pitch = w;
    __sdl_surface.clip_rect = (SDL_Rect){0, 0, w, h};

    if (w == fw && h == fh) {
        __sdl_surface_uses_hw_fb = 1;
        __sdl_surface.pixels = of_video_surface();
        return 0;
    }

    size_t need = (size_t)w * (size_t)h;
    if (need == 0) return -1;
    if (need > __sdl_surface_storage_size) {
        uint8_t *p = (uint8_t *)realloc(__sdl_surface_storage, need);
        if (!p) return -1;
        if (need > __sdl_surface_storage_size)
            memset(p + __sdl_surface_storage_size, 0, need - __sdl_surface_storage_size);
        __sdl_surface_storage = p;
        __sdl_surface_storage_size = need;
    }

    __sdl_surface_uses_hw_fb = 0;
    __sdl_surface.pixels = __sdl_surface_storage;
    return 0;
}

static inline void __sdl_present_window_surface(void) {
    if (!__sdl_surface.pixels) return;

    if (__sdl_surface_uses_hw_fb) {
        of_video_flip();
        __sdl_surface.pixels = of_video_surface();
        return;
    }

    int fw, fh, fs;
    __sdl_fb_dims(&fw, &fh, &fs);
    uint8_t *fb = of_video_surface();
    const uint8_t *src = (const uint8_t *)__sdl_surface.pixels;
    int sw = __sdl_surface.w;
    int sh = __sdl_surface.h;
    int sp = __sdl_surface.pitch;

    if (sw == fw && sh == fh && sp == fs) {
        memcpy(fb, src, (size_t)fs * (size_t)fh);
    } else if (sw == fw && sh == fh) {
        for (int y = 0; y < fh; y++)
            memcpy(fb + (size_t)y * (size_t)fs,
                   src + (size_t)y * (size_t)sp, (size_t)fw);
    } else {
        uint32_t xstep = ((uint32_t)sw << 16) / (uint32_t)fw;
        uint32_t ystep = ((uint32_t)sh << 16) / (uint32_t)fh;
        uint32_t yacc = 0;
        for (int y = 0; y < fh; y++) {
            int sy = (int)(yacc >> 16);
            const uint8_t *srow = src + (size_t)sy * (size_t)sp;
            uint8_t *drow = fb + (size_t)y * (size_t)fs;
            uint32_t xacc = 0;
            for (int x = 0; x < fw; x++) {
                int sx = (int)(xacc >> 16);
                drow[x] = srow[sx];
                xacc += xstep;
            }
            yacc += ystep;
        }
    }

    of_video_flip();
}

/* SDL2-native path */
static inline SDL_Window *SDL_CreateWindow(const char *title, int x, int y,
                                            int w, int h, uint32_t flags) {
    (void)title; (void)x; (void)y;
    of_video_init();
    __sdl_setup_surface();
    __sdl_win.w = w > 0 ? w : OF_SCREEN_W;
    __sdl_win.h = h > 0 ? h : OF_SCREEN_H;
    __sdl_win.flags = flags | SDL_WINDOW_SHOWN;
    if (__sdl_configure_window_surface(__sdl_win.w, __sdl_win.h) < 0)
        return NULL;
    return &__sdl_win;
}
static inline void SDL_DestroyWindow(SDL_Window *w) {
    (void)w;
    free(__sdl_surface_storage);
    __sdl_surface_storage = NULL;
    __sdl_surface_storage_size = 0;
    __sdl_surface_uses_hw_fb = 0;
}
static inline uint32_t SDL_GetWindowFlags(SDL_Window *w) {
    return w ? w->flags : SDL_WINDOW_SHOWN;
}
static inline int SDL_GetNumVideoDisplays(void) {
    return 1;
}
static inline int SDL_GetNumDisplayModes(int displayIndex) {
    if (displayIndex != 0) return -1;
    return of_video_get_mode_count();
}
static inline int SDL_GetDisplayMode(int displayIndex, int modeIndex,
                                      SDL_DisplayMode *mode) {
    if (displayIndex != 0 || !mode) return -1;
    of_video_mode_t of_mode;
    if (of_video_get_mode_info(modeIndex, &of_mode) < 0)
        return -1;
    mode->format = SDL_PIXELFORMAT_INDEX8;
    mode->w = of_mode.width;
    mode->h = of_mode.height;
    mode->refresh_rate = 60;
    mode->driverdata = NULL;
    return 0;
}
static inline int SDL_GetCurrentDisplayMode(int displayIndex,
                                             SDL_DisplayMode *mode) {
    if (displayIndex != 0 || !mode) return -1;
    of_video_mode_t of_mode;
    of_video_get_mode(&of_mode);
    mode->format = SDL_PIXELFORMAT_INDEX8;
    mode->w = of_mode.width;
    mode->h = of_mode.height;
    mode->refresh_rate = 60;
    mode->driverdata = NULL;
    return 0;
}
static inline int SDL_GetDesktopDisplayMode(int displayIndex,
                                             SDL_DisplayMode *mode) {
    return SDL_GetCurrentDisplayMode(displayIndex, mode);
}
static inline int SDL_VideoModeOK(int width, int height, int bpp,
                                   uint32_t flags) {
    (void)flags;
    if (width <= 0) width = OF_SCREEN_W;
    if (height <= 0) height = OF_SCREEN_H;
    if (bpp != 0 && bpp != 8)
        return 0;
    if (width > OF_VIDEO_MAX_WIDTH || height > OF_VIDEO_MAX_HEIGHT)
        return 0;

    of_video_mode_t mode = {
        (uint16_t)width, (uint16_t)height, 0, OF_VIDEO_MODE_8BIT, 0
    };
    return of_video_check_mode(&mode, NULL) == 0 ? 8 : 0;
}
static inline SDL_Rect **SDL_ListModes(SDL_PixelFormat *format,
                                        uint32_t flags) {
    (void)flags;
    if (format && format->BitsPerPixel != 0 && format->BitsPerPixel != 8)
        return NULL;
    return (SDL_Rect **)-1;
}
static inline int SDL_SetWindowDisplayMode(SDL_Window *w,
                                            const SDL_DisplayMode *mode) {
    if (!w || !mode) return -1;
    w->w = mode->w > 0 ? mode->w : OF_SCREEN_W;
    w->h = mode->h > 0 ? mode->h : OF_SCREEN_H;
    return __sdl_configure_window_surface(w->w, w->h);
}
static inline int SDL_GetWindowDisplayMode(SDL_Window *w,
                                            SDL_DisplayMode *mode) {
    (void)w;
    return SDL_GetCurrentDisplayMode(0, mode);
}
static inline void SDL_GetWindowSize(SDL_Window *w, int *ow, int *oh) {
    if (ow) *ow = (w && w->w > 0) ? w->w : OF_SCREEN_W;
    if (oh) *oh = (w && w->h > 0) ? w->h : OF_SCREEN_H;
}
static inline void SDL_SetWindowSize(SDL_Window *w, int width, int height) {
    if (!w) return;
    w->w = width > 0 ? width : OF_SCREEN_W;
    w->h = height > 0 ? height : OF_SCREEN_H;
    (void)__sdl_configure_window_surface(w->w, w->h);
}

static inline SDL_Surface *SDL_GetWindowSurface(SDL_Window *w) {
    int w_out = (w && w->w > 0) ? w->w : OF_SCREEN_W;
    int h_out = (w && w->h > 0) ? w->h : OF_SCREEN_H;
    if (__sdl_configure_window_surface(w_out, h_out) < 0)
        return NULL;
    return &__sdl_surface;
}

static inline int SDL_UpdateWindowSurface(SDL_Window *w) {
    (void)w;
    __sdl_present_window_surface();
    return 0;
}

static inline int SDL_UpdateWindowSurfaceRects(SDL_Window *w,
                                                const SDL_Rect *rects,
                                                int numrects) {
    (void)rects; (void)numrects;
    return SDL_UpdateWindowSurface(w);
}

/* SDL 1.2 compat path — returns a surface at the requested logical size.
 * If it does not match the hardware framebuffer, SDL_Flip scales it during
 * presentation. */
static inline SDL_Surface *SDL_SetVideoMode(int width, int height,
                                              int bpp, uint32_t flags) {
    (void)bpp;
    of_video_init();
    of_video_clear(0);
    __sdl_win.w = width > 0 ? width : OF_SCREEN_W;
    __sdl_win.h = height > 0 ? height : OF_SCREEN_H;
    __sdl_win.flags = flags | SDL_WINDOW_SHOWN;
    if (__sdl_configure_window_surface(__sdl_win.w, __sdl_win.h) < 0)
        return NULL;
    return &__sdl_surface;
}

static inline SDL_Surface *SDL_GetVideoSurface(void) {
    return SDL_GetWindowSurface(&__sdl_win);
}

/* Present the current frame and prepare the next back buffer.
 * Matches the proven SDK pattern: flip → clear → update pointer. */
static inline void SDL_Flip(SDL_Surface *s) {
    (void)s;
    __sdl_present_window_surface();
    of_video_clear(0);
    if (__sdl_surface_uses_hw_fb)
        __sdl_surface.pixels = of_video_surface();
}

/* ======================================================================
 * Palette
 * ====================================================================== */

static inline int SDL_SetPaletteColors(SDL_Palette *palette,
                                        const SDL_Color *colors,
                                        int first, int ncolors) {
    for (int i = 0; i < ncolors && (first + i) < 256; i++) {
        int idx = first + i;
        palette->colors[idx] = colors[i];
    }

    if (first == 0 && ncolors >= 256) {
        uint32_t pal32[256];
        for (int i = 0; i < 256; i++) {
            SDL_Color c = palette->colors[i];
            pal32[i] = ((uint32_t)c.r << 16) |
                       ((uint32_t)c.g << 8) |
                       (uint32_t)c.b;
        }
        of_video_palette_bulk(pal32, 256);
        return 0;
    }

    for (int i = 0; i < ncolors && (first + i) < 256; i++) {
        int idx = first + i;
        SDL_Color c = palette->colors[idx];
        uint32_t rgb = ((uint32_t)c.r << 16) |
                       ((uint32_t)c.g << 8) |
                       (uint32_t)c.b;
        of_video_palette((uint8_t)idx, rgb);
    }
    return 0;
}

static inline void SDL_SetPalette(SDL_Surface *surf, int flag,
                                   const SDL_Color *pal, int first, int count) {
    (void)flag;
    if (!surf || !surf->format || !surf->format->palette) return;
    SDL_SetPaletteColors(surf->format->palette, pal, first, count);
}

static inline int SDL_SetSurfacePalette(SDL_Surface *s, SDL_Palette *p) {
    s->format->palette = p; return 0;
}

static inline SDL_Palette *SDL_AllocPalette(int n) {
    (void)n; return &__sdl_palette;
}
static inline void SDL_FreePalette(SDL_Palette *p) { (void)p; }

/* ======================================================================
 * Drawing
 * ====================================================================== */

static inline int SDL_FillRect(SDL_Surface *dst, const SDL_Rect *rect,
                                uint32_t color) {
    uint8_t c = (uint8_t)color;
    if (!dst) return -1;
    if (!rect) {
        memset(dst->pixels, c, (size_t)(dst->pitch * dst->h));
    } else {
        uint8_t *p = (uint8_t *)dst->pixels;
        int x0 = rect->x < 0 ? 0 : rect->x;
        int y0 = rect->y < 0 ? 0 : rect->y;
        int x1 = rect->x + rect->w; if (x1 > dst->w) x1 = dst->w;
        int y1 = rect->y + rect->h; if (y1 > dst->h) y1 = dst->h;
        for (int y = y0; y < y1; y++)
            memset(p + y * dst->pitch + x0, c, (size_t)(x1 - x0));
    }
    return 0;
}

static inline uint32_t SDL_MapRGB(const SDL_PixelFormat *fmt,
                                   uint8_t r, uint8_t g, uint8_t b) {
    if (fmt && fmt->palette && fmt->BitsPerPixel == 8) {
        for (int i = 0; i < fmt->palette->ncolors; i++) {
            SDL_Color c = fmt->palette->colors[i];
            if (c.r == r && c.g == g && c.b == b) return (uint32_t)i;
        }
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static inline int SDL_SetColorKey(SDL_Surface *s, int flag, uint32_t key) {
    (void)s; (void)flag; (void)key; return 0;
}

static inline int SDL_ShowCursor(int toggle) { (void)toggle; return 0; }

/* ======================================================================
 * Renderer / Texture stubs
 * ====================================================================== */

static inline SDL_Renderer *SDL_CreateRenderer(SDL_Window *w, int idx,
                                                uint32_t flags) {
    (void)w; (void)idx; (void)flags; return &__sdl_ren;
}
static inline void SDL_DestroyRenderer(SDL_Renderer *r) { (void)r; }
static inline int SDL_RenderSetLogicalSize(SDL_Renderer *r, int w, int h) {
    (void)r; (void)w; (void)h; return 0;
}
static inline SDL_Texture *SDL_CreateTexture(SDL_Renderer *r, uint32_t fmt,
                                              int access, int w, int h) {
    (void)r; (void)fmt; (void)access;
    __sdl_tex.w = w > 0 ? w : OF_SCREEN_W;
    __sdl_tex.h = h > 0 ? h : OF_SCREEN_H;
    __sdl_tex.pitch = __sdl_tex.w;
    __sdl_tex.pixels = of_video_surface();
    return &__sdl_tex;
}
static inline void SDL_DestroyTexture(SDL_Texture *t) { (void)t; }
static inline int SDL_QueryTexture(SDL_Texture *t, uint32_t *format,
                                   int *access, int *w, int *h) {
    if (!t) return -1;
    if (format) *format = SDL_PIXELFORMAT_INDEX8;
    if (access) *access = SDL_TEXTUREACCESS_STREAMING;
    if (w) *w = t->w;
    if (h) *h = t->h;
    return 0;
}
static inline SDL_bool SDL_PixelFormatEnumToMasks(uint32_t format, int *bpp,
                                                  uint32_t *Rmask, uint32_t *Gmask,
                                                  uint32_t *Bmask, uint32_t *Amask) {
    switch (format) {
    case SDL_PIXELFORMAT_ARGB8888:
        if (bpp) *bpp = 32;
        if (Rmask) *Rmask = 0x00ff0000;
        if (Gmask) *Gmask = 0x0000ff00;
        if (Bmask) *Bmask = 0x000000ff;
        if (Amask) *Amask = 0xff000000;
        return SDL_TRUE;
    case SDL_PIXELFORMAT_RGB565:
        if (bpp) *bpp = 16;
        if (Rmask) *Rmask = 0xf800;
        if (Gmask) *Gmask = 0x07e0;
        if (Bmask) *Bmask = 0x001f;
        if (Amask) *Amask = 0;
        return SDL_TRUE;
    case SDL_PIXELFORMAT_ARGB1555:
        if (bpp) *bpp = 15;
        if (Rmask) *Rmask = 0x7c00;
        if (Gmask) *Gmask = 0x03e0;
        if (Bmask) *Bmask = 0x001f;
        if (Amask) *Amask = 0x8000;
        return SDL_TRUE;
    default:
        if (bpp) *bpp = 8;
        if (Rmask) *Rmask = 0;
        if (Gmask) *Gmask = 0;
        if (Bmask) *Bmask = 0;
        if (Amask) *Amask = 0;
        return SDL_TRUE;
    }
}
static inline int SDL_UpdateTexture(SDL_Texture *t, const SDL_Rect *r,
                                     const void *px, int pitch) {
    (void)t; (void)r; (void)px; (void)pitch; return 0;
}
static inline int SDL_SetRenderDrawColor(SDL_Renderer *r, uint8_t R,
                                          uint8_t g, uint8_t b, uint8_t a) {
    (void)r; (void)R; (void)g; (void)b; (void)a; return 0;
}
static inline int SDL_RenderClear(SDL_Renderer *r) {
    (void)r; of_video_clear(0); return 0;
}
static inline int SDL_RenderCopy(SDL_Renderer *r, SDL_Texture *t,
                                  const SDL_Rect *s, const SDL_Rect *d) {
    (void)r; (void)t; (void)s; (void)d; return 0;
}
static inline void SDL_RenderPresent(SDL_Renderer *r) {
    (void)r; of_video_flip();
}
static inline int SDL_LockSurface(SDL_Surface *s) {
    if (!s) return -1;
    if (!s->pixels) s->pixels = of_video_surface();
    s->locked++;
    return 0;
}
static inline void SDL_UnlockSurface(SDL_Surface *s) {
    if (s && s->locked > 0) s->locked--;
}
static inline int SDL_LockTexture(SDL_Texture *t, const SDL_Rect *r,
                                  void **pixels, int *pitch) {
    (void)r;
    if (!t) return -1;
    t->pixels = of_video_surface();
    t->pitch = t->w > 0 ? t->w : OF_SCREEN_W;
    if (pixels) *pixels = t->pixels;
    if (pitch) *pitch = t->pitch;
    return 0;
}
static inline void SDL_UnlockTexture(SDL_Texture *t) { (void)t; }

/* ======================================================================
 * Events / Input
 * ====================================================================== */

static inline void __sdl_update_keystate(void) {
    memset(__sdl_keystate, 0, sizeof(__sdl_keystate));
    uint32_t b = __sdl_curr_input.buttons;
    if (b & OF_BTN_UP)     __sdl_keystate[SDL_SCANCODE_UP] = 1;
    if (b & OF_BTN_DOWN)   __sdl_keystate[SDL_SCANCODE_DOWN] = 1;
    if (b & OF_BTN_LEFT)   __sdl_keystate[SDL_SCANCODE_LEFT] = 1;
    if (b & OF_BTN_RIGHT)  __sdl_keystate[SDL_SCANCODE_RIGHT] = 1;
    if (b & OF_BTN_A)    { __sdl_keystate[SDL_SCANCODE_Z] = 1;
                           __sdl_keystate[SDL_SCANCODE_C] = 1; }
    if (b & OF_BTN_B)    { __sdl_keystate[SDL_SCANCODE_X] = 1;
                           __sdl_keystate[SDL_SCANCODE_V] = 1; }
    if (b & OF_BTN_X)     __sdl_keystate[SDL_SCANCODE_E] = 1;
    if (b & OF_BTN_Y)     __sdl_keystate[SDL_SCANCODE_F9] = 1;
    if (b & OF_BTN_L1)    __sdl_keystate[SDL_SCANCODE_LSHIFT] = 1;
    if (b & OF_BTN_R1)    __sdl_keystate[SDL_SCANCODE_S] = 1;
    if (b & OF_BTN_L2)    __sdl_keystate[SDL_SCANCODE_D] = 1;
    if (b & OF_BTN_SELECT) __sdl_keystate[SDL_SCANCODE_F11] = 1;
    if (b & OF_BTN_START) __sdl_keystate[SDL_SCANCODE_ESCAPE] = 1;
}

static inline void __sdl_do_poll(void) {
    if (__sdl_polled) return;
    of_input_poll();
    of_input_state(0, &__sdl_curr_input);
    __sdl_pressed  = __sdl_curr_input.buttons & ~__sdl_prev_input.buttons;
    __sdl_released = ~__sdl_curr_input.buttons & __sdl_prev_input.buttons;
    __sdl_prev_input = __sdl_curr_input;
    __sdl_update_keystate();
    __sdl_polled = 1;
}

static inline SDL_Scancode __sdl_btn_to_scancode(int bit) {
    switch (1 << bit) {
    case OF_BTN_UP:     return SDL_SCANCODE_UP;
    case OF_BTN_DOWN:   return SDL_SCANCODE_DOWN;
    case OF_BTN_LEFT:   return SDL_SCANCODE_LEFT;
    case OF_BTN_RIGHT:  return SDL_SCANCODE_RIGHT;
    case OF_BTN_A:      return SDL_SCANCODE_Z;
    case OF_BTN_B:      return SDL_SCANCODE_X;
    case OF_BTN_X:      return SDL_SCANCODE_E;
    case OF_BTN_Y:      return SDL_SCANCODE_F9;
    case OF_BTN_L1:     return SDL_SCANCODE_LSHIFT;
    case OF_BTN_R1:     return SDL_SCANCODE_S;
    case OF_BTN_L2:     return SDL_SCANCODE_D;
    case OF_BTN_SELECT: return SDL_SCANCODE_F11;
    case OF_BTN_START:  return SDL_SCANCODE_ESCAPE;
    default:            return SDL_SCANCODE_UNKNOWN;
    }
}

static inline int SDL_PollEvent(SDL_Event *event) {
    if (!__sdl_events_pending) {
        __sdl_do_poll();
        __sdl_events_pending = 1;
        __sdl_event_bit = 0;
    }

    while (__sdl_event_bit < 16) {
        uint32_t mask = 1u << __sdl_event_bit;
        __sdl_event_bit++;
        if (__sdl_pressed & mask) {
            event->type = SDL_KEYDOWN;
            event->key.type = SDL_KEYDOWN;
            event->key.repeat = 0;
            event->key.keysym.scancode = __sdl_btn_to_scancode(__sdl_event_bit - 1);
            event->key.keysym.sym = event->key.keysym.scancode;
            event->key.keysym.mod = 0;
            return 1;
        }
        if (__sdl_released & mask) {
            event->type = SDL_KEYUP;
            event->key.type = SDL_KEYUP;
            event->key.repeat = 0;
            event->key.keysym.scancode = __sdl_btn_to_scancode(__sdl_event_bit - 1);
            event->key.keysym.sym = event->key.keysym.scancode;
            event->key.keysym.mod = 0;
            return 1;
        }
    }

    __sdl_events_pending = 0;
    __sdl_polled = 0;
    return 0;
}

static inline const uint8_t *SDL_GetKeyboardState(int *numkeys) {
    if (numkeys) *numkeys = SDL_NUM_SCANCODES;
    return __sdl_keystate;
}
#define SDL_GetKeyState SDL_GetKeyboardState

static inline int SDL_PushEvent(SDL_Event *ev) { (void)ev; return 0; }
static inline void SDL_PumpEvents(void) {}
static inline int SDL_WaitEvent(SDL_Event *event) {
    while (!SDL_PollEvent(event)) usleep(10000);
    return 1;
}
static inline int SDL_WaitEventTimeout(SDL_Event *event, int timeout) {
    uint32_t start = of_time_ms();
    do {
        if (SDL_PollEvent(event)) return 1;
        usleep(1000);
    } while ((int)(of_time_ms() - start) < timeout);
    return 0;
}
static inline uint8_t SDL_EventState(uint32_t type, int state) {
    (void)type; (void)state; return SDL_ENABLE;
}
static inline SDL_Keymod SDL_GetModState(void) { return __sdl_mod_state; }
static inline void SDL_SetModState(SDL_Keymod modstate) { __sdl_mod_state = modstate; }
static inline int SDL_SetRelativeMouseMode(SDL_bool enabled) { (void)enabled; return 0; }
static inline uint32_t SDL_GetMouseState(int *x, int *y) {
    if (x) *x = 0;
    if (y) *y = 0;
    return 0;
}
static inline uint32_t SDL_GetRelativeMouseState(int *x, int *y) {
    __sdl_do_poll();
    if (x) *x = (int)__sdl_curr_input.joy_rx;
    if (y) *y = (int)__sdl_curr_input.joy_ry;
    return 0;
}
static inline void SDL_WarpMouseInWindow(SDL_Window *window, int x, int y) {
    (void)window; (void)x; (void)y;
}

/* ======================================================================
 * Game Controller
 * ====================================================================== */

static inline int SDL_NumJoysticks(void) { return 1; }
static inline SDL_bool SDL_IsGameController(int i) {
    return (i == 0) ? SDL_TRUE : SDL_FALSE;
}
static inline SDL_GameController *SDL_GameControllerOpen(int i) {
    (void)i; return &__sdl_gc;
}
static inline const char *SDL_GameControllerName(SDL_GameController *gc) {
    (void)gc; return "Analogue Pocket";
}
static inline void SDL_GameControllerUpdate(void) { __sdl_do_poll(); }
static inline int SDL_GameControllerEventState(int state) { (void)state; return SDL_ENABLE; }
static inline void SDL_GameControllerClose(SDL_GameController *gc) { (void)gc; }

static inline SDL_bool SDL_GameControllerGetButton(SDL_GameController *gc,
                                                     SDL_GameControllerButton btn) {
    (void)gc;
    uint32_t b = __sdl_curr_input.buttons;
    switch (btn) {
    case SDL_CONTROLLER_BUTTON_A:             return (b & OF_BTN_A) != 0;
    case SDL_CONTROLLER_BUTTON_B:             return (b & OF_BTN_B) != 0;
    case SDL_CONTROLLER_BUTTON_X:             return (b & OF_BTN_X) != 0;
    case SDL_CONTROLLER_BUTTON_Y:             return (b & OF_BTN_Y) != 0;
    case SDL_CONTROLLER_BUTTON_BACK:          return (b & OF_BTN_SELECT) != 0;
    case SDL_CONTROLLER_BUTTON_START:         return (b & OF_BTN_START) != 0;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return (b & OF_BTN_L1) != 0;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return (b & OF_BTN_R1) != 0;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:       return (b & OF_BTN_UP) != 0;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return (b & OF_BTN_DOWN) != 0;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return (b & OF_BTN_LEFT) != 0;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return (b & OF_BTN_RIGHT) != 0;
    default: return 0;
    }
}

static inline SDL_Joystick *SDL_JoystickOpen(int i) {
    __sdl_joy.__idx = i;
    return &__sdl_joy;
}
static inline void SDL_JoystickClose(SDL_Joystick *j) { (void)j; }
static inline void SDL_JoystickUpdate(void) { __sdl_do_poll(); }
static inline int SDL_JoystickNumButtons(SDL_Joystick *j) {
    (void)j; return SDL_CONTROLLER_BUTTON_MAX;
}
static inline int SDL_JoystickNumAxes(SDL_Joystick *j) {
    (void)j; return SDL_CONTROLLER_AXIS_MAX;
}
static inline int SDL_JoystickNumHats(SDL_Joystick *j) {
    (void)j; return 1;
}
static inline Sint16 SDL_JoystickGetAxis(SDL_Joystick *j, int axis) {
    (void)j;
    switch (axis) {
    case 0: return __sdl_curr_input.joy_lx;
    case 1: return __sdl_curr_input.joy_ly;
    case 2: return __sdl_curr_input.joy_rx;
    case 3: return __sdl_curr_input.joy_ry;
    default: return 0;
    }
}
static inline uint8_t SDL_JoystickGetButton(SDL_Joystick *j, int button) {
    (void)j;
    return SDL_GameControllerGetButton(&__sdl_gc, (SDL_GameControllerButton)button);
}
static inline uint8_t SDL_JoystickGetHat(SDL_Joystick *j, int hat) {
    (void)j; (void)hat;
    uint8_t out = SDL_HAT_CENTERED;
    uint32_t b = __sdl_curr_input.buttons;
    if (b & OF_BTN_UP) out |= SDL_HAT_UP;
    if (b & OF_BTN_DOWN) out |= SDL_HAT_DOWN;
    if (b & OF_BTN_LEFT) out |= SDL_HAT_LEFT;
    if (b & OF_BTN_RIGHT) out |= SDL_HAT_RIGHT;
    return out;
}

static inline Sint16 SDL_GameControllerGetAxis(SDL_GameController *gc,
                                                 SDL_GameControllerAxis axis) {
    (void)gc;
    switch (axis) {
    case SDL_CONTROLLER_AXIS_LEFTX:  return __sdl_curr_input.joy_lx;
    case SDL_CONTROLLER_AXIS_LEFTY:  return __sdl_curr_input.joy_ly;
    case SDL_CONTROLLER_AXIS_RIGHTX: return __sdl_curr_input.joy_rx;
    case SDL_CONTROLLER_AXIS_RIGHTY: return __sdl_curr_input.joy_ry;
    default: return 0;
    }
}

static inline SDL_GameControllerButton
SDL_GameControllerGetButtonFromString(const char *s) {
    if (!s) return SDL_CONTROLLER_BUTTON_INVALID;
    if (!strcmp(s,"a")) return SDL_CONTROLLER_BUTTON_A;
    if (!strcmp(s,"b")) return SDL_CONTROLLER_BUTTON_B;
    if (!strcmp(s,"start")) return SDL_CONTROLLER_BUTTON_START;
    if (!strcmp(s,"leftshoulder")) return SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
    if (!strcmp(s,"rightshoulder")) return SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
    if (!strcmp(s,"dpup")) return SDL_CONTROLLER_BUTTON_DPAD_UP;
    if (!strcmp(s,"dpdown")) return SDL_CONTROLLER_BUTTON_DPAD_DOWN;
    if (!strcmp(s,"dpleft")) return SDL_CONTROLLER_BUTTON_DPAD_LEFT;
    if (!strcmp(s,"dpright")) return SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
    return SDL_CONTROLLER_BUTTON_INVALID;
}

static inline const char *
SDL_GameControllerGetStringForButton(SDL_GameControllerButton b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return "a";
    case SDL_CONTROLLER_BUTTON_B: return "b";
    case SDL_CONTROLLER_BUTTON_START: return "start";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "leftshoulder";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "rightshoulder";
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return "dpup";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "dpdown";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "dpleft";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "dpright";
    default: return "?";
    }
}

/* ======================================================================
 * Timer
 * ====================================================================== */

static inline uint32_t SDL_GetTicks(void) { return of_time_ms(); }
static inline void SDL_Delay(uint32_t ms) {
    usleep(ms * 1000u);
}

/* ======================================================================
 * Audio
 * ====================================================================== */

static inline SDL_RWops *SDL_AllocRW(void) {
    return (SDL_RWops *)calloc(1, sizeof(SDL_RWops));
}

static inline Sint64 __sdl_mem_size(SDL_RWops *ctx) {
    return (Sint64)(ctx->hidden.mem.stop - ctx->hidden.mem.base);
}
static inline Sint64 __sdl_mem_seek(SDL_RWops *ctx, Sint64 offset, int whence) {
    uint8_t *pos = ctx->hidden.mem.here;
    if (whence == RW_SEEK_SET) pos = ctx->hidden.mem.base + offset;
    else if (whence == RW_SEEK_CUR) pos = ctx->hidden.mem.here + offset;
    else if (whence == RW_SEEK_END) pos = ctx->hidden.mem.stop + offset;
    if (pos < ctx->hidden.mem.base) pos = ctx->hidden.mem.base;
    if (pos > ctx->hidden.mem.stop) pos = ctx->hidden.mem.stop;
    ctx->hidden.mem.here = pos;
    return (Sint64)(pos - ctx->hidden.mem.base);
}
static inline size_t __sdl_mem_read(SDL_RWops *ctx, void *ptr, size_t size, size_t maxnum) {
    size_t bytes = size * maxnum;
    size_t left = (size_t)(ctx->hidden.mem.stop - ctx->hidden.mem.here);
    if (bytes > left) bytes = left;
    if (bytes) {
        memcpy(ptr, ctx->hidden.mem.here, bytes);
        ctx->hidden.mem.here += bytes;
    }
    return size ? bytes / size : 0;
}
static inline size_t __sdl_mem_write(SDL_RWops *ctx, const void *ptr, size_t size, size_t num) {
    (void)ctx; (void)ptr; (void)size; (void)num; return 0;
}
static inline int __sdl_mem_close(SDL_RWops *ctx) {
    free(ctx);
    return 0;
}
static inline SDL_RWops *SDL_RWFromMem(void *mem, int size) {
    if (!mem || size < 0) return NULL;
    SDL_RWops *rw = SDL_AllocRW();
    if (!rw) return NULL;
    rw->size = __sdl_mem_size;
    rw->seek = __sdl_mem_seek;
    rw->read = __sdl_mem_read;
    rw->write = __sdl_mem_write;
    rw->close = __sdl_mem_close;
    rw->hidden.mem.base = (uint8_t *)mem;
    rw->hidden.mem.here = (uint8_t *)mem;
    rw->hidden.mem.stop = (uint8_t *)mem + size;
    return rw;
}
static inline void SDL_FreeRW(SDL_RWops *rw) {
    if (rw) free(rw);
}

static inline SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device,
        int iscapture, const SDL_AudioSpec *desired, SDL_AudioSpec *obtained,
        int allowed_changes) {
    (void)device; (void)iscapture; (void)allowed_changes;
    of_audio_init();
    /* Route the caller's sample rate through the hardware mixer's
     * stream voice.  Without this of_audio_write samples play back at
     * a default 48 kHz 1:1 regardless of desired->freq, so 22 kHz Doom
     * output pitch-shifts up to sound chipmunk-y.  stream_open
     * reconfigures mixer voice 31 to consume the ring at the requested
     * rate. */
    if (desired && desired->freq > 0) of_audio_stream_open(desired->freq);
    if (desired->callback) {
        __sdl_audio_cb = desired->callback;
        __sdl_audio_userdata = desired->userdata;
    }
    if (obtained) *obtained = *desired;
    return 1;
}

static inline void SDL_CloseAudioDevice(SDL_AudioDeviceID d) {
    (void)d; __sdl_audio_cb = 0;
}
static inline void SDL_PauseAudioDevice(SDL_AudioDeviceID d, int p) {
    (void)d; (void)p;
}

static inline void SDL_AudioPump(void) {
    if (__sdl_audio_cb) {
        int free_pairs = of_audio_free();
        if (free_pairs > 256) free_pairs = 256;
        if (free_pairs > 0) {
            int16_t buf[512];
            __sdl_audio_cb(__sdl_audio_userdata, (uint8_t *)buf, free_pairs * 4);
            of_audio_write(buf, free_pairs);
        }
    }
}

static inline int SDL_QueueAudio(SDL_AudioDeviceID d, const void *data, uint32_t len) {
    (void)d;
    of_audio_write((const int16_t *)data, (int)(len / 4));
    return 0;
}

static inline SDL_AudioSpec *__sdl_load_wav_from_memory(const uint8_t *data, uint32_t size,
                                             SDL_AudioSpec *spec,
                                             uint8_t **audio_buf,
                                             uint32_t *audio_len) {
    of_codec_result_t result;
    if (!data || !spec || !audio_buf || !audio_len ||
        of_codec_parse_wav(data, size, &result) < 0) {
        return NULL;
    }
    spec->freq = (int)result.sample_rate;
    spec->format = (result.bits_per_sample == 16) ? AUDIO_S16SYS : AUDIO_U8;
    spec->channels = result.channels;
    spec->silence = 0;
    spec->samples = 4096;
    spec->size = result.pcm_len;
    spec->callback = 0;
    spec->userdata = 0;
    uint8_t *pcm = (uint8_t *)malloc(result.pcm_len);
    if (!pcm) return NULL;
    memcpy(pcm, result.pcm, result.pcm_len);
    *audio_buf = pcm;
    *audio_len = result.pcm_len;
    return spec;
}

static inline SDL_AudioSpec *SDL_LoadWAV_RW(SDL_RWops *src, int freesrc,
                                             SDL_AudioSpec *spec,
                                             uint8_t **audio_buf,
                                             uint32_t *audio_len) {
    if (audio_buf) *audio_buf = 0;
    if (audio_len) *audio_len = 0;
    if (!src) return NULL;

    Sint64 size64 = src->size ? src->size(src) : -1;
    if (size64 < 0) {
        Sint64 here = src->seek ? src->seek(src, 0, RW_SEEK_CUR) : 0;
        Sint64 end = src->seek ? src->seek(src, 0, RW_SEEK_END) : 0;
        if (src->seek) src->seek(src, here, RW_SEEK_SET);
        size64 = end;
    }
    if (size64 <= 0 || size64 > 4 * 1024 * 1024) {
        if (freesrc && src->close) src->close(src);
        return NULL;
    }
    uint8_t *data = (uint8_t *)malloc((size_t)size64);
    if (!data) {
        if (freesrc && src->close) src->close(src);
        return NULL;
    }
    if (src->seek) src->seek(src, 0, RW_SEEK_SET);
    size_t got = src->read ? src->read(src, data, 1, (size_t)size64) : 0;
    SDL_AudioSpec *ret = got == (size_t)size64
        ? __sdl_load_wav_from_memory(data, (uint32_t)size64, spec, audio_buf, audio_len)
        : NULL;
    free(data);
    if (freesrc && src->close) src->close(src);
    return ret;
}

static inline SDL_AudioSpec *SDL_LoadWAV(const char *file, SDL_AudioSpec *spec,
                                          uint8_t **audio_buf, uint32_t *audio_len) {
    *audio_buf = 0; *audio_len = 0;
    FILE *f = fopen(file, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size <= 0 || size > 4*1024*1024) { fclose(f); return 0; }
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (!data) { fclose(f); return 0; }
    fread(data, 1, (size_t)size, f);
    fclose(f);
    SDL_AudioSpec *ret = __sdl_load_wav_from_memory(data, (uint32_t)size, spec, audio_buf, audio_len);
    free(data);
    return ret;
}

static inline void SDL_FreeWAV(uint8_t *buf) { free(buf); }

static inline int SDL_BuildAudioCVT(SDL_AudioCVT *cvt, uint16_t src_format,
                                    uint8_t src_channels, int src_rate,
                                    uint16_t dst_format, uint8_t dst_channels,
                                    int dst_rate) {
    if (!cvt) return -1;
    memset(cvt, 0, sizeof(*cvt));
    cvt->src_format = src_format;
    cvt->dst_format = dst_format;
    cvt->needed = (src_format != dst_format || src_channels != dst_channels || src_rate != dst_rate);
    cvt->len_mult = 1;
    cvt->len_ratio = 1.0;
    cvt->rate_incr = 1.0;
    return 0;
}
static inline int SDL_ConvertAudio(SDL_AudioCVT *cvt) {
    if (!cvt) return -1;
    cvt->len_cvt = cvt->len;
    return 0;
}

static inline void SDL_MixAudioFormat(uint8_t *dst, const uint8_t *src,
                                       uint16_t fmt, uint32_t len, int vol) {
    (void)fmt;
    const int16_t *s = (const int16_t *)src;
    int16_t *d = (int16_t *)dst;
    for (uint32_t i = 0; i < len/2; i++) {
        int32_t m = (int32_t)d[i] + (((int32_t)s[i] * vol) >> 7);
        if (m > 32767) m = 32767;
        if (m < -32768) m = -32768;
        d[i] = (int16_t)m;
    }
}
#define SDL_MIX_MAXVOLUME 128

/* ======================================================================
 * Misc stubs
 * ====================================================================== */

static inline SDL_mutex *SDL_CreateMutex(void) { return (SDL_mutex *)1; }
static inline void SDL_DestroyMutex(SDL_mutex *m) { (void)m; }
static inline int SDL_LockMutex(SDL_mutex *m) { (void)m; return 0; }
static inline int SDL_UnlockMutex(SDL_mutex *m) { (void)m; return 0; }

static inline void SDL_WM_SetCaption(const char *t, const char *i) { (void)t; (void)i; }
static inline int SDL_WM_ToggleFullScreen(SDL_Surface *s) { (void)s; return 1; }
static inline int SDL_SetWindowFullscreen(SDL_Window *w, uint32_t f) { (void)w;(void)f; return 0; }
static inline void SDL_SetWindowTitle(SDL_Window *w, const char *t) { (void)w;(void)t; }

/* ======================================================================
 * SDL2 compat defines — map SDLK_* to SDL_SCANCODE_*
 * ====================================================================== */

#define SDLK_F9      SDL_SCANCODE_F9
#define SDLK_ESCAPE  SDL_SCANCODE_ESCAPE
#define SDLK_DELETE  SDL_SCANCODE_DELETE
#define SDLK_F11     SDL_SCANCODE_F11
#define SDLK_LSHIFT  SDL_SCANCODE_LSHIFT
#define SDLK_LEFT    SDL_SCANCODE_LEFT
#define SDLK_RIGHT   SDL_SCANCODE_RIGHT
#define SDLK_UP      SDL_SCANCODE_UP
#define SDLK_DOWN    SDL_SCANCODE_DOWN
#define SDLK_SCROLLLOCK SDL_SCANCODE_SCROLLLOCK
#define SDLK_SCROLLOCK  SDL_SCANCODE_SCROLLLOCK
#define SDLK_BACKQUOTE  SDL_SCANCODE_GRAVE
#define SDLK_5       SDL_SCANCODE_5
#define SDLK_a       SDL_SCANCODE_A
#define SDLK_b       SDL_SCANCODE_B
#define SDLK_c       SDL_SCANCODE_C
#define SDLK_d       SDL_SCANCODE_D
#define SDLK_e       SDL_SCANCODE_E
#define SDLK_m       SDL_SCANCODE_M
#define SDLK_n       SDL_SCANCODE_N
#define SDLK_s       SDL_SCANCODE_S
#define SDLK_v       SDL_SCANCODE_V
#define SDLK_x       SDL_SCANCODE_X
#define SDLK_z       SDL_SCANCODE_Z

#define sym scancode

#endif /* OF_PC */
#endif /* _OF_SDL2_SHIM_H */
