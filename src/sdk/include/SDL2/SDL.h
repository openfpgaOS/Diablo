//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * SDL2 compatibility layer for openfpgaOS  (SDK-wide, reusable).
 *
 * This is NOT real SDL2. It declares the subset of the SDL2 API that the
 * common 2D software-rendered games use (DevilutionX, ECWolf, Doom,
 * ScummVM, ...) and is implemented over the openfpgaOS `of_*` HAL in a
 * single translation unit, src/sdk/of_sdl2.c.
 *
 * Why a declaration header + one impl TU (not a header-only inline shim):
 * SDL state (window surface, palette, event queue, audio callback) must be
 * shared across all of a game's translation units. A header-only
 * `static inline` shim gives every file its OWN copy of that state, which
 * silently breaks multi-TU games that create surfaces or pump events from
 * more than one file. of_sdl2.c is the single source of truth; sdk.mk
 * auto-links it into every app and --gc-sections drops it from apps that
 * call no SDL_* function.
 *
 * Build model: on the device this header supplies types/macros/prototypes
 * and of_sdl2.c supplies the bodies; on PC (-DOF_PC) it forwards to the
 * real system SDL2 via #include_next and the game links libSDL2.
 *
 * Video: an 8-bit indexed SDL_Surface backed by the of_video framebuffer
 * (alias when window size == active mode, else nearest-neighbor scale at
 * present). Input: of_input is exposed as BOTH controller and keyboard
 * event streams plus a live keystate (narrow with -DOF_SDL_NO_*_EVENTS).
 * Audio: the SDL_AudioCallback is auto-pumped from PollEvent/Delay/present.
 */
#ifndef OF_SDL2_SHIM_SDL_H
#define OF_SDL2_SHIM_SDL_H

#ifdef OF_PC
/* PC build: use the real system SDL2 and link libSDL2. */
#include_next <SDL2/SDL.h>
#else

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================== */
/* Compiler glue                                                          */
/* ===================================================================== */
#define DECLSPEC
#define SDLCALL
#ifndef SDL_INLINE
#define SDL_INLINE static inline
#endif
#ifndef SDL_FORCE_INLINE
#define SDL_FORCE_INLINE static inline
#endif

/* ===================================================================== */
/* Basic types (SDL_stdinc.h)                                             */
/* ===================================================================== */
typedef uint8_t  Uint8;
typedef int8_t   Sint8;
typedef uint16_t Uint16;
typedef int16_t  Sint16;
typedef uint32_t Uint32;
typedef int32_t  Sint32;
typedef uint64_t Uint64;
typedef int64_t  Sint64;

typedef enum { SDL_FALSE = 0, SDL_TRUE = 1 } SDL_bool;

/* stdinc passthroughs (DevilutionX uses SDL_malloc/free/strdup/strlen/...) */
#define SDL_malloc   malloc
#define SDL_calloc   calloc
#define SDL_realloc  realloc
#define SDL_free     free
#define SDL_memcpy   memcpy
#define SDL_memmove  memmove
#define SDL_memset   memset
#define SDL_memcmp   memcmp
#define SDL_strlen   strlen
#define SDL_strdup   strdup
#define SDL_strcmp   strcmp
#define SDL_strncmp  strncmp
#define SDL_strcasecmp  strcasecmp
#define SDL_strncasecmp strncasecmp
#define SDL_strchr   strchr
#define SDL_strrchr  strrchr
#define SDL_strstr   strstr
#define SDL_strtol   strtol
#define SDL_atoi     atoi
#define SDL_snprintf snprintf
#define SDL_vsnprintf vsnprintf
#define SDL_qsort    qsort
#define SDL_getenv   getenv
#define SDL_floor    floor
#define SDL_ceil     ceil
#define SDL_pow      pow
#define SDL_sqrt     sqrt

#define SDL_arraysize(a) (sizeof(a) / sizeof((a)[0]))
#define SDL_zero(x)  SDL_memset(&(x), 0, sizeof((x)))
#define SDL_zerop(x) SDL_memset((x), 0, sizeof(*(x)))
#define SDL_min(a, b) (((a) < (b)) ? (a) : (b))
#define SDL_max(a, b) (((a) > (b)) ? (a) : (b))
size_t SDL_strlcpy(char *dst, const char *src, size_t maxlen);
size_t SDL_strlcat(char *dst, const char *src, size_t maxlen);

/* ===================================================================== */
/* Version (SDL_version.h) -- advertise 2.0.16                            */
/* ===================================================================== */
#define SDL_MAJOR_VERSION 2
#define SDL_MINOR_VERSION 0
#define SDL_PATCHLEVEL    16
typedef struct SDL_version { Uint8 major, minor, patch; } SDL_version;
#define SDL_VERSIONNUM(X, Y, Z) ((X) * 1000 + (Y) * 100 + (Z))
#define SDL_COMPILEDVERSION SDL_VERSIONNUM(SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL)
#define SDL_VERSION_ATLEAST(X, Y, Z) (SDL_COMPILEDVERSION >= SDL_VERSIONNUM(X, Y, Z))
#define SDL_VERSION(v) do { (v)->major = SDL_MAJOR_VERSION; (v)->minor = SDL_MINOR_VERSION; (v)->patch = SDL_PATCHLEVEL; } while (0)
void SDL_GetVersion(SDL_version *ver);
const char *SDL_GetRevision(void);

/* ===================================================================== */
/* Endianness (SDL_endian.h) -- target is little-endian                   */
/* ===================================================================== */
#define SDL_LIL_ENDIAN 1234
#define SDL_BIG_ENDIAN 4321
#define SDL_BYTEORDER  SDL_LIL_ENDIAN
SDL_INLINE Uint16 SDL_Swap16(Uint16 x) { return (Uint16)((x << 8) | (x >> 8)); }
SDL_INLINE Uint32 SDL_Swap32(Uint32 x) { return __builtin_bswap32(x); }
SDL_INLINE Uint64 SDL_Swap64(Uint64 x) { return __builtin_bswap64(x); }
#define SDL_SwapLE16(x) (x)
#define SDL_SwapLE32(x) (x)
#define SDL_SwapLE64(x) (x)
#define SDL_SwapBE16(x) SDL_Swap16(x)
#define SDL_SwapBE32(x) SDL_Swap32(x)
#define SDL_SwapBE64(x) SDL_Swap64(x)
#define SDL_SwapFloatLE(x) (x)

/* ===================================================================== */
/* Init (SDL.h)                                                           */
/* ===================================================================== */
#define SDL_INIT_TIMER          0x00000001u
#define SDL_INIT_AUDIO          0x00000010u
#define SDL_INIT_VIDEO          0x00000020u
#define SDL_INIT_JOYSTICK       0x00000200u
#define SDL_INIT_HAPTIC         0x00001000u
#define SDL_INIT_GAMECONTROLLER 0x00002000u
#define SDL_INIT_EVENTS         0x00004000u
#define SDL_INIT_EVERYTHING     0x0000FFFFu
int  SDL_Init(Uint32 flags);
int  SDL_InitSubSystem(Uint32 flags);
void SDL_QuitSubSystem(Uint32 flags);
Uint32 SDL_WasInit(Uint32 flags);
void SDL_Quit(void);

/* ===================================================================== */
/* Error (SDL_error.h)                                                    */
/* ===================================================================== */
int  SDL_SetError(const char *fmt, ...);
const char *SDL_GetError(void);
void SDL_ClearError(void);
int  SDL_Error(int code);
#define SDL_OutOfMemory()        SDL_SetError("out of memory")
#define SDL_Unsupported()        SDL_SetError("unsupported")
#define SDL_InvalidParamError(p) SDL_SetError("invalid param: %s", #p)

/* ===================================================================== */
/* Log (SDL_log.h)                                                        */
/* ===================================================================== */
typedef enum {
	SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_CATEGORY_ERROR, SDL_LOG_CATEGORY_ASSERT,
	SDL_LOG_CATEGORY_SYSTEM, SDL_LOG_CATEGORY_AUDIO, SDL_LOG_CATEGORY_VIDEO,
	SDL_LOG_CATEGORY_RENDER, SDL_LOG_CATEGORY_INPUT, SDL_LOG_CATEGORY_TEST,
	SDL_LOG_CATEGORY_CUSTOM = 19
} SDL_LogCategory;
typedef enum {
	SDL_LOG_PRIORITY_VERBOSE = 1, SDL_LOG_PRIORITY_DEBUG, SDL_LOG_PRIORITY_INFO,
	SDL_LOG_PRIORITY_WARN, SDL_LOG_PRIORITY_ERROR, SDL_LOG_PRIORITY_CRITICAL,
	SDL_NUM_LOG_PRIORITIES
} SDL_LogPriority;
/* NB: real SDL has no SDL_LogLevel type, and DevilutionX defines its own;
 * do not declare one here or it becomes ambiguous. */
void SDL_Log(const char *fmt, ...);
void SDL_LogVerbose(int cat, const char *fmt, ...);
void SDL_LogDebug(int cat, const char *fmt, ...);
void SDL_LogInfo(int cat, const char *fmt, ...);
void SDL_LogWarn(int cat, const char *fmt, ...);
void SDL_LogError(int cat, const char *fmt, ...);
void SDL_LogCritical(int cat, const char *fmt, ...);
void SDL_LogMessage(int cat, SDL_LogPriority pri, const char *fmt, ...);
void SDL_LogMessageV(int cat, SDL_LogPriority pri, const char *fmt, va_list ap);
void SDL_LogSetPriority(int cat, SDL_LogPriority pri);
SDL_LogPriority SDL_LogGetPriority(int cat);
void SDL_LogSetAllPriority(SDL_LogPriority pri);

/* ===================================================================== */
/* Geometry (SDL_rect.h)                                                  */
/* ===================================================================== */
typedef struct SDL_Point { int x, y; } SDL_Point;
typedef struct SDL_Rect  { int x, y, w, h; } SDL_Rect;
typedef struct SDL_FPoint { float x, y; } SDL_FPoint;
typedef struct SDL_FRect { float x, y, w, h; } SDL_FRect;
SDL_bool SDL_HasIntersection(const SDL_Rect *a, const SDL_Rect *b);
SDL_bool SDL_IntersectRect(const SDL_Rect *a, const SDL_Rect *b, SDL_Rect *out);
SDL_INLINE SDL_bool SDL_RectEmpty(const SDL_Rect *r) { return (!r || r->w <= 0 || r->h <= 0) ? SDL_TRUE : SDL_FALSE; }
SDL_bool SDL_PointInRect(const SDL_Point *p, const SDL_Rect *r);

/* ===================================================================== */
/* Pixels (SDL_pixels.h)                                                  */
/* ===================================================================== */
#define SDL_ALPHA_OPAQUE      255
#define SDL_ALPHA_TRANSPARENT 0
/* Canonical SDL2 pixel-format enum values (used by DevilutionX directly). */
#define SDL_PIXELFORMAT_INDEX8   0x13000001u
#define SDL_PIXELFORMAT_RGB888   0x16161804u
#define SDL_PIXELFORMAT_RGBX8888 0x16261804u
#define SDL_PIXELFORMAT_BGR888   0x16561804u
#define SDL_PIXELFORMAT_ARGB8888 0x16362004u
#define SDL_PIXELFORMAT_RGBA8888 0x16462004u
#define SDL_PIXELFORMAT_ABGR8888 0x16762004u
#define SDL_PIXELFORMAT_BGRA8888 0x16862004u
#define SDL_PIXELFORMAT_RGB565   0x15151002u
#define SDL_PIXELFORMAT_UNKNOWN  0u

typedef struct SDL_Color { Uint8 r, g, b, a; } SDL_Color;

typedef struct SDL_Palette {
	int ncolors;
	SDL_Color *colors;
	Uint32 version;
	int refcount;
} SDL_Palette;

typedef struct SDL_PixelFormat {
	Uint32 format;
	SDL_Palette *palette;
	Uint8 BitsPerPixel;
	Uint8 BytesPerPixel;
	Uint8 padding[2];
	Uint32 Rmask, Gmask, Bmask, Amask;
	Uint8 Rloss, Gloss, Bloss, Aloss;
	Uint8 Rshift, Gshift, Bshift, Ashift;
	int refcount;
	struct SDL_PixelFormat *next;
} SDL_PixelFormat;

SDL_Palette *SDL_AllocPalette(int ncolors);
void SDL_FreePalette(SDL_Palette *palette);
int  SDL_SetPaletteColors(SDL_Palette *palette, const SDL_Color *colors, int firstcolor, int ncolors);
SDL_PixelFormat *SDL_AllocFormat(Uint32 pixel_format);
void SDL_FreeFormat(SDL_PixelFormat *format);
SDL_bool SDL_PixelFormatEnumToMasks(Uint32 format, int *bpp, Uint32 *r, Uint32 *g, Uint32 *b, Uint32 *a);
Uint32 SDL_MapRGB(const SDL_PixelFormat *fmt, Uint8 r, Uint8 g, Uint8 b);
Uint32 SDL_MapRGBA(const SDL_PixelFormat *fmt, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
void SDL_GetRGB(Uint32 pixel, const SDL_PixelFormat *fmt, Uint8 *r, Uint8 *g, Uint8 *b);
void SDL_GetRGBA(Uint32 pixel, const SDL_PixelFormat *fmt, Uint8 *r, Uint8 *g, Uint8 *b, Uint8 *a);

/* ===================================================================== */
/* Surface (SDL_surface.h)                                                */
/* ===================================================================== */
#define SDL_SWSURFACE 0
#define SDL_PREALLOC  0x00000001
#define SDL_RLEACCEL  0x00000002

typedef struct SDL_BlitMap SDL_BlitMap;
typedef struct SDL_Surface {
	Uint32 flags;
	SDL_PixelFormat *format;
	int w, h;
	int pitch;
	void *pixels;
	void *userdata;
	int locked;
	void *list_blitmap;
	SDL_Rect clip_rect;
	SDL_BlitMap *map;
	int refcount;
} SDL_Surface;

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int width, int height, int depth,
    Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask);
SDL_Surface *SDL_CreateRGBSurfaceFrom(void *pixels, int width, int height, int depth, int pitch,
    Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask);
SDL_Surface *SDL_CreateRGBSurfaceWithFormat(Uint32 flags, int width, int height, int depth, Uint32 format);
SDL_Surface *SDL_CreateRGBSurfaceWithFormatFrom(void *pixels, int width, int height, int depth, int pitch, Uint32 format);
void SDL_FreeSurface(SDL_Surface *surface);
int  SDL_LockSurface(SDL_Surface *surface);
void SDL_UnlockSurface(SDL_Surface *surface);
int  SDL_SetSurfacePalette(SDL_Surface *surface, SDL_Palette *palette);
int  SDL_SetColorKey(SDL_Surface *surface, int flag, Uint32 key);
int  SDL_GetColorKey(SDL_Surface *surface, Uint32 *key);
int  SDL_SetSurfaceBlendMode(SDL_Surface *surface, int blendMode);
int  SDL_SetSurfaceAlphaMod(SDL_Surface *surface, Uint8 alpha);
int  SDL_SetClipRect(SDL_Surface *surface, const SDL_Rect *rect);
void SDL_GetClipRect(SDL_Surface *surface, SDL_Rect *rect);
int  SDL_FillRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color);
int  SDL_FillRects(SDL_Surface *dst, const SDL_Rect *rects, int count, Uint32 color);
int  SDL_UpperBlit(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect);
int  SDL_UpperBlitScaled(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect);
int  SDL_SoftStretch(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, const SDL_Rect *dstrect);
SDL_Surface *SDL_ConvertSurface(SDL_Surface *src, const SDL_PixelFormat *fmt, Uint32 flags);
SDL_Surface *SDL_ConvertSurfaceFormat(SDL_Surface *src, Uint32 pixel_format, Uint32 flags);
#define SDL_BlitSurface       SDL_UpperBlit
#define SDL_BlitScaled        SDL_UpperBlitScaled

/* SDL 1.2 palette compat (DevilutionX's sdl_compat.h references these names) */
#define SDL_LOGPAL   0x01
#define SDL_PHYSPAL  0x02
#define SDL_SRCCOLORKEY 0x00001000

/* ===================================================================== */
/* RWops (SDL_rwops.h)                                                    */
/* ===================================================================== */
#define SDL_RWOPS_UNKNOWN   0
#define SDL_RWOPS_STDIO     2
#define SDL_RWOPS_MEMORY    4
#define RW_SEEK_SET 0
#define RW_SEEK_CUR 1
#define RW_SEEK_END 2

typedef struct SDL_RWops {
	Sint64 (*size)(struct SDL_RWops *ctx);
	Sint64 (*seek)(struct SDL_RWops *ctx, Sint64 offset, int whence);
	size_t (*read)(struct SDL_RWops *ctx, void *ptr, size_t size, size_t maxnum);
	size_t (*write)(struct SDL_RWops *ctx, const void *ptr, size_t size, size_t num);
	int (*close)(struct SDL_RWops *ctx);
	Uint32 type;
	union {
		struct { void *data1, *data2; } unknown;
		struct { void *fp; } stdio;
		struct { Uint8 *base, *here, *stop; } mem;
	} hidden;
} SDL_RWops;

SDL_RWops *SDL_RWFromFile(const char *file, const char *mode);
SDL_RWops *SDL_RWFromMem(void *mem, int size);
SDL_RWops *SDL_RWFromConstMem(const void *mem, int size);
SDL_RWops *SDL_AllocRW(void);
void SDL_FreeRW(SDL_RWops *area);
#define SDL_RWsize(ctx)            (ctx)->size(ctx)
#define SDL_RWseek(ctx, off, w)    (ctx)->seek(ctx, (off), (w))
#define SDL_RWtell(ctx)            (ctx)->seek(ctx, 0, RW_SEEK_CUR)
#define SDL_RWread(ctx, p, s, n)   (ctx)->read(ctx, (p), (s), (n))
#define SDL_RWwrite(ctx, p, s, n)  (ctx)->write(ctx, (p), (s), (n))
#define SDL_RWclose(ctx)           (ctx)->close(ctx)
Uint8  SDL_ReadU8(SDL_RWops *src);
Uint16 SDL_ReadLE16(SDL_RWops *src);
Uint32 SDL_ReadLE32(SDL_RWops *src);
void  *SDL_LoadFile_RW(SDL_RWops *src, size_t *datasize, int freesrc);

/* ===================================================================== */
/* Keyboard (SDL_scancode.h / SDL_keycode.h)                              */
/* ===================================================================== */
typedef enum {
	SDL_SCANCODE_UNKNOWN = 0,
	SDL_SCANCODE_A = 4, SDL_SCANCODE_B, SDL_SCANCODE_C, SDL_SCANCODE_D, SDL_SCANCODE_E,
	SDL_SCANCODE_F, SDL_SCANCODE_G, SDL_SCANCODE_H, SDL_SCANCODE_I, SDL_SCANCODE_J,
	SDL_SCANCODE_K, SDL_SCANCODE_L, SDL_SCANCODE_M, SDL_SCANCODE_N, SDL_SCANCODE_O,
	SDL_SCANCODE_P, SDL_SCANCODE_Q, SDL_SCANCODE_R, SDL_SCANCODE_S, SDL_SCANCODE_T,
	SDL_SCANCODE_U, SDL_SCANCODE_V, SDL_SCANCODE_W, SDL_SCANCODE_X, SDL_SCANCODE_Y, SDL_SCANCODE_Z,
	SDL_SCANCODE_1 = 30, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5,
	SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9, SDL_SCANCODE_0,
	SDL_SCANCODE_RETURN = 40, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_TAB, SDL_SCANCODE_SPACE,
	SDL_SCANCODE_MINUS, SDL_SCANCODE_EQUALS, SDL_SCANCODE_LEFTBRACKET, SDL_SCANCODE_RIGHTBRACKET,
	SDL_SCANCODE_BACKSLASH, SDL_SCANCODE_NONUSHASH, SDL_SCANCODE_SEMICOLON, SDL_SCANCODE_APOSTROPHE,
	SDL_SCANCODE_GRAVE, SDL_SCANCODE_COMMA, SDL_SCANCODE_PERIOD, SDL_SCANCODE_SLASH, SDL_SCANCODE_CAPSLOCK,
	SDL_SCANCODE_F1, SDL_SCANCODE_F2, SDL_SCANCODE_F3, SDL_SCANCODE_F4, SDL_SCANCODE_F5, SDL_SCANCODE_F6,
	SDL_SCANCODE_F7, SDL_SCANCODE_F8, SDL_SCANCODE_F9, SDL_SCANCODE_F10, SDL_SCANCODE_F11, SDL_SCANCODE_F12,
	SDL_SCANCODE_PRINTSCREEN, SDL_SCANCODE_SCROLLLOCK, SDL_SCANCODE_PAUSE, SDL_SCANCODE_INSERT,
	SDL_SCANCODE_HOME, SDL_SCANCODE_PAGEUP, SDL_SCANCODE_DELETE, SDL_SCANCODE_END, SDL_SCANCODE_PAGEDOWN,
	SDL_SCANCODE_RIGHT, SDL_SCANCODE_LEFT, SDL_SCANCODE_DOWN, SDL_SCANCODE_UP,
	SDL_SCANCODE_NUMLOCKCLEAR, SDL_SCANCODE_KP_DIVIDE, SDL_SCANCODE_KP_MULTIPLY, SDL_SCANCODE_KP_MINUS,
	SDL_SCANCODE_KP_PLUS, SDL_SCANCODE_KP_ENTER,
	SDL_SCANCODE_KP_1, SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_3, SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_5,
	SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_7, SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_9, SDL_SCANCODE_KP_0,
	SDL_SCANCODE_KP_PERIOD = 99,
	SDL_SCANCODE_KP_EQUALS = 103,
	SDL_SCANCODE_F13 = 104, SDL_SCANCODE_F14, SDL_SCANCODE_F15, SDL_SCANCODE_F16,
	SDL_SCANCODE_F17, SDL_SCANCODE_F18, SDL_SCANCODE_F19, SDL_SCANCODE_F20,
	SDL_SCANCODE_F21, SDL_SCANCODE_F22, SDL_SCANCODE_F23, SDL_SCANCODE_F24,
	SDL_SCANCODE_MENU = 118,
	SDL_SCANCODE_LCTRL = 224, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LALT, SDL_SCANCODE_LGUI,
	SDL_SCANCODE_RCTRL, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RALT, SDL_SCANCODE_RGUI,
	SDL_NUM_SCANCODES = 512
} SDL_Scancode;

typedef Sint32 SDL_Keycode;
#define SDLK_SCANCODE_MASK (1 << 30)
#define SDL_SCANCODE_TO_KEYCODE(X) ((X) | SDLK_SCANCODE_MASK)
enum {
	SDLK_UNKNOWN = 0,
	SDLK_RETURN = '\r', SDLK_ESCAPE = '\x1B', SDLK_BACKSPACE = '\b', SDLK_TAB = '\t', SDLK_SPACE = ' ',
	SDLK_EXCLAIM = '!', SDLK_QUOTEDBL = '"', SDLK_HASH = '#', SDLK_PERCENT = '%', SDLK_DOLLAR = '$',
	SDLK_AMPERSAND = '&', SDLK_QUOTE = '\'', SDLK_LEFTPAREN = '(', SDLK_RIGHTPAREN = ')',
	SDLK_ASTERISK = '*', SDLK_PLUS = '+', SDLK_COMMA = ',', SDLK_MINUS = '-', SDLK_PERIOD = '.',
	SDLK_SLASH = '/', SDLK_0 = '0', SDLK_1, SDLK_2, SDLK_3, SDLK_4, SDLK_5, SDLK_6, SDLK_7, SDLK_8, SDLK_9,
	SDLK_COLON = ':', SDLK_SEMICOLON = ';', SDLK_LESS = '<', SDLK_EQUALS = '=', SDLK_GREATER = '>',
	SDLK_QUESTION = '?', SDLK_AT = '@', SDLK_LEFTBRACKET = '[', SDLK_BACKSLASH = '\\',
	SDLK_RIGHTBRACKET = ']', SDLK_CARET = '^', SDLK_UNDERSCORE = '_', SDLK_BACKQUOTE = '`',
	SDLK_a = 'a', SDLK_b, SDLK_c, SDLK_d, SDLK_e, SDLK_f, SDLK_g, SDLK_h, SDLK_i, SDLK_j, SDLK_k,
	SDLK_l, SDLK_m, SDLK_n, SDLK_o, SDLK_p, SDLK_q, SDLK_r, SDLK_s, SDLK_t, SDLK_u, SDLK_v, SDLK_w,
	SDLK_x, SDLK_y, SDLK_z, SDLK_DELETE = '\x7F',
	SDLK_CAPSLOCK = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_CAPSLOCK),
	SDLK_F1 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F1), SDLK_F2 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F2),
	SDLK_F3 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F3), SDLK_F4 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F4),
	SDLK_F5 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F5), SDLK_F6 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F6),
	SDLK_F7 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F7), SDLK_F8 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F8),
	SDLK_F9 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F9), SDLK_F10 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F10),
	SDLK_F11 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F11), SDLK_F12 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F12),
	SDLK_PRINTSCREEN = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_PRINTSCREEN),
	SDLK_INSERT = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_INSERT),
	SDLK_HOME = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_HOME),
	SDLK_PAGEUP = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_PAGEUP),
	SDLK_END = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_END),
	SDLK_PAGEDOWN = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_PAGEDOWN),
	SDLK_RIGHT = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_RIGHT),
	SDLK_LEFT = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_LEFT),
	SDLK_DOWN = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_DOWN),
	SDLK_UP = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_UP),
	SDLK_KP_ENTER = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_KP_ENTER),
	SDLK_KP_0 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_KP_0),
	SDLK_KP_PLUS = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_KP_PLUS),
	SDLK_KP_MINUS = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_KP_MINUS),
	SDLK_LCTRL = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_LCTRL),
	SDLK_LSHIFT = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_LSHIFT),
	SDLK_LALT = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_LALT),
	SDLK_LGUI = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_LGUI),
	SDLK_RCTRL = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_RCTRL),
	SDLK_RSHIFT = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_RSHIFT),
	SDLK_RALT = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_RALT),
	SDLK_RGUI = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_RGUI),
	SDLK_PAUSE = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_PAUSE),
	SDLK_SCROLLLOCK = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_SCROLLLOCK),
	SDLK_NUMLOCKCLEAR = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_NUMLOCKCLEAR),
	SDLK_KP_PERIOD = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_KP_PERIOD),
	SDLK_KP_DIVIDE = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_KP_DIVIDE),
	SDLK_KP_MULTIPLY = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_KP_MULTIPLY),
	SDLK_KP_EQUALS = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_KP_EQUALS),
	SDLK_F13 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F13),
	SDLK_F14 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F14),
	SDLK_F15 = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F15),
	SDLK_MENU = SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_MENU)
};

typedef enum {
	KMOD_NONE = 0x0000, KMOD_LSHIFT = 0x0001, KMOD_RSHIFT = 0x0002,
	KMOD_LCTRL = 0x0040, KMOD_RCTRL = 0x0080, KMOD_LALT = 0x0100, KMOD_RALT = 0x0200,
	KMOD_LGUI = 0x0400, KMOD_RGUI = 0x0800, KMOD_NUM = 0x1000, KMOD_CAPS = 0x2000,
	KMOD_CTRL = KMOD_LCTRL | KMOD_RCTRL, KMOD_SHIFT = KMOD_LSHIFT | KMOD_RSHIFT,
	KMOD_ALT = KMOD_LALT | KMOD_RALT, KMOD_GUI = KMOD_LGUI | KMOD_RGUI
} SDL_Keymod;

typedef struct SDL_Keysym {
	SDL_Scancode scancode;
	SDL_Keycode sym;
	Uint16 mod;
	Uint32 unused;
} SDL_Keysym;

const Uint8 *SDL_GetKeyboardState(int *numkeys);
SDL_Keymod SDL_GetModState(void);
void SDL_SetModState(SDL_Keymod modstate);
SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode scancode);
SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode key);
const char *SDL_GetKeyName(SDL_Keycode key);
const char *SDL_GetScancodeName(SDL_Scancode scancode);
void SDL_StartTextInput(void);
void SDL_StopTextInput(void);
SDL_bool SDL_IsTextInputActive(void);
void SDL_SetTextInputRect(SDL_Rect *rect);
SDL_bool SDL_HasScreenKeyboardSupport(void);

/* ===================================================================== */
/* Mouse (SDL_mouse.h)                                                    */
/* ===================================================================== */
#define SDL_BUTTON(X)      (1u << ((X) - 1))
#define SDL_BUTTON_LEFT    1
#define SDL_BUTTON_MIDDLE  2
#define SDL_BUTTON_RIGHT   3
#define SDL_BUTTON_X1      4
#define SDL_BUTTON_X2      5
#define SDL_BUTTON_LMASK   SDL_BUTTON(SDL_BUTTON_LEFT)
#define SDL_BUTTON_MMASK   SDL_BUTTON(SDL_BUTTON_MIDDLE)
#define SDL_BUTTON_RMASK   SDL_BUTTON(SDL_BUTTON_RIGHT)
#define SDL_PRESSED  1
#define SDL_RELEASED 0
typedef struct SDL_Cursor SDL_Cursor;
typedef enum { SDL_SYSTEM_CURSOR_ARROW, SDL_SYSTEM_CURSOR_HAND, SDL_NUM_SYSTEM_CURSORS } SDL_SystemCursor;
Uint32 SDL_GetMouseState(int *x, int *y);
Uint32 SDL_GetGlobalMouseState(int *x, int *y);
struct SDL_Window;
void SDL_WarpMouseInWindow(struct SDL_Window *window, int x, int y);
int  SDL_ShowCursor(int toggle);
#define SDL_QUERY  -1
#define SDL_IGNORE  0
#define SDL_DISABLE 0
#define SDL_ENABLE  1
SDL_Cursor *SDL_CreateCursor(const Uint8 *data, const Uint8 *mask, int w, int h, int hot_x, int hot_y);
SDL_Cursor *SDL_CreateColorCursor(SDL_Surface *surface, int hot_x, int hot_y);
SDL_Cursor *SDL_CreateSystemCursor(SDL_SystemCursor id);
void SDL_SetCursor(SDL_Cursor *cursor);
SDL_Cursor *SDL_GetDefaultCursor(void);
void SDL_FreeCursor(SDL_Cursor *cursor);
int  SDL_CaptureMouse(SDL_bool enabled);
#define SDL_TOUCH_MOUSEID ((Uint32)-1)

/* ===================================================================== */
/* Joystick / Game Controller                                             */
/* ===================================================================== */
typedef Sint32 SDL_JoystickID;
typedef struct SDL_Joystick SDL_Joystick;
typedef struct SDL_GameController SDL_GameController;
#define SDL_HAT_CENTERED 0x00
#define SDL_HAT_UP    0x01
#define SDL_HAT_RIGHT 0x02
#define SDL_HAT_DOWN  0x04
#define SDL_HAT_LEFT  0x08
#define SDL_JOYSTICK_AXIS_MAX  32767
#define SDL_JOYSTICK_AXIS_MIN  (-32768)

typedef enum {
	SDL_CONTROLLER_BUTTON_INVALID = -1,
	SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
	SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_GUIDE, SDL_CONTROLLER_BUTTON_START,
	SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
	SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
	SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
	SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
	SDL_CONTROLLER_BUTTON_MISC1, SDL_CONTROLLER_BUTTON_PADDLE1, SDL_CONTROLLER_BUTTON_PADDLE2,
	SDL_CONTROLLER_BUTTON_PADDLE3, SDL_CONTROLLER_BUTTON_PADDLE4, SDL_CONTROLLER_BUTTON_TOUCHPAD,
	SDL_CONTROLLER_BUTTON_MAX
} SDL_GameControllerButton;
typedef enum {
	SDL_CONTROLLER_AXIS_INVALID = -1,
	SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY,
	SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY,
	SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
	SDL_CONTROLLER_AXIS_MAX
} SDL_GameControllerAxis;
typedef enum {
	SDL_CONTROLLER_BINDTYPE_NONE = 0, SDL_CONTROLLER_BINDTYPE_BUTTON,
	SDL_CONTROLLER_BINDTYPE_AXIS, SDL_CONTROLLER_BINDTYPE_HAT
} SDL_GameControllerBindType;
typedef struct SDL_GameControllerButtonBind {
	SDL_GameControllerBindType bindType;
	union { int button; int axis; struct { int hat, hat_mask; } hat; } value;
} SDL_GameControllerButtonBind;

int  SDL_NumJoysticks(void);
SDL_bool SDL_IsGameController(int joystick_index);
SDL_GameController *SDL_GameControllerOpen(int joystick_index);
void SDL_GameControllerClose(SDL_GameController *gamecontroller);
SDL_GameController *SDL_GameControllerFromInstanceID(SDL_JoystickID joyid);
const char *SDL_GameControllerName(SDL_GameController *gamecontroller);
SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *gamecontroller);
SDL_bool SDL_GameControllerGetButton(SDL_GameController *gamecontroller, SDL_GameControllerButton button);
Sint16 SDL_GameControllerGetAxis(SDL_GameController *gamecontroller, SDL_GameControllerAxis axis);
SDL_bool SDL_GameControllerHasButton(SDL_GameController *gamecontroller, SDL_GameControllerButton button);
SDL_GameControllerButtonBind SDL_GameControllerGetBindForButton(SDL_GameController *gamecontroller, SDL_GameControllerButton button);
const char *SDL_GameControllerGetStringForButton(SDL_GameControllerButton button);
SDL_GameControllerButton SDL_GameControllerGetButtonFromString(const char *str);
const char *SDL_GameControllerGetStringForAxis(SDL_GameControllerAxis axis);
SDL_GameControllerAxis SDL_GameControllerGetAxisFromString(const char *str);
int  SDL_GameControllerAddMapping(const char *mappingString);
void SDL_GameControllerUpdate(void);
SDL_Joystick *SDL_JoystickOpen(int device_index);
SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *joystick);
const char *SDL_JoystickName(SDL_Joystick *joystick);

/* ===================================================================== */
/* Events (SDL_events.h)                                                  */
/* ===================================================================== */
typedef enum {
	SDL_FIRSTEVENT = 0,
	SDL_QUIT = 0x100,
	SDL_APP_TERMINATING, SDL_APP_LOWMEMORY, SDL_APP_WILLENTERBACKGROUND,
	SDL_APP_DIDENTERBACKGROUND, SDL_APP_WILLENTERFOREGROUND, SDL_APP_DIDENTERFOREGROUND,
	SDL_WINDOWEVENT = 0x200, SDL_SYSWMEVENT,
	SDL_KEYDOWN = 0x300, SDL_KEYUP, SDL_TEXTEDITING, SDL_TEXTINPUT, SDL_KEYMAPCHANGED,
	SDL_MOUSEMOTION = 0x400, SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP, SDL_MOUSEWHEEL,
	SDL_JOYAXISMOTION = 0x600, SDL_JOYBALLMOTION, SDL_JOYHATMOTION, SDL_JOYBUTTONDOWN,
	SDL_JOYBUTTONUP, SDL_JOYDEVICEADDED, SDL_JOYDEVICEREMOVED,
	SDL_CONTROLLERAXISMOTION = 0x650, SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLERBUTTONUP,
	SDL_CONTROLLERDEVICEADDED, SDL_CONTROLLERDEVICEREMOVED, SDL_CONTROLLERDEVICEREMAPPED,
	SDL_FINGERDOWN = 0x700, SDL_FINGERUP, SDL_FINGERMOTION,
	SDL_CLIPBOARDUPDATE = 0x900,
	SDL_DROPFILE = 0x1000, SDL_DROPTEXT, SDL_DROPBEGIN, SDL_DROPCOMPLETE,
	SDL_AUDIODEVICEADDED = 0x1100, SDL_AUDIODEVICEREMOVED,
	SDL_RENDER_TARGETS_RESET = 0x2000, SDL_RENDER_DEVICE_RESET,
	SDL_USEREVENT = 0x8000,
	SDL_LASTEVENT = 0xFFFF
} SDL_EventType;

typedef enum {
	SDL_WINDOWEVENT_NONE, SDL_WINDOWEVENT_SHOWN, SDL_WINDOWEVENT_HIDDEN, SDL_WINDOWEVENT_EXPOSED,
	SDL_WINDOWEVENT_MOVED, SDL_WINDOWEVENT_RESIZED, SDL_WINDOWEVENT_SIZE_CHANGED,
	SDL_WINDOWEVENT_MINIMIZED, SDL_WINDOWEVENT_MAXIMIZED, SDL_WINDOWEVENT_RESTORED,
	SDL_WINDOWEVENT_ENTER, SDL_WINDOWEVENT_LEAVE, SDL_WINDOWEVENT_FOCUS_GAINED,
	SDL_WINDOWEVENT_FOCUS_LOST, SDL_WINDOWEVENT_CLOSE, SDL_WINDOWEVENT_TAKE_FOCUS, SDL_WINDOWEVENT_HIT_TEST
} SDL_WindowEventID;

typedef Sint64 SDL_TouchID;
typedef Sint64 SDL_FingerID;

typedef struct SDL_CommonEvent { Uint32 type; Uint32 timestamp; } SDL_CommonEvent;
typedef struct SDL_WindowEvent { Uint32 type, timestamp, windowID; Uint8 event, padding1, padding2, padding3; Sint32 data1, data2; } SDL_WindowEvent;
typedef struct SDL_KeyboardEvent { Uint32 type, timestamp, windowID; Uint8 state, repeat, padding2, padding3; SDL_Keysym keysym; } SDL_KeyboardEvent;
#define SDL_TEXTINPUTEVENT_TEXT_SIZE 32
typedef struct SDL_TextInputEvent { Uint32 type, timestamp, windowID; char text[SDL_TEXTINPUTEVENT_TEXT_SIZE]; } SDL_TextInputEvent;
#define SDL_TEXTEDITINGEVENT_TEXT_SIZE 32
typedef struct SDL_TextEditingEvent { Uint32 type, timestamp, windowID; char text[SDL_TEXTEDITINGEVENT_TEXT_SIZE]; Sint32 start, length; } SDL_TextEditingEvent;
typedef struct SDL_MouseMotionEvent { Uint32 type, timestamp, windowID, which; Uint32 state; Sint32 x, y, xrel, yrel; } SDL_MouseMotionEvent;
typedef struct SDL_MouseButtonEvent { Uint32 type, timestamp, windowID, which; Uint8 button, state, clicks, padding1; Sint32 x, y; } SDL_MouseButtonEvent;
typedef struct SDL_MouseWheelEvent { Uint32 type, timestamp, windowID, which; Sint32 x, y; Uint32 direction; float preciseX, preciseY; } SDL_MouseWheelEvent;
typedef struct SDL_JoyAxisEvent { Uint32 type, timestamp; SDL_JoystickID which; Uint8 axis, padding1, padding2, padding3; Sint16 value; Uint16 padding4; } SDL_JoyAxisEvent;
typedef struct SDL_JoyButtonEvent { Uint32 type, timestamp; SDL_JoystickID which; Uint8 button, state, padding1, padding2; } SDL_JoyButtonEvent;
typedef struct SDL_JoyBallEvent { Uint32 type, timestamp; SDL_JoystickID which; Uint8 ball, padding1, padding2, padding3; Sint16 xrel, yrel; } SDL_JoyBallEvent;
typedef struct SDL_JoyHatEvent { Uint32 type, timestamp; SDL_JoystickID which; Uint8 hat, value, padding1, padding2; } SDL_JoyHatEvent;
typedef struct SDL_JoyDeviceEvent { Uint32 type, timestamp; Sint32 which; } SDL_JoyDeviceEvent;
typedef struct SDL_ControllerAxisEvent { Uint32 type, timestamp; SDL_JoystickID which; Uint8 axis, padding1, padding2, padding3; Sint16 value; Uint16 padding4; } SDL_ControllerAxisEvent;
typedef struct SDL_ControllerButtonEvent { Uint32 type, timestamp; SDL_JoystickID which; Uint8 button, state, padding1, padding2; } SDL_ControllerButtonEvent;
typedef struct SDL_ControllerDeviceEvent { Uint32 type, timestamp; Sint32 which; } SDL_ControllerDeviceEvent;
typedef struct SDL_TouchFingerEvent { Uint32 type, timestamp; SDL_TouchID touchId; SDL_FingerID fingerId; float x, y, dx, dy, pressure; Uint32 windowID; } SDL_TouchFingerEvent;
typedef struct SDL_QuitEvent { Uint32 type, timestamp; } SDL_QuitEvent;
typedef struct SDL_UserEvent { Uint32 type, timestamp, windowID; Sint32 code; void *data1, *data2; } SDL_UserEvent;
typedef struct SDL_DropEvent { Uint32 type, timestamp; char *file; Uint32 windowID; } SDL_DropEvent;
typedef struct SDL_AudioDeviceEvent { Uint32 type, timestamp, which; Uint8 iscapture, padding1, padding2, padding3; } SDL_AudioDeviceEvent;

typedef union SDL_Event {
	Uint32 type;
	SDL_CommonEvent common;
	SDL_WindowEvent window;
	SDL_KeyboardEvent key;
	SDL_TextEditingEvent edit;
	SDL_TextInputEvent text;
	SDL_MouseMotionEvent motion;
	SDL_MouseButtonEvent button;
	SDL_MouseWheelEvent wheel;
	SDL_JoyAxisEvent jaxis;
	SDL_JoyBallEvent jball;
	SDL_JoyButtonEvent jbutton;
	SDL_JoyHatEvent jhat;
	SDL_JoyDeviceEvent jdevice;
	SDL_ControllerAxisEvent caxis;
	SDL_ControllerButtonEvent cbutton;
	SDL_ControllerDeviceEvent cdevice;
	SDL_TouchFingerEvent tfinger;
	SDL_QuitEvent quit;
	SDL_UserEvent user;
	SDL_DropEvent drop;
	SDL_AudioDeviceEvent adevice;
	Uint8 padding[56];
} SDL_Event;

typedef enum { SDL_ADDEVENT, SDL_PEEKEVENT, SDL_GETEVENT } SDL_eventaction;
typedef int (*SDL_EventFilter)(void *userdata, SDL_Event *event);

int  SDL_PollEvent(SDL_Event *event);
int  SDL_WaitEvent(SDL_Event *event);
int  SDL_WaitEventTimeout(SDL_Event *event, int timeout);
int  SDL_PushEvent(SDL_Event *event);
void SDL_PumpEvents(void);
int  SDL_PeepEvents(SDL_Event *events, int numevents, SDL_eventaction action, Uint32 minType, Uint32 maxType);
Uint32 SDL_RegisterEvents(int numevents);
SDL_bool SDL_HasEvent(Uint32 type);
void SDL_FlushEvent(Uint32 type);
void SDL_FlushEvents(Uint32 minType, Uint32 maxType);
Uint8 SDL_EventState(Uint32 type, int state);
void SDL_SetEventFilter(SDL_EventFilter filter, void *userdata);

/* ===================================================================== */
/* Video / Window (SDL_video.h)                                           */
/* ===================================================================== */
typedef struct SDL_Window SDL_Window;
typedef struct SDL_DisplayMode { Uint32 format; int w, h, refresh_rate; void *driverdata; } SDL_DisplayMode;
#define SDL_WINDOWPOS_UNDEFINED 0x1FFF0000u
#define SDL_WINDOWPOS_CENTERED  0x2FFF0000u
typedef enum {
	SDL_WINDOW_FULLSCREEN = 0x00000001, SDL_WINDOW_OPENGL = 0x00000002, SDL_WINDOW_SHOWN = 0x00000004,
	SDL_WINDOW_HIDDEN = 0x00000008, SDL_WINDOW_BORDERLESS = 0x00000010, SDL_WINDOW_RESIZABLE = 0x00000020,
	SDL_WINDOW_MINIMIZED = 0x00000040, SDL_WINDOW_MAXIMIZED = 0x00000080,
	SDL_WINDOW_FULLSCREEN_DESKTOP = 0x00001001, SDL_WINDOW_ALLOW_HIGHDPI = 0x00002000
} SDL_WindowFlags;
#define SDL_FULLSCREEN SDL_WINDOW_FULLSCREEN

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags);
void SDL_DestroyWindow(SDL_Window *window);
SDL_Surface *SDL_GetWindowSurface(SDL_Window *window);
int  SDL_UpdateWindowSurface(SDL_Window *window);
int  SDL_UpdateWindowSurfaceRects(SDL_Window *window, const SDL_Rect *rects, int numrects);
void SDL_SetWindowTitle(SDL_Window *window, const char *title);
void SDL_GetWindowSize(SDL_Window *window, int *w, int *h);
void SDL_SetWindowSize(SDL_Window *window, int w, int h);
Uint32 SDL_GetWindowFlags(SDL_Window *window);
int  SDL_SetWindowFullscreen(SDL_Window *window, Uint32 flags);
void SDL_SetWindowResizable(SDL_Window *window, SDL_bool resizable);
Uint32 SDL_GetWindowID(SDL_Window *window);
SDL_Window *SDL_GetWindowFromID(Uint32 id);
void SDL_GetWindowPosition(SDL_Window *window, int *x, int *y);
void SDL_SetWindowPosition(SDL_Window *window, int x, int y);
SDL_Surface *SDL_GetVideoSurface(void);
int  SDL_GetCurrentDisplayMode(int displayIndex, SDL_DisplayMode *mode);
int  SDL_GetDesktopDisplayMode(int displayIndex, SDL_DisplayMode *mode);
int  SDL_GetDisplayBounds(int displayIndex, SDL_Rect *rect);
int  SDL_GetNumVideoDisplays(void);
int  SDL_GetNumDisplayModes(int displayIndex);
int  SDL_GetDisplayMode(int displayIndex, int modeIndex, SDL_DisplayMode *mode);
const char *SDL_GetCurrentVideoDriver(void);

/* ===================================================================== */
/* Renderer / Texture (SDL_render.h)                                      */
/* ===================================================================== */
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;
typedef enum {
	SDL_RENDERER_SOFTWARE = 0x01, SDL_RENDERER_ACCELERATED = 0x02,
	SDL_RENDERER_PRESENTVSYNC = 0x04, SDL_RENDERER_TARGETTEXTURE = 0x08
} SDL_RendererFlags;
typedef enum { SDL_TEXTUREACCESS_STATIC, SDL_TEXTUREACCESS_STREAMING, SDL_TEXTUREACCESS_TARGET } SDL_TextureAccess;
typedef struct SDL_RendererInfo { const char *name; Uint32 flags; Uint32 num_texture_formats; Uint32 texture_formats[16]; int max_texture_width, max_texture_height; } SDL_RendererInfo;

SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags);
void SDL_DestroyRenderer(SDL_Renderer *renderer);
int  SDL_GetRendererInfo(SDL_Renderer *renderer, SDL_RendererInfo *info);
int  SDL_RenderSetLogicalSize(SDL_Renderer *renderer, int w, int h);
void SDL_RenderGetLogicalSize(SDL_Renderer *renderer, int *w, int *h);
int  SDL_RenderSetIntegerScale(SDL_Renderer *renderer, SDL_bool enable);
void SDL_RenderGetScale(SDL_Renderer *renderer, float *scaleX, float *scaleY);
void SDL_RenderGetViewport(SDL_Renderer *renderer, SDL_Rect *rect);
int  SDL_RenderSetViewport(SDL_Renderer *renderer, const SDL_Rect *rect);
int  SDL_SetRenderDrawColor(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
int  SDL_RenderClear(SDL_Renderer *renderer);
int  SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *srcrect, const SDL_Rect *dstrect);
void SDL_RenderPresent(SDL_Renderer *renderer);
int  SDL_RenderReadPixels(SDL_Renderer *renderer, const SDL_Rect *rect, Uint32 format, void *pixels, int pitch);
SDL_Texture *SDL_CreateTexture(SDL_Renderer *renderer, Uint32 format, int access, int w, int h);
SDL_Texture *SDL_CreateTextureFromSurface(SDL_Renderer *renderer, SDL_Surface *surface);
void SDL_DestroyTexture(SDL_Texture *texture);
int  SDL_UpdateTexture(SDL_Texture *texture, const SDL_Rect *rect, const void *pixels, int pitch);
int  SDL_LockTexture(SDL_Texture *texture, const SDL_Rect *rect, void **pixels, int *pitch);
void SDL_UnlockTexture(SDL_Texture *texture);
int  SDL_SetTextureBlendMode(SDL_Texture *texture, int blendMode);
int  SDL_QueryTexture(SDL_Texture *texture, Uint32 *format, int *access, int *w, int *h);

/* ===================================================================== */
/* Timer (SDL_timer.h)                                                    */
/* ===================================================================== */
Uint32 SDL_GetTicks(void);
Uint64 SDL_GetTicks64(void);
Uint64 SDL_GetPerformanceCounter(void);
Uint64 SDL_GetPerformanceFrequency(void);
void   SDL_Delay(Uint32 ms);
typedef Uint32 (*SDL_TimerCallback)(Uint32 interval, void *param);
typedef int SDL_TimerID;
SDL_TimerID SDL_AddTimer(Uint32 interval, SDL_TimerCallback callback, void *param);
SDL_bool SDL_RemoveTimer(SDL_TimerID id);

/* ===================================================================== */
/* Threads / Mutexes / Cond / Semaphores (SDL_thread.h, SDL_mutex.h)      */
/*                                                                        */
/* The Pocket softcore is single-threaded (no preemptive RTOS). These are */
/* declared so DevilutionX's SdlThread/SdlMutex wrappers compile; the     */
/* implementation in of_sdl2.cpp runs thread bodies cooperatively.        */
/* ===================================================================== */
typedef struct SDL_Thread SDL_Thread;
typedef unsigned long SDL_threadID;
typedef int (SDLCALL *SDL_ThreadFunction)(void *data);
SDL_Thread  *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data);
void         SDL_WaitThread(SDL_Thread *thread, int *status);
void         SDL_DetachThread(SDL_Thread *thread);
SDL_threadID SDL_GetThreadID(SDL_Thread *thread);
SDL_threadID SDL_ThreadID(void);
const char  *SDL_GetThreadName(SDL_Thread *thread);

#define SDL_MUTEX_TIMEDOUT 1
#define SDL_MUTEX_MAXWAIT  (~(Uint32)0)
typedef struct SDL_mutex SDL_mutex;
SDL_mutex *SDL_CreateMutex(void);
int   SDL_LockMutex(SDL_mutex *mutex);
int   SDL_TryLockMutex(SDL_mutex *mutex);
int   SDL_UnlockMutex(SDL_mutex *mutex);
void  SDL_DestroyMutex(SDL_mutex *mutex);

typedef struct SDL_cond SDL_cond;
SDL_cond *SDL_CreateCond(void);
void SDL_DestroyCond(SDL_cond *cond);
int  SDL_CondSignal(SDL_cond *cond);
int  SDL_CondBroadcast(SDL_cond *cond);
int  SDL_CondWait(SDL_cond *cond, SDL_mutex *mutex);
int  SDL_CondWaitTimeout(SDL_cond *cond, SDL_mutex *mutex, Uint32 ms);

typedef struct SDL_sem SDL_sem;
SDL_sem *SDL_CreateSemaphore(Uint32 initial_value);
void SDL_DestroySemaphore(SDL_sem *sem);
int  SDL_SemWait(SDL_sem *sem);
int  SDL_SemTryWait(SDL_sem *sem);
int  SDL_SemWaitTimeout(SDL_sem *sem, Uint32 ms);
int  SDL_SemPost(SDL_sem *sem);
Uint32 SDL_SemValue(SDL_sem *sem);

/* ===================================================================== */
/* Hints / MessageBox / Filesystem / CPU                                  */
/* ===================================================================== */
#define SDL_HINT_RENDER_SCALE_QUALITY "SDL_RENDER_SCALE_QUALITY"
#define SDL_HINT_ORIENTATIONS "SDL_IOS_ORIENTATIONS"
typedef enum { SDL_HINT_DEFAULT, SDL_HINT_NORMAL, SDL_HINT_OVERRIDE } SDL_HintPriority;
SDL_bool SDL_SetHint(const char *name, const char *value);
SDL_bool SDL_SetHintWithPriority(const char *name, const char *value, SDL_HintPriority priority);
const char *SDL_GetHint(const char *name);

#define SDL_MESSAGEBOX_ERROR       0x10
#define SDL_MESSAGEBOX_WARNING     0x20
#define SDL_MESSAGEBOX_INFORMATION 0x40
int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window);

char *SDL_GetBasePath(void);
char *SDL_GetPrefPath(const char *org, const char *app);
int   SDL_GetCPUCount(void);
int   SDL_GetSystemRAM(void);

/* ===================================================================== */
/* Audio (SDL_audio.h) -- declared for completeness; NOSOUND build path    */
/* leaves these unused. Real wiring lands with the audio shim.            */
/* ===================================================================== */
typedef Uint16 SDL_AudioFormat;
#define AUDIO_U8     0x0008
#define AUDIO_S8     0x8008
#define AUDIO_S16LSB 0x8010
#define AUDIO_S16SYS 0x8010
#define AUDIO_S16    0x8010
#define AUDIO_S32SYS 0x8020
#define AUDIO_F32SYS 0x8120
typedef Uint32 SDL_AudioDeviceID;
typedef void (*SDL_AudioCallback)(void *userdata, Uint8 *stream, int len);
typedef struct SDL_AudioSpec {
	int freq; SDL_AudioFormat format; Uint8 channels, silence; Uint16 samples, padding;
	Uint32 size; SDL_AudioCallback callback; void *userdata;
} SDL_AudioSpec;
SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture, const SDL_AudioSpec *desired, SDL_AudioSpec *obtained, int allowed_changes);
void SDL_CloseAudioDevice(SDL_AudioDeviceID dev);
void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on);
void SDL_LockAudioDevice(SDL_AudioDeviceID dev);
void SDL_UnlockAudioDevice(SDL_AudioDeviceID dev);
int  SDL_QueueAudio(SDL_AudioDeviceID dev, const void *data, Uint32 len);
Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID dev);
void SDL_ClearQueuedAudio(SDL_AudioDeviceID dev);
int  SDL_GetNumAudioDevices(int iscapture);
const char *SDL_GetAudioDeviceName(int index, int iscapture);

/* ===================================================================== */
/* Window / display / clipboard / joystick extras                        */
/* ===================================================================== */
void SDL_ShowWindow(SDL_Window *window);
void SDL_HideWindow(SDL_Window *window);
void SDL_RaiseWindow(SDL_Window *window);
void SDL_RestoreWindow(SDL_Window *window);
void SDL_MaximizeWindow(SDL_Window *window);
void SDL_MinimizeWindow(SDL_Window *window);
void SDL_SetWindowGrab(SDL_Window *window, SDL_bool grabbed);
int  SDL_GetWindowDisplayIndex(SDL_Window *window);
int  SDL_GetWindowDisplayMode(SDL_Window *window, SDL_DisplayMode *mode);
int  SDL_SetWindowDisplayMode(SDL_Window *window, const SDL_DisplayMode *mode);
SDL_Window *SDL_GetKeyboardFocus(void);
int  SDL_GetRendererOutputSize(SDL_Renderer *renderer, int *w, int *h);
int  SDL_GetDisplayDPI(int displayIndex, float *ddpi, float *hdpi, float *vdpi);
void SDL_DisableScreenSaver(void);
void SDL_EnableScreenSaver(void);

int  SDL_SetClipboardText(const char *text);
char *SDL_GetClipboardText(void);
SDL_bool SDL_HasClipboardText(void);

#define SDL_MUSTLOCK(S)      (((S)->flags & SDL_RLEACCEL) != 0)
#define SDL_BITSPERPIXEL(X)  (((X) >> 8) & 0xFF)
#define SDL_BYTESPERPIXEL(X) ((X) & 0xFF)

/* Joystick */
typedef struct SDL_JoystickGUID { Uint8 data[16]; } SDL_JoystickGUID;
const char *SDL_JoystickNameForIndex(int device_index);
int    SDL_JoystickNumButtons(SDL_Joystick *joystick);
int    SDL_JoystickNumAxes(SDL_Joystick *joystick);
int    SDL_JoystickNumHats(SDL_Joystick *joystick);
Uint8  SDL_JoystickGetButton(SDL_Joystick *joystick, int button);
Sint16 SDL_JoystickGetAxis(SDL_Joystick *joystick, int axis);
Uint8  SDL_JoystickGetHat(SDL_Joystick *joystick, int hat);
SDL_JoystickGUID SDL_JoystickGetGUID(SDL_Joystick *joystick);
void   SDL_JoystickClose(SDL_Joystick *joystick);

/* Game controller type */
typedef enum {
	SDL_CONTROLLER_TYPE_UNKNOWN = 0, SDL_CONTROLLER_TYPE_XBOX360, SDL_CONTROLLER_TYPE_XBOXONE,
	SDL_CONTROLLER_TYPE_PS3, SDL_CONTROLLER_TYPE_PS4, SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO,
	SDL_CONTROLLER_TYPE_VIRTUAL, SDL_CONTROLLER_TYPE_PS5, SDL_CONTROLLER_TYPE_AMAZON_LUNA,
	SDL_CONTROLLER_TYPE_GOOGLE_STADIA, SDL_CONTROLLER_TYPE_NVIDIA_SHIELD,
	SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT, SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT,
	SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR
} SDL_GameControllerType;
SDL_GameControllerType SDL_GameControllerGetType(SDL_GameController *gamecontroller);
SDL_GameControllerType SDL_GameControllerTypeForIndex(int joystick_index);
char *SDL_GameControllerMappingForGUID(SDL_JoystickGUID guid);
char *SDL_GameControllerMapping(SDL_GameController *gamecontroller);

/* SDL1 leftovers referenced from a few compatibility paths (no-ops here) */
void SDL_WarpMouse(Uint16 x, Uint16 y);
int  SDL_EnableUNICODE(int enable);

/* Extra hint names referenced by DevilutionX */
#define SDL_HINT_MOUSE_TOUCH_EVENTS "SDL_MOUSE_TOUCH_EVENTS"
#define SDL_HINT_TOUCH_MOUSE_EVENTS "SDL_TOUCH_MOUSE_EVENTS"
#define SDL_HINT_IME_INTERNAL_EDITING "SDL_IME_INTERNAL_EDITING"
#define SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS "SDL_GAMECONTROLLER_USE_BUTTON_LABELS"
#define SDL_HINT_ACCELEROMETER_AS_JOYSTICK "SDL_ACCELEROMETER_AS_JOYSTICK"
#define SDL_HINT_RENDER_VSYNC "SDL_RENDER_VSYNC"
#define SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS "SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS"


/* =====================================================================
 * openfpgaOS additions -- broaden coverage beyond the DevilutionX subset
 * so SDL 1.2-style and renderer/audio-heavy ports build unmodified.
 * (All implemented in of_sdl2.c.)
 * ===================================================================== */

/* extra pixel format aliases */
#define SDL_PIXELFORMAT_RGBA32 SDL_PIXELFORMAT_ABGR8888
#define SDL_PIXELFORMAT_BGRA32 SDL_PIXELFORMAT_ARGB8888
#define SDL_PIXELFORMAT_ARGB32 SDL_PIXELFORMAT_BGRA8888
#define SDL_PIXELFORMAT_ABGR32 SDL_PIXELFORMAT_RGBA8888

/* init extras */
#define SDL_INIT_NOPARACHUTE 0x00100000u
const char *SDL_GetPlatform(void);
int  SDL_GetNumTouchDevices(void);

/* SDL 1.2 video compat */
SDL_Surface *SDL_SetVideoMode(int width, int height, int bpp, Uint32 flags);
void SDL_Flip(SDL_Surface *screen);
int  SDL_VideoModeOK(int width, int height, int bpp, Uint32 flags);
SDL_Rect **SDL_ListModes(SDL_PixelFormat *format, Uint32 flags);
void SDL_WM_SetCaption(const char *title, const char *icon);
int  SDL_WM_ToggleFullScreen(SDL_Surface *surface);
void SDL_WM_SetIcon(SDL_Surface *icon, Uint8 *mask);
void SDL_SetWindowIcon(SDL_Window *window, SDL_Surface *icon);
Uint32 SDL_GetWindowPixelFormat(SDL_Window *window);
#define SDL_HWSURFACE  0x00000001
#define SDL_DOUBLEBUF  0x40000000
#define SDL_HWPALETTE  0x20000000
#define SDL_ANYFORMAT  0x10000000
#define SDL_RESIZABLE  0x00000010

/* SDL 1.2 palette compat */
int  SDL_SetColors(SDL_Surface *surface, const SDL_Color *colors, int firstcolor, int ncolors);
int  SDL_SetPalette(SDL_Surface *surface, int flags, const SDL_Color *colors, int firstcolor, int ncolors);

/* openfpgaOS extension: track an offscreen render palette (DevilutionX). */
void of_sdl_set_screen_palette_is_render(int enabled);

/* keyboard alias + relative mouse */
#define SDL_GetKeyState SDL_GetKeyboardState
Uint32 SDL_GetRelativeMouseState(int *x, int *y);
int    SDL_SetRelativeMouseMode(SDL_bool enabled);
void   SDL_WarpMouseGlobal(int x, int y);

/* joystick / controller event-state toggles */
int  SDL_JoystickEventState(int state);
int  SDL_GameControllerEventState(int state);
void SDL_JoystickUpdate(void);
SDL_bool SDL_JoystickGetAttached(SDL_Joystick *joystick);

/* renderer extras */
typedef enum { SDL_FLIP_NONE = 0, SDL_FLIP_HORIZONTAL = 1, SDL_FLIP_VERTICAL = 2 } SDL_RendererFlip;
int  SDL_RenderCopyEx(SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *src,
                      const SDL_Rect *dst, double angle, const SDL_Point *center,
                      SDL_RendererFlip flip);
int  SDL_RenderDrawPoint(SDL_Renderer *r, int x, int y);
int  SDL_RenderDrawLine(SDL_Renderer *r, int x1, int y1, int x2, int y2);
int  SDL_RenderDrawRect(SDL_Renderer *r, const SDL_Rect *rect);
int  SDL_RenderFillRect(SDL_Renderer *r, const SDL_Rect *rect);
int  SDL_RenderSetClipRect(SDL_Renderer *r, const SDL_Rect *rect);
int  SDL_SetRenderTarget(SDL_Renderer *r, SDL_Texture *texture);
int  SDL_SetTextureColorMod(SDL_Texture *t, Uint8 red, Uint8 g, Uint8 b);
int  SDL_SetTextureAlphaMod(SDL_Texture *t, Uint8 alpha);
int  SDL_GetTextureBlendMode(SDL_Texture *t, int *blendMode);

/* blend modes */
typedef enum { SDL_BLENDMODE_NONE = 0, SDL_BLENDMODE_BLEND = 1,
               SDL_BLENDMODE_ADD = 2, SDL_BLENDMODE_MOD = 4 } SDL_BlendMode;

/* BMP I/O (minimal) */
SDL_Surface *SDL_LoadBMP_RW(SDL_RWops *src, int freesrc);
int          SDL_SaveBMP_RW(SDL_Surface *surface, SDL_RWops *dst, int freedst);
#define SDL_LoadBMP(file) SDL_LoadBMP_RW(SDL_RWFromFile(file, "rb"), 1)
#define SDL_SaveBMP(surf, file) SDL_SaveBMP_RW(surf, SDL_RWFromFile(file, "wb"), 1)

/* Audio (full): callback (auto-pumped), queue, WAV load, CVT, mix. */
#define SDL_MIX_MAXVOLUME 128
typedef struct SDL_AudioCVT {
	int needed;
	SDL_AudioFormat src_format;
	SDL_AudioFormat dst_format;
	double rate_incr;
	Uint8 *buf;
	int len;
	int len_cvt;
	int len_mult;
	double len_ratio;
	void (*filters[10])(struct SDL_AudioCVT *cvt, SDL_AudioFormat format);
	int filter_index;
} SDL_AudioCVT;
typedef enum { SDL_AUDIO_STOPPED = 0, SDL_AUDIO_PLAYING, SDL_AUDIO_PAUSED } SDL_AudioStatus;
int  SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained);
void SDL_CloseAudio(void);
void SDL_PauseAudio(int pause_on);
void SDL_LockAudio(void);
void SDL_UnlockAudio(void);
SDL_AudioStatus SDL_GetAudioStatus(void);
SDL_AudioStatus SDL_GetAudioDeviceStatus(SDL_AudioDeviceID dev);
void SDL_AudioPump(void);
SDL_AudioSpec *SDL_LoadWAV_RW(SDL_RWops *src, int freesrc, SDL_AudioSpec *spec,
                              Uint8 **audio_buf, Uint32 *audio_len);
#define SDL_LoadWAV(file, spec, buf, len) \
	SDL_LoadWAV_RW(SDL_RWFromFile(file, "rb"), 1, spec, buf, len)
void SDL_FreeWAV(Uint8 *audio_buf);
int  SDL_BuildAudioCVT(SDL_AudioCVT *cvt, SDL_AudioFormat src_format,
                       Uint8 src_channels, int src_rate, SDL_AudioFormat dst_format,
                       Uint8 dst_channels, int dst_rate);
int  SDL_ConvertAudio(SDL_AudioCVT *cvt);
void SDL_MixAudio(Uint8 *dst, const Uint8 *src, Uint32 len, int volume);
void SDL_MixAudioFormat(Uint8 *dst, const Uint8 *src, SDL_AudioFormat format,
                        Uint32 len, int volume);

#ifdef __cplusplus
}
#endif

#endif /* OF_PC */
#endif /* OF_SDL2_SHIM_SDL_H */
