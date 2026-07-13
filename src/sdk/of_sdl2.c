//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_sdl2.c -- the single implementation TU for the openfpgaOS SDL2
 * compatibility layer (declared in <SDL2/SDL.h> and <SDL2/SDL_mixer.h>).
 *
 * Holds ALL SDL state (window surface, palette, event queue, audio
 * callback) so it is shared across every translation unit of a game --
 * unlike a header-only `static inline` shim, where each TU would get its
 * own private copy. sdk.mk auto-links this object into every app; apps
 * that call no SDL_* function have it removed entirely by --gc-sections.
 *
 * This file is device-only. On PC builds (-DOF_PC) it compiles to nothing
 * and the game links the real system SDL2 (see sdk.mk's app_pc rule and
 * the #include_next in <SDL2/SDL.h>).
 *
 * Implemented over the of_* HAL: of_video (8-bit indexed framebuffer +
 * palette + triple-buffer flip), of_input (gamepad), of_audio (PCM FIFO),
 * of_mixer / of_midi (SDL_mixer), of_time, and the C library (musl).
 * It deliberately does NOT use of_gpu -- the shim is CPU/surface based so
 * it is portable across targets; games add GPU acceleration themselves.
 */
#if !defined(OF_PC)

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>

#include "of.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <unistd.h>

/* This shim is intentionally dense: many SDL entry points are one-line
 * accessors with the body and a trailing `return` on the same line. That
 * trips -Wmisleading-indentation (a purely cosmetic check) under -Wall;
 * silence it here rather than line-break every trivial accessor. */
#pragma GCC diagnostic ignored "-Wmisleading-indentation"

/* ===================================================================== */
/* Opaque types completed here (forward-declared in the headers)          */
/* ===================================================================== */
struct SDL_BlitMap {
	int    has_colorkey;
	Uint32 colorkey;
	int    blendmode;
	Uint8  alphamod;
	Uint8  cmod_r, cmod_g, cmod_b;  /* surface color mod (rarely used) */
	int    owns_pixels;             /* free(pixels) on SDL_FreeSurface */
};
struct SDL_Window      { int w, h; Uint32 flags; };
struct SDL_Renderer    { int logical_w, logical_h; SDL_Texture *target;
                         Uint8 dr, dg, db, da; };
struct SDL_Texture     { SDL_Surface *surface; Uint32 format; int access;
                         int blend; Uint8 cmod_r, cmod_g, cmod_b, amod; };
struct SDL_Cursor      { int dummy; };
struct SDL_GameController { int idx; };
struct SDL_Joystick    { int idx; };
struct SDL_Thread      { int done; int result; };
struct SDL_mutex       { int locked; };
struct SDL_cond        { int dummy; };
struct SDL_sem         { Uint32 v; };

/* ===================================================================== */
/* Error / Log                                                            */
/* ===================================================================== */
static char g_error[512];

int SDL_SetError(const char *fmt, ...) {
	va_list ap; va_start(ap, fmt);
	vsnprintf(g_error, sizeof g_error, fmt, ap);
	va_end(ap);
	return -1;
}
const char *SDL_GetError(void) { return g_error; }
void SDL_ClearError(void) { g_error[0] = 0; }
int  SDL_Error(int code) { (void)code; return -1; }

static void of_vlog(const char *fmt, va_list ap) { vprintf(fmt, ap); printf("\n"); }
void SDL_LogMessageV(int cat, SDL_LogPriority pri, const char *fmt, va_list ap) { (void)cat; (void)pri; of_vlog(fmt, ap); }
void SDL_Log(const char *fmt, ...)            { va_list a; va_start(a,fmt); of_vlog(fmt,a); va_end(a); }
void SDL_LogVerbose(int c,const char*f,...)   { (void)c; va_list a; va_start(a,f); of_vlog(f,a); va_end(a); }
void SDL_LogDebug(int c,const char*f,...)     { (void)c; va_list a; va_start(a,f); of_vlog(f,a); va_end(a); }
void SDL_LogInfo(int c,const char*f,...)      { (void)c; va_list a; va_start(a,f); of_vlog(f,a); va_end(a); }
void SDL_LogWarn(int c,const char*f,...)      { (void)c; va_list a; va_start(a,f); of_vlog(f,a); va_end(a); }
void SDL_LogError(int c,const char*f,...)     { (void)c; va_list a; va_start(a,f); of_vlog(f,a); va_end(a); }
void SDL_LogCritical(int c,const char*f,...)  { (void)c; va_list a; va_start(a,f); of_vlog(f,a); va_end(a); }
void SDL_LogMessage(int c,SDL_LogPriority p,const char*f,...){ (void)c;(void)p; va_list a; va_start(a,f); of_vlog(f,a); va_end(a); }
void SDL_LogSetPriority(int c, SDL_LogPriority p) { (void)c; (void)p; }
SDL_LogPriority SDL_LogGetPriority(int c) { (void)c; return SDL_LOG_PRIORITY_INFO; }
void SDL_LogSetAllPriority(SDL_LogPriority p) { (void)p; }

/* ===================================================================== */
/* stdinc helpers                                                         */
/* ===================================================================== */
size_t SDL_strlcpy(char *dst, const char *src, size_t maxlen) {
	size_t srclen = strlen(src);
	if (maxlen > 0) { size_t n = srclen < maxlen-1 ? srclen : maxlen-1; memcpy(dst, src, n); dst[n] = 0; }
	return srclen;
}
size_t SDL_strlcat(char *dst, const char *src, size_t maxlen) {
	size_t dstlen = strnlen(dst, maxlen);
	if (dstlen == maxlen) return maxlen + strlen(src);
	return dstlen + SDL_strlcpy(dst + dstlen, src, maxlen - dstlen);
}

/* ===================================================================== */
/* Version / platform                                                     */
/* ===================================================================== */
void SDL_GetVersion(SDL_version *v) { if (v) { v->major = 2; v->minor = 0; v->patch = 16; } }
const char *SDL_GetRevision(void) { return "openfpgaOS-shim"; }
const char *SDL_GetPlatform(void) { return "openfpgaOS"; }
int SDL_GetNumTouchDevices(void) { return 0; }

/* ===================================================================== */
/* Init / Quit                                                            */
/* ===================================================================== */
static int g_video_inited;
int SDL_Init(Uint32 flags) { return SDL_InitSubSystem(flags); }
int SDL_InitSubSystem(Uint32 flags) {
	if ((flags & SDL_INIT_VIDEO) && !g_video_inited) {
		of_video_init();
		g_video_inited = 1;
	}
	if (flags & SDL_INIT_AUDIO) of_audio_init();
	return 0;
}
void SDL_QuitSubSystem(Uint32 flags) { (void)flags; }
Uint32 SDL_WasInit(Uint32 flags) { (void)flags; return g_video_inited ? SDL_INIT_VIDEO : 0; }
void SDL_Quit(void) {}

/* ===================================================================== */
/* Geometry                                                               */
/* ===================================================================== */
SDL_bool SDL_PointInRect(const SDL_Point *p, const SDL_Rect *r) {
	return (p->x >= r->x && p->x < r->x + r->w && p->y >= r->y && p->y < r->y + r->h) ? SDL_TRUE : SDL_FALSE;
}
SDL_bool SDL_HasIntersection(const SDL_Rect *a, const SDL_Rect *b) {
	if (!a || !b) return SDL_FALSE;
	if (a->x + a->w <= b->x || b->x + b->w <= a->x) return SDL_FALSE;
	if (a->y + a->h <= b->y || b->y + b->h <= a->y) return SDL_FALSE;
	return SDL_TRUE;
}
SDL_bool SDL_IntersectRect(const SDL_Rect *a, const SDL_Rect *b, SDL_Rect *out) {
	if (!SDL_HasIntersection(a, b)) { if (out) { out->x=out->y=out->w=out->h=0; } return SDL_FALSE; }
	int x0 = a->x > b->x ? a->x : b->x, y0 = a->y > b->y ? a->y : b->y;
	int x1 = (a->x+a->w < b->x+b->w) ? a->x+a->w : b->x+b->w;
	int y1 = (a->y+a->h < b->y+b->h) ? a->y+a->h : b->y+b->h;
	if (out) { out->x=x0; out->y=y0; out->w=x1-x0; out->h=y1-y0; }
	return SDL_TRUE;
}

/* ===================================================================== */
/* Pixel formats / palettes                                               */
/* ===================================================================== */
SDL_bool SDL_PixelFormatEnumToMasks(Uint32 format, int *bpp, Uint32 *r, Uint32 *g, Uint32 *b, Uint32 *a) {
	switch (format) {
	case SDL_PIXELFORMAT_INDEX8: *bpp=8; *r=*g=*b=*a=0; return SDL_TRUE;
	case SDL_PIXELFORMAT_RGB565: *bpp=16; *r=0xF800;*g=0x07E0;*b=0x001F;*a=0; return SDL_TRUE;
	case SDL_PIXELFORMAT_RGB888:
	case SDL_PIXELFORMAT_RGBX8888: *bpp=32; *r=0xFF0000;*g=0xFF00;*b=0xFF;*a=0; return SDL_TRUE;
	case SDL_PIXELFORMAT_ARGB8888: *bpp=32; *a=0xFF000000;*r=0xFF0000;*g=0xFF00;*b=0xFF; return SDL_TRUE;
	case SDL_PIXELFORMAT_RGBA8888: *bpp=32; *r=0xFF000000;*g=0xFF0000;*b=0xFF00;*a=0xFF; return SDL_TRUE;
	case SDL_PIXELFORMAT_ABGR8888: *bpp=32; *a=0xFF000000;*b=0xFF0000;*g=0xFF00;*r=0xFF; return SDL_TRUE;
	case SDL_PIXELFORMAT_BGRA8888: *bpp=32; *b=0xFF000000;*g=0xFF0000;*r=0xFF00;*a=0xFF; return SDL_TRUE;
	default: *bpp=32; *r=0xFF0000;*g=0xFF00;*b=0xFF;*a=0xFF000000; return SDL_TRUE;
	}
}
static void fill_format(SDL_PixelFormat *f, Uint32 fmt) {
	int bpp; Uint32 r,g,b,a; SDL_PixelFormatEnumToMasks(fmt,&bpp,&r,&g,&b,&a);
	memset(f, 0, sizeof *f);
	f->format = fmt; f->BitsPerPixel = (Uint8)bpp; f->BytesPerPixel = (Uint8)((bpp+7)/8);
	f->Rmask=r; f->Gmask=g; f->Bmask=b; f->Amask=a;
}
SDL_Palette *SDL_AllocPalette(int ncolors) {
	SDL_Palette *p = (SDL_Palette *)calloc(1, sizeof *p);
	if (!p) return NULL;
	p->ncolors = ncolors;
	p->colors = (SDL_Color *)calloc(ncolors > 0 ? ncolors : 1, sizeof(SDL_Color));
	p->refcount = 1;
	return p;
}
void SDL_FreePalette(SDL_Palette *p) {
	/* Refcount-correct: SDL2 palettes are shared (SDL_SetSurfacePalette
	 * bumps refcount). Only release at 0, else the first FreeSurface frees
	 * a palette another surface still points at -> double free. */
	if (!p) return;
	if (--p->refcount > 0) return;
	free(p->colors);
	free(p);
}

/* The window surface palette doubles as the hardware palette. */
static SDL_Palette *g_screen_palette;
static SDL_Surface *g_screen;
static int          g_screen_palette_is_render;
/* The palette the game actually renders with. For games that render into an
 * offscreen back buffer (its own palette) and blit pixels-only to the window
 * surface, the blit does not carry the palette; present pushes THIS one. */
static SDL_Palette *g_render_palette;

void of_sdl_set_screen_palette_is_render(int enabled) {
	g_screen_palette_is_render = enabled != 0;
	if (g_screen_palette_is_render && g_screen
	    && g_screen->format && g_screen->format->palette) {
		g_render_palette = g_screen->format->palette;
	}
}
static void push_palette_to_hw(const SDL_Palette *p, int first, int n) {
	for (int i = 0; i < n && (first+i) < 256; i++) {
		SDL_Color c = p->colors[first+i];
		of_video_palette((uint8_t)(first+i), ((uint32_t)c.r<<16)|((uint32_t)c.g<<8)|c.b);
	}
}
int SDL_SetPaletteColors(SDL_Palette *palette, const SDL_Color *colors, int first, int ncolors) {
	if (!palette) return -1;
	for (int i = 0; i < ncolors && (first+i) < palette->ncolors; i++)
		palette->colors[first+i] = colors[i];
	palette->version++;
	if (palette == g_screen_palette) push_palette_to_hw(palette, first, ncolors);
	return 0;
}
SDL_PixelFormat *SDL_AllocFormat(Uint32 fmt) {
	SDL_PixelFormat *f = (SDL_PixelFormat *)calloc(1, sizeof *f);
	if (!f) return NULL;
	fill_format(f, fmt);
	if (f->BitsPerPixel == 8) f->palette = SDL_AllocPalette(256);
	f->refcount = 1;
	return f;
}
void SDL_FreeFormat(SDL_PixelFormat *f) { if (f) { if (f->palette) SDL_FreePalette(f->palette); free(f); } }

Uint32 SDL_MapRGB(const SDL_PixelFormat *fmt, Uint8 r, Uint8 g, Uint8 b) {
	if (fmt && fmt->palette) {
		int best = 0, bestd = 1<<30;
		for (int i = 0; i < fmt->palette->ncolors; i++) {
			SDL_Color c = fmt->palette->colors[i];
			int d = (c.r-r)*(c.r-r)+(c.g-g)*(c.g-g)+(c.b-b)*(c.b-b);
			if (d < bestd) { bestd = d; best = i; if (!d) break; }
		}
		return (Uint32)best;
	}
	return ((Uint32)r<<16)|((Uint32)g<<8)|b;
}
Uint32 SDL_MapRGBA(const SDL_PixelFormat *fmt, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
	if (fmt && fmt->palette) return SDL_MapRGB(fmt, r, g, b);
	return ((Uint32)a<<24)|((Uint32)r<<16)|((Uint32)g<<8)|b;
}
void SDL_GetRGB(Uint32 pixel, const SDL_PixelFormat *fmt, Uint8 *r, Uint8 *g, Uint8 *b) {
	if (fmt && fmt->palette && (int)pixel < fmt->palette->ncolors) {
		SDL_Color c = fmt->palette->colors[pixel]; *r=c.r; *g=c.g; *b=c.b; return;
	}
	*r=(pixel>>16)&0xFF; *g=(pixel>>8)&0xFF; *b=pixel&0xFF;
}
void SDL_GetRGBA(Uint32 pixel, const SDL_PixelFormat *fmt, Uint8 *r, Uint8 *g, Uint8 *b, Uint8 *a) {
	SDL_GetRGB(pixel, fmt, r, g, b);
	*a = (fmt && fmt->palette) ? 255 : ((pixel>>24)&0xFF);
}

/* ===================================================================== */
/* Surfaces                                                               */
/* ===================================================================== */
static SDL_Surface *new_surface(int w, int h, Uint32 fmt, void *pixels, int pitch) {
	SDL_Surface *s = (SDL_Surface *)calloc(1, sizeof *s);
	if (!s) return NULL;
	s->format = SDL_AllocFormat(fmt);
	s->w = w; s->h = h;
	s->pitch = pitch ? pitch : w * s->format->BytesPerPixel;
	struct SDL_BlitMap *m = (struct SDL_BlitMap *)calloc(1, sizeof *m);
	s->map = m;
	if (pixels) { s->pixels = pixels; m->owns_pixels = 0; }
	else { s->pixels = calloc(1, (size_t)s->pitch * (h > 0 ? h : 1)); m->owns_pixels = 1; }
	s->clip_rect.x = 0; s->clip_rect.y = 0; s->clip_rect.w = w; s->clip_rect.h = h;
	s->refcount = 1;
	return s;
}
SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int w, int h, int depth,
    Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask) {
	(void)flags; (void)Rmask; (void)Gmask; (void)Bmask;
	Uint32 fmt = (depth == 8) ? SDL_PIXELFORMAT_INDEX8
	           : (Amask ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_RGB888);
	return new_surface(w, h, fmt, NULL, 0);
}
SDL_Surface *SDL_CreateRGBSurfaceFrom(void *pixels, int w, int h, int depth, int pitch,
    Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask) {
	(void)Rmask;(void)Gmask;(void)Bmask;
	Uint32 fmt = depth == 8 ? SDL_PIXELFORMAT_INDEX8 : (Amask ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_RGB888);
	return new_surface(w, h, fmt, pixels, pitch);
}
SDL_Surface *SDL_CreateRGBSurfaceWithFormat(Uint32 flags, int w, int h, int depth, Uint32 format) {
	(void)flags; (void)depth; return new_surface(w, h, format, NULL, 0);
}
SDL_Surface *SDL_CreateRGBSurfaceWithFormatFrom(void *pixels, int w, int h, int depth, int pitch, Uint32 format) {
	(void)depth; return new_surface(w, h, format, pixels, pitch);
}
void SDL_FreeSurface(SDL_Surface *s) {
	if (!s) return;
	if (--s->refcount > 0) return;
	if (s->map && s->map->owns_pixels) free(s->pixels);
	free(s->map);
	if (s->format) SDL_FreeFormat(s->format);
	free(s);
}
int  SDL_LockSurface(SDL_Surface *s) { if (s) s->locked++; return 0; }
void SDL_UnlockSurface(SDL_Surface *s) { if (s && s->locked) s->locked--; }
int  SDL_SetSurfacePalette(SDL_Surface *s, SDL_Palette *p) {
	if (!s || !s->format) return -1;
	if (s->format->palette && s->format->palette != p) SDL_FreePalette(s->format->palette);
	s->format->palette = p; if (p) p->refcount++;
	if (s == g_screen && p && g_screen_palette_is_render) g_render_palette = p;
	return 0;
}
int SDL_SetColorKey(SDL_Surface *s, int flag, Uint32 key) {
	if (!s || !s->map) return -1;
	s->map->has_colorkey = flag ? 1 : 0; s->map->colorkey = key;
	return 0;
}
int SDL_GetColorKey(SDL_Surface *s, Uint32 *key) {
	if (!s || !s->map || !s->map->has_colorkey) return -1;
	if (key) *key = s->map->colorkey; return 0;
}
int SDL_SetSurfaceBlendMode(SDL_Surface *s, int m) { if (s&&s->map) s->map->blendmode=m; return 0; }
int SDL_SetSurfaceAlphaMod(SDL_Surface *s, Uint8 a) { if (s&&s->map) s->map->alphamod=a; return 0; }
int SDL_SetClipRect(SDL_Surface *s, const SDL_Rect *r) {
	if (!s) return -1;
	if (r) { SDL_Rect full; full.x=0; full.y=0; full.w=s->w; full.h=s->h; SDL_IntersectRect(r,&full,&s->clip_rect); }
	else { s->clip_rect.x=0; s->clip_rect.y=0; s->clip_rect.w=s->w; s->clip_rect.h=s->h; }
	return 0;
}
void SDL_GetClipRect(SDL_Surface *s, SDL_Rect *r) { if (s&&r) *r = s->clip_rect; }

int SDL_FillRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color) {
	if (!dst) return -1;
	SDL_Rect r, full, clip;
	if (rect) r = *rect; else { r.x=0; r.y=0; r.w=dst->w; r.h=dst->h; }
	full.x=0; full.y=0; full.w=dst->w; full.h=dst->h;
	if (!SDL_IntersectRect(&r,&full,&clip)) return 0;
	int bpp = dst->format->BytesPerPixel;
	for (int y = clip.y; y < clip.y+clip.h; y++) {
		Uint8 *row = (Uint8*)dst->pixels + (size_t)y*dst->pitch + (size_t)clip.x*bpp;
		if (bpp == 1) memset(row, (int)(color & 0xFF), clip.w);
		else { Uint32 *p=(Uint32*)row; for (int x=0;x<clip.w;x++) p[x]=color; }
	}
	return 0;
}
int SDL_FillRects(SDL_Surface *dst, const SDL_Rect *rects, int count, Uint32 color) {
	for (int i=0;i<count;i++) SDL_FillRect(dst,&rects[i],color); return 0;
}

/* Generic same-bpp blit with colorkey + clipping; mixed-format via map/get. */
int SDL_UpperBlit(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect) {
	if (!src || !dst) return -1;
	/* Track the palette of the pixels that will end up on screen: the last
	 * blit into the window surface before present supplies it. */
	if (dst == g_screen && g_screen_palette_is_render
	    && dst->format && dst->format->palette) {
		g_render_palette = dst->format->palette;
	} else if (src->format && src->format->palette) {
		g_render_palette = src->format->palette;
	}
	SDL_Rect sr;
	if (srcrect) sr = *srcrect; else { sr.x=0; sr.y=0; sr.w=src->w; sr.h=src->h; }
	int dx = dstrect ? dstrect->x : 0, dy = dstrect ? dstrect->y : 0;
	if (sr.x < 0) { dx -= sr.x; sr.w += sr.x; sr.x = 0; }
	if (sr.y < 0) { dy -= sr.y; sr.h += sr.y; sr.y = 0; }
	if (sr.x + sr.w > src->w) sr.w = src->w - sr.x;
	if (sr.y + sr.h > src->h) sr.h = src->h - sr.y;
	SDL_Rect cl = dst->clip_rect;
	if (dx < cl.x) { int d = cl.x - dx; sr.x += d; sr.w -= d; dx = cl.x; }
	if (dy < cl.y) { int d = cl.y - dy; sr.y += d; sr.h -= d; dy = cl.y; }
	if (dx + sr.w > cl.x + cl.w) sr.w = cl.x + cl.w - dx;
	if (dy + sr.h > cl.y + cl.h) sr.h = cl.y + cl.h - dy;
	if (sr.w <= 0 || sr.h <= 0) { if (dstrect){dstrect->w=0;dstrect->h=0;} return 0; }

	int sbpp = src->format->BytesPerPixel, dbpp = dst->format->BytesPerPixel;
	int ck = src->map && src->map->has_colorkey;
	Uint32 key = ck ? src->map->colorkey : 0;
	for (int y = 0; y < sr.h; y++) {
		Uint8 *sp = (Uint8*)src->pixels + (size_t)(sr.y+y)*src->pitch + (size_t)sr.x*sbpp;
		Uint8 *dp = (Uint8*)dst->pixels + (size_t)(dy+y)*dst->pitch + (size_t)dx*dbpp;
		if (sbpp == dbpp && !ck) { memcpy(dp, sp, (size_t)sr.w*sbpp); continue; }
		if (sbpp == 1 && dbpp == 1) {
			for (int x=0;x<sr.w;x++){ Uint8 v=sp[x]; if(ck && v==(Uint8)key) continue; dp[x]=v; }
		} else if (sbpp == 4 && dbpp == 4) {
			Uint32 *s32=(Uint32*)sp,*d32=(Uint32*)dp;
			for (int x=0;x<sr.w;x++){ Uint32 v=s32[x]; if(ck && v==key) continue; d32[x]=v; }
		} else {
			for (int x=0;x<sr.w;x++){
				Uint32 v = sbpp==1 ? sp[x] : ((Uint32*)sp)[x];
				if (ck && v==key) continue;
				Uint8 r,g,b,a; SDL_GetRGBA(v, src->format,&r,&g,&b,&a);
				Uint32 o = SDL_MapRGBA(dst->format,r,g,b,a);
				if (dbpp==1) dp[x]=(Uint8)o; else ((Uint32*)dp)[x]=o;
			}
		}
	}
	if (dstrect) { dstrect->w = sr.w; dstrect->h = sr.h; }
	return 0;
}
int SDL_UpperBlitScaled(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect) {
	if (!src || !dst) return -1;
	if (dst == g_screen && g_screen_palette_is_render
	    && dst->format && dst->format->palette) {
		g_render_palette = dst->format->palette;
	} else if (src->format && src->format->palette) {
		g_render_palette = src->format->palette;
	}
	SDL_Rect sr, dr;
	if (srcrect) sr = *srcrect; else { sr.x=0; sr.y=0; sr.w=src->w; sr.h=src->h; }
	if (dstrect) dr = *dstrect; else { dr.x=0; dr.y=0; dr.w=dst->w; dr.h=dst->h; }
	if (sr.w<=0||sr.h<=0||dr.w<=0||dr.h<=0) return 0;
	int sbpp=src->format->BytesPerPixel, dbpp=dst->format->BytesPerPixel;
	int ck = src->map && src->map->has_colorkey; Uint32 key = ck?src->map->colorkey:0;
	for (int y=0;y<dr.h;y++){
		int dyy=dr.y+y; if(dyy<dst->clip_rect.y||dyy>=dst->clip_rect.y+dst->clip_rect.h) continue;
		int syy=sr.y + (int)((long)y*sr.h/dr.h);
		Uint8 *srow=(Uint8*)src->pixels+(size_t)syy*src->pitch;
		Uint8 *drow=(Uint8*)dst->pixels+(size_t)dyy*dst->pitch;
		for (int x=0;x<dr.w;x++){
			int dxx=dr.x+x; if(dxx<dst->clip_rect.x||dxx>=dst->clip_rect.x+dst->clip_rect.w) continue;
			int sxx=sr.x + (int)((long)x*sr.w/dr.w);
			if (sbpp==1&&dbpp==1){ Uint8 v=srow[sxx]; if(ck&&v==(Uint8)key) continue; drow[dxx]=v; }
			else if (sbpp==4&&dbpp==4){ Uint32 v=((Uint32*)srow)[sxx]; if(ck&&v==key) continue; ((Uint32*)drow)[dxx]=v; }
			else {
				Uint32 v = sbpp==1 ? srow[sxx] : ((Uint32*)srow)[sxx];
				if (ck && v==key) continue;
				Uint8 r,g,b,a; SDL_GetRGBA(v, src->format,&r,&g,&b,&a);
				Uint32 o = SDL_MapRGBA(dst->format,r,g,b,a);
				if (dbpp==1) drow[dxx]=(Uint8)o; else ((Uint32*)drow)[dxx]=o;
			}
		}
	}
	return 0;
}
int SDL_SoftStretch(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, const SDL_Rect *dstrect) {
	SDL_Rect dr;
	if (dstrect) dr = *dstrect; else { dr.x=0; dr.y=0; dr.w=dst->w; dr.h=dst->h; }
	return SDL_UpperBlitScaled(src, srcrect, dst, &dr);
}
SDL_Surface *SDL_ConvertSurface(SDL_Surface *src, const SDL_PixelFormat *fmt, Uint32 flags) {
	(void)flags;
	SDL_Surface *d = new_surface(src->w, src->h, fmt->format, NULL, 0);
	if (!d) return NULL;
	if (fmt->palette && d->format->palette)
		SDL_SetPaletteColors(d->format->palette, fmt->palette->colors, 0, fmt->palette->ncolors);
	SDL_Rect r; r.x=0; r.y=0; r.w=src->w; r.h=src->h;
	SDL_UpperBlit(src, &r, d, &r);
	return d;
}
SDL_Surface *SDL_ConvertSurfaceFormat(SDL_Surface *src, Uint32 pixel_format, Uint32 flags) {
	(void)flags;
	SDL_PixelFormat tmp; fill_format(&tmp, pixel_format); tmp.palette = NULL;
	return SDL_ConvertSurface(src, &tmp, 0);
}

/* SDL 1.2 palette compat */
int SDL_SetColors(SDL_Surface *s, const SDL_Color *colors, int first, int n) {
	if (!s || !s->format || !s->format->palette) return 0;
	SDL_SetPaletteColors(s->format->palette, colors, first, n);
	return 1;
}
int SDL_SetPalette(SDL_Surface *s, int flags, const SDL_Color *colors, int first, int n) {
	(void)flags; return SDL_SetColors(s, colors, first, n);
}

/* ===================================================================== */
/* RWops (FILE*-backed + memory)                                          */
/* ===================================================================== */
static Sint64 rw_stdio_size(SDL_RWops *c){ FILE*f=(FILE*)c->hidden.stdio.fp; long cur=ftell(f); fseek(f,0,SEEK_END); long e=ftell(f); fseek(f,cur,SEEK_SET); return e; }
static Sint64 rw_stdio_seek(SDL_RWops *c, Sint64 off, int w){ FILE*f=(FILE*)c->hidden.stdio.fp; fseek(f,(long)off,w); return ftell(f); }
static size_t rw_stdio_read(SDL_RWops *c, void*p, size_t sz, size_t n){ return fread(p,sz,n,(FILE*)c->hidden.stdio.fp); }
static size_t rw_stdio_write(SDL_RWops *c, const void*p, size_t sz, size_t n){ return fwrite(p,sz,n,(FILE*)c->hidden.stdio.fp); }
static int    rw_stdio_close(SDL_RWops *c){ if(c){ if(c->hidden.stdio.fp) fclose((FILE*)c->hidden.stdio.fp); free(c);} return 0; }

static Sint64 rw_mem_size(SDL_RWops *c){ return (Sint64)(c->hidden.mem.stop - c->hidden.mem.base); }
static Sint64 rw_mem_seek(SDL_RWops *c, Sint64 off, int w){
	Uint8 *np; if(w==RW_SEEK_SET)np=c->hidden.mem.base+off; else if(w==RW_SEEK_CUR)np=c->hidden.mem.here+off; else np=c->hidden.mem.stop+off;
	if(np<c->hidden.mem.base)np=c->hidden.mem.base; if(np>c->hidden.mem.stop)np=c->hidden.mem.stop;
	c->hidden.mem.here=np; return np-c->hidden.mem.base;
}
static size_t rw_mem_read(SDL_RWops *c, void*p, size_t sz, size_t n){
	if(sz==0||n==0) return 0; size_t avail=(size_t)(c->hidden.mem.stop-c->hidden.mem.here)/sz; if(n>avail)n=avail;
	memcpy(p,c->hidden.mem.here,n*sz); c->hidden.mem.here+=n*sz; return n;
}
static size_t rw_mem_write(SDL_RWops *c, const void*p, size_t sz, size_t n){
	size_t avail=(size_t)(c->hidden.mem.stop-c->hidden.mem.here)/sz; if(n>avail)n=avail;
	memcpy(c->hidden.mem.here,p,n*sz); c->hidden.mem.here+=n*sz; return n;
}
static int rw_mem_close(SDL_RWops *c){ free(c); return 0; }

SDL_RWops *SDL_AllocRW(void){ return (SDL_RWops*)calloc(1,sizeof(SDL_RWops)); }
void SDL_FreeRW(SDL_RWops *a){ free(a); }
SDL_RWops *SDL_RWFromFile(const char *file, const char *mode) {
	FILE *f = fopen(file, mode); if (!f) { SDL_SetError("open %s failed", file); return NULL; }
	SDL_RWops *c = SDL_AllocRW(); if(!c){ fclose(f); return NULL; }
	c->type=SDL_RWOPS_STDIO; c->hidden.stdio.fp=f;
	c->size=rw_stdio_size; c->seek=rw_stdio_seek; c->read=rw_stdio_read; c->write=rw_stdio_write; c->close=rw_stdio_close;
	return c;
}
SDL_RWops *SDL_RWFromMem(void *mem, int size) {
	SDL_RWops *c=SDL_AllocRW(); if(!c) return NULL; c->type=SDL_RWOPS_MEMORY;
	c->hidden.mem.base=(Uint8*)mem; c->hidden.mem.here=(Uint8*)mem; c->hidden.mem.stop=(Uint8*)mem+size;
	c->size=rw_mem_size; c->seek=rw_mem_seek; c->read=rw_mem_read; c->write=rw_mem_write; c->close=rw_mem_close;
	return c;
}
SDL_RWops *SDL_RWFromConstMem(const void *mem, int size) { return SDL_RWFromMem((void*)mem, size); }
Uint8  SDL_ReadU8(SDL_RWops *s){ Uint8 v=0; s->read(s,&v,1,1); return v; }
Uint16 SDL_ReadLE16(SDL_RWops *s){ Uint8 b[2]={0}; s->read(s,b,2,1); return (Uint16)(b[0]|(b[1]<<8)); }
Uint32 SDL_ReadLE32(SDL_RWops *s){ Uint8 b[4]={0}; s->read(s,b,4,1); return (Uint32)(b[0]|(b[1]<<8)|(b[2]<<16)|((Uint32)b[3]<<24)); }
void *SDL_LoadFile_RW(SDL_RWops *src, size_t *datasize, int freesrc) {
	if (!src) return NULL;
	Sint64 sz = src->size(src); if (sz < 0) sz = 0;
	void *buf = malloc((size_t)sz + 1);
	if (buf) { src->seek(src,0,RW_SEEK_SET); src->read(src, buf, 1, (size_t)sz); ((char*)buf)[sz]=0; if (datasize) *datasize=(size_t)sz; }
	if (freesrc) src->close(src);
	return buf;
}

/* ===================================================================== */
/* Window + framebuffer present                                           */
/* ===================================================================== */
static struct SDL_Window g_window;
/* When true, g_screen->pixels IS the of_video back buffer (one of three
 * triple-buffered pages owned by the OS), so the game draws straight into
 * the buffer of_video_flip() presents -- no per-frame copy. Requires the
 * surface dims to match the live video mode. After each flip we re-bind
 * g_screen->pixels to the new draw buffer (flip rotates the pages). */
static int g_screen_aliases_fb;

static void fb_dims(int *w, int *h) {
	of_video_mode_t m; of_video_get_mode(&m);
	*w = m.width  ? (int)m.width  : OF_SCREEN_W;
	*h = m.height ? (int)m.height : OF_SCREEN_H;
}
/* Try to put the OS into an 8-bit mode matching the requested size, so the
 * window surface can alias the framebuffer 1:1. Returns 1 if the active
 * mode now matches (w,h) in 8-bit. */
static int try_match_mode(int w, int h) {
	of_video_mode_t m; of_video_get_mode(&m);
	if (m.width == (uint16_t)w && m.height == (uint16_t)h && m.color_mode == OF_VIDEO_MODE_8BIT)
		return 1;
	if (w <= 0 || h <= 0 || w > OF_VIDEO_MAX_WIDTH || h > OF_VIDEO_MAX_HEIGHT)
		return 0;
	of_video_mode_t want; want.width=(uint16_t)w; want.height=(uint16_t)h; want.stride=0;
	want.color_mode=OF_VIDEO_MODE_8BIT; want.reserved=0;
	if (of_video_check_mode(&want, NULL) < 0) return 0;
	if (of_video_set_mode(&want) != 0) return 0;
	of_video_get_mode(&m);
	return (m.width == (uint16_t)w && m.height == (uint16_t)h);
}
static SDL_Surface *make_screen(int w, int h) {
	try_match_mode(w, h);
	int fw, fh; fb_dims(&fw, &fh);
	of_video_mode_t m; of_video_get_mode(&m);
	int fstride = m.stride ? (int)m.stride : fw;
	SDL_Surface *s;
	if (w == fw && h == fh) {
		/* Alias: hand new_surface the live draw buffer; owns_pixels=0 so
		 * SDL_FreeSurface never frees OS-owned memory. */
		s = new_surface(w, h, SDL_PIXELFORMAT_INDEX8, of_video_surface(), fstride);
		g_screen_aliases_fb = 1;
	} else {
		s = new_surface(w, h, SDL_PIXELFORMAT_INDEX8, NULL, 0);
		g_screen_aliases_fb = 0;
	}
	g_screen_palette = s->format->palette;
	return s;
}
SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags) {
	(void)title;(void)x;(void)y;
	if (!g_video_inited) { of_video_init(); g_video_inited = 1; }
	if (w <= 0) w = OF_SCREEN_W; if (h <= 0) h = OF_SCREEN_H;
	g_window.w = w; g_window.h = h; g_window.flags = flags | SDL_WINDOW_SHOWN;
	if (!g_screen) g_screen = make_screen(w, h);
	return &g_window;
}
void SDL_DestroyWindow(SDL_Window *win) { (void)win; }
SDL_Surface *SDL_GetWindowSurface(SDL_Window *win) {
	(void)win;
	if (!g_screen) g_screen = make_screen(g_window.w?g_window.w:OF_SCREEN_W, g_window.h?g_window.h:OF_SCREEN_H);
	return g_screen;
}
SDL_Surface *SDL_GetVideoSurface(void) { return SDL_GetWindowSurface(&g_window); }

static void audio_pump(void);  /* fwd */

static void present_screen(void) {
	if (!g_screen) return;
	audio_pump(); /* feed audio before the copy/flip can stall */
	of_video_mode_t m; of_video_get_mode(&m);
	int fw = m.width  ? (int)m.width  : OF_SCREEN_W;
	int fh = m.height ? (int)m.height : OF_SCREEN_H;
	int fstride = m.stride ? (int)m.stride : fw;
	const uint8_t *src = (const uint8_t *)g_screen->pixels;
	int sw = g_screen->w, sh = g_screen->h, sp = g_screen->pitch;

	if (!g_screen_aliases_fb) {
		uint8_t *fb = of_video_surface();
		if (sw == fw && sh == fh) {
			for (int y = 0; y < fh; y++) memcpy(fb + (size_t)y*fstride, src + (size_t)y*sp, fw);
		} else {
			for (int y = 0; y < fh; y++) {
				int sy = (int)((long)y * sh / fh);
				const uint8_t *srow = src + (size_t)sy*sp;
				uint8_t *drow = fb + (size_t)y*fstride;
				for (int x = 0; x < fw; x++) drow[x] = srow[(int)((long)x * sw / fw)];
			}
		}
	}
	if (g_screen_palette_is_render && g_screen->format && g_screen->format->palette)
		g_render_palette = g_screen->format->palette;
	/* Push the render palette to hardware only when it changed. The blit
	 * that copies indexed pixels into the window surface does not carry the
	 * palette, so without this the colors would be wrong. */
	if (g_render_palette) {
		static unsigned last_pal_ver = ~0u;
		static SDL_Palette *last_pal_ptr;
		if (g_render_palette->version != last_pal_ver || g_render_palette != last_pal_ptr) {
			last_pal_ver = g_render_palette->version;
			last_pal_ptr = g_render_palette;
			uint32_t pal[256];
			int n = g_render_palette->ncolors > 256 ? 256 : g_render_palette->ncolors;
			for (int i = 0; i < n; i++) {
				SDL_Color c = g_render_palette->colors[i];
				pal[i] = ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
			}
			for (int i = n; i < 256; i++) pal[i] = 0;
			of_video_palette_bulk(pal, 256);
		}
	}
	of_video_flip();
	if (g_screen_aliases_fb) g_screen->pixels = of_video_surface();
	audio_pump();
}
int SDL_UpdateWindowSurface(SDL_Window *win) { (void)win; present_screen(); return 0; }
int SDL_UpdateWindowSurfaceRects(SDL_Window *win, const SDL_Rect *r, int n) { (void)win;(void)r;(void)n; present_screen(); return 0; }
void SDL_SetWindowTitle(SDL_Window *win, const char *t) { (void)win;(void)t; }
void SDL_GetWindowSize(SDL_Window *win, int *w, int *h) { if(w)*w=win?win->w:g_window.w; if(h)*h=win?win->h:g_window.h; }
void SDL_SetWindowSize(SDL_Window *win, int w, int h) { if(win){win->w=w;win->h=h;} }
Uint32 SDL_GetWindowFlags(SDL_Window *win) { return win ? win->flags : (SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN); }
int SDL_SetWindowFullscreen(SDL_Window *win, Uint32 flags) { (void)win;(void)flags; return 0; }
void SDL_SetWindowResizable(SDL_Window *win, SDL_bool r) { (void)win;(void)r; }
Uint32 SDL_GetWindowID(SDL_Window *win) { (void)win; return 1; }
SDL_Window *SDL_GetWindowFromID(Uint32 id) { (void)id; return &g_window; }
void SDL_GetWindowPosition(SDL_Window *win, int *x, int *y) { (void)win; if(x)*x=0; if(y)*y=0; }
void SDL_SetWindowPosition(SDL_Window *win, int x, int y) { (void)win;(void)x;(void)y; }
void SDL_ShowWindow(SDL_Window *w){(void)w;} void SDL_HideWindow(SDL_Window *w){(void)w;}
void SDL_RaiseWindow(SDL_Window *w){(void)w;} void SDL_RestoreWindow(SDL_Window *w){(void)w;}
void SDL_MaximizeWindow(SDL_Window *w){(void)w;} void SDL_MinimizeWindow(SDL_Window *w){(void)w;}
void SDL_SetWindowGrab(SDL_Window *w, SDL_bool g){(void)w;(void)g;}
int SDL_GetWindowDisplayIndex(SDL_Window *w){(void)w; return 0;}
SDL_Window *SDL_GetKeyboardFocus(void){ return &g_window; }
void SDL_DisableScreenSaver(void){} void SDL_EnableScreenSaver(void){}
Uint32 SDL_GetWindowPixelFormat(SDL_Window *window){ (void)window; return SDL_PIXELFORMAT_INDEX8; }
void SDL_SetWindowIcon(SDL_Window *w, SDL_Surface *s){ (void)w;(void)s; }

static void mode_from_fb(SDL_DisplayMode *m){ int w,h; fb_dims(&w,&h); m->format=SDL_PIXELFORMAT_INDEX8; m->w=w; m->h=h; m->refresh_rate=60; m->driverdata=NULL; }
int SDL_GetCurrentDisplayMode(int d, SDL_DisplayMode *m){ (void)d; if(m) mode_from_fb(m); return 0; }
int SDL_GetDesktopDisplayMode(int d, SDL_DisplayMode *m){ (void)d; if(m) mode_from_fb(m); return 0; }
int SDL_GetDisplayMode(int d, int i, SDL_DisplayMode *m){
	(void)d;
	if (!m) return -1;
	of_video_mode_t om;
	if (of_video_get_mode_info(i, &om) < 0) return -1;
	m->format = SDL_PIXELFORMAT_INDEX8; m->w = om.width; m->h = om.height; m->refresh_rate = 60; m->driverdata = NULL;
	return 0;
}
int SDL_GetWindowDisplayMode(SDL_Window *win, SDL_DisplayMode *m){ (void)win; if(m) mode_from_fb(m); return 0; }
int SDL_SetWindowDisplayMode(SDL_Window *win, const SDL_DisplayMode *m){
	if (!win || !m) return -1;
	win->w = m->w > 0 ? m->w : OF_SCREEN_W; win->h = m->h > 0 ? m->h : OF_SCREEN_H;
	return 0;
}
int SDL_GetDisplayBounds(int d, SDL_Rect *r){ (void)d; if(r){int w,h;fb_dims(&w,&h);r->x=0;r->y=0;r->w=w;r->h=h;} return 0; }
int SDL_GetNumVideoDisplays(void){ return 1; }
int SDL_GetNumDisplayModes(int d){ (void)d; int n = of_video_get_mode_count(); return n > 0 ? n : 1; }
int SDL_GetDisplayDPI(int d, float *ddpi, float *hdpi, float *vdpi){ (void)d; if(ddpi)*ddpi=96; if(hdpi)*hdpi=96; if(vdpi)*vdpi=96; return 0; }
const char *SDL_GetCurrentVideoDriver(void){ return "openfpga"; }

/* SDL 1.2 video compat */
SDL_Surface *SDL_SetVideoMode(int width, int height, int bpp, Uint32 flags) {
	(void)bpp; (void)flags;
	if (!g_video_inited) { of_video_init(); g_video_inited = 1; }
	if (width <= 0) width = OF_SCREEN_W; if (height <= 0) height = OF_SCREEN_H;
	g_window.w = width; g_window.h = height; g_window.flags = SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN;
	if (g_screen) { SDL_FreeSurface(g_screen); g_screen = NULL; }
	g_screen = make_screen(width, height);
	of_video_clear(0);
	return g_screen;
}
void SDL_Flip(SDL_Surface *screen) { (void)screen; present_screen(); }
int SDL_VideoModeOK(int width, int height, int bpp, Uint32 flags) {
	(void)flags;
	if (bpp != 0 && bpp != 8) return 0;
	if (width <= 0) width = OF_SCREEN_W; if (height <= 0) height = OF_SCREEN_H;
	if (width > OF_VIDEO_MAX_WIDTH || height > OF_VIDEO_MAX_HEIGHT) return 0;
	of_video_mode_t want; want.width=(uint16_t)width; want.height=(uint16_t)height; want.stride=0;
	want.color_mode=OF_VIDEO_MODE_8BIT; want.reserved=0;
	return of_video_check_mode(&want, NULL) == 0 ? 8 : 0;
}
SDL_Rect **SDL_ListModes(SDL_PixelFormat *format, Uint32 flags) {
	(void)flags;
	if (format && format->BitsPerPixel != 0 && format->BitsPerPixel != 8) return NULL;
	return (SDL_Rect **)-1;  /* any size OK */
}
void SDL_WM_SetCaption(const char *title, const char *icon) { (void)title; (void)icon; }
int  SDL_WM_ToggleFullScreen(SDL_Surface *surface) { (void)surface; return 1; }
void SDL_WM_SetIcon(SDL_Surface *icon, Uint8 *mask) { (void)icon; (void)mask; }

/* ===================================================================== */
/* Renderer / Texture (software, backed by surfaces)                      */
/* ===================================================================== */
static struct SDL_Renderer g_renderer;
static SDL_Texture *g_render_target;   /* NULL = window surface */

static SDL_Surface *render_dst(void) { return g_render_target ? g_render_target->surface : g_screen; }

SDL_Renderer *SDL_CreateRenderer(SDL_Window *win, int idx, Uint32 flags){
	(void)win;(void)idx;(void)flags;
	g_renderer.logical_w=0; g_renderer.logical_h=0; g_renderer.target=NULL;
	g_renderer.dr=g_renderer.dg=g_renderer.db=g_renderer.da=0;
	if (!g_screen) g_screen = make_screen(g_window.w?g_window.w:OF_SCREEN_W, g_window.h?g_window.h:OF_SCREEN_H);
	return &g_renderer;
}
void SDL_DestroyRenderer(SDL_Renderer *r){ (void)r; }
int SDL_GetRendererInfo(SDL_Renderer *r, SDL_RendererInfo *info){ (void)r; if(info){ memset(info,0,sizeof*info); info->name="openfpga"; info->flags=SDL_RENDERER_SOFTWARE; } return 0; }
int SDL_RenderSetLogicalSize(SDL_Renderer *r, int w, int h){ if(r){r->logical_w=w;r->logical_h=h;} return 0; }
void SDL_RenderGetLogicalSize(SDL_Renderer *r, int *w, int *h){ if(w)*w=r?r->logical_w:0; if(h)*h=r?r->logical_h:0; }
int SDL_RenderSetIntegerScale(SDL_Renderer *r, SDL_bool e){ (void)r;(void)e; return 0; }
void SDL_RenderGetScale(SDL_Renderer *r, float *sx, float *sy){ (void)r; if(sx)*sx=1.0f; if(sy)*sy=1.0f; }
void SDL_RenderGetViewport(SDL_Renderer *r, SDL_Rect *rect){ (void)r; if(rect){int w,h;fb_dims(&w,&h);rect->x=0;rect->y=0;rect->w=w;rect->h=h;} }
int SDL_RenderSetViewport(SDL_Renderer *r, const SDL_Rect *rect){ (void)r;(void)rect; return 0; }
int SDL_RenderSetClipRect(SDL_Renderer *r, const SDL_Rect *rect){ (void)r; SDL_Surface *d=render_dst(); if(d) SDL_SetClipRect(d, rect); return 0; }
int SDL_SetRenderDrawColor(SDL_Renderer *r, Uint8 a,Uint8 b,Uint8 c,Uint8 d){ if(r){r->dr=a;r->dg=b;r->db=c;r->da=d;} return 0; }
static Uint32 draw_color(SDL_Renderer *r, SDL_Surface *d) {
	if (!d || !d->format) return 0;
	if (d->format->palette) return SDL_MapRGB(d->format, r?r->dr:0, r?r->dg:0, r?r->db:0);
	return ((Uint32)(r?r->da:255)<<24)|((Uint32)(r?r->dr:0)<<16)|((Uint32)(r?r->dg:0)<<8)|(r?r->db:0);
}
int SDL_RenderClear(SDL_Renderer *r){ SDL_Surface *d=render_dst(); if(d) SDL_FillRect(d,NULL,draw_color(r,d)); return 0; }
int SDL_RenderCopy(SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *src, const SDL_Rect *dst){
	(void)r; SDL_Surface *d=render_dst(); if(!t||!t->surface||!d) return 0;
	SDL_Rect dr;
	if (dst) dr=*dst; else { dr.x=0; dr.y=0; dr.w=d->w; dr.h=d->h; }
	return SDL_UpperBlitScaled(t->surface, src, d, &dr);
}
int SDL_RenderCopyEx(SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *src, const SDL_Rect *dst,
                     double angle, const SDL_Point *center, SDL_RendererFlip flip){
	(void)angle; (void)center; (void)flip;  /* rotation/flip not supported in SW path */
	return SDL_RenderCopy(r, t, src, dst);
}
void SDL_RenderPresent(SDL_Renderer *r){ (void)r; present_screen(); }
int SDL_RenderReadPixels(SDL_Renderer *r, const SDL_Rect *rect, Uint32 fmt, void *px, int pitch){ (void)r;(void)rect;(void)fmt;(void)px;(void)pitch; return 0; }
int SDL_RenderDrawPoint(SDL_Renderer *r, int x, int y){
	SDL_Surface *d=render_dst(); if(!d) return 0;
	SDL_Rect rr; rr.x=x; rr.y=y; rr.w=1; rr.h=1;
	return SDL_FillRect(d, &rr, draw_color(r,d));
}
int SDL_RenderDrawLine(SDL_Renderer *r, int x1, int y1, int x2, int y2){
	int dx = x2>x1?x2-x1:x1-x2, sx = x1<x2?1:-1;
	int dy = y2>y1?y2-y1:y1-y2, sy = y1<y2?1:-1;
	int err = (dx>dy?dx:-dy)/2, e2;
	for (;;) { SDL_RenderDrawPoint(r, x1, y1); if (x1==x2 && y1==y2) break; e2=err; if (e2>-dx){err-=dy;x1+=sx;} if (e2<dy){err+=dx;y1+=sy;} }
	return 0;
}
int SDL_RenderFillRect(SDL_Renderer *r, const SDL_Rect *rect){
	SDL_Surface *d=render_dst(); if(!d) return 0;
	return SDL_FillRect(d, rect, draw_color(r,d));
}
int SDL_RenderDrawRect(SDL_Renderer *r, const SDL_Rect *rect){
	SDL_Surface *d=render_dst(); if(!d) return 0;
	SDL_Rect rr; if (rect) rr=*rect; else { rr.x=0; rr.y=0; rr.w=d->w; rr.h=d->h; }
	int x=rr.x,y=rr.y,w=rr.w,h=rr.h;
	SDL_RenderDrawLine(r,x,y,x+w-1,y); SDL_RenderDrawLine(r,x,y+h-1,x+w-1,y+h-1);
	SDL_RenderDrawLine(r,x,y,x,y+h-1); SDL_RenderDrawLine(r,x+w-1,y,x+w-1,y+h-1);
	return 0;
}
int SDL_SetRenderTarget(SDL_Renderer *r, SDL_Texture *texture){ (void)r; g_render_target = texture; if(r) r->target=texture; return 0; }
SDL_Texture *SDL_CreateTexture(SDL_Renderer *r, Uint32 format, int access, int w, int h){
	(void)r; SDL_Texture *t=(SDL_Texture*)calloc(1,sizeof*t); if(!t) return NULL;
	t->format=format; t->access=access; t->amod=255; t->cmod_r=t->cmod_g=t->cmod_b=255;
	t->surface=new_surface(w,h,format,NULL,0); return t;
}
SDL_Texture *SDL_CreateTextureFromSurface(SDL_Renderer *r, SDL_Surface *s){
	(void)r; SDL_Texture *t=(SDL_Texture*)calloc(1,sizeof*t); if(!t) return NULL;
	t->format=s->format->format; t->amod=255; t->cmod_r=t->cmod_g=t->cmod_b=255;
	t->surface=SDL_ConvertSurface(s,s->format,0); return t;
}
void SDL_DestroyTexture(SDL_Texture *t){ if(t){ if(g_render_target==t) g_render_target=NULL; SDL_FreeSurface(t->surface); free(t); } }
int SDL_UpdateTexture(SDL_Texture *t, const SDL_Rect *rect, const void *px, int pitch){
	if(!t||!t->surface) return -1; SDL_Surface*s=t->surface;
	SDL_Rect r; if (rect) r=*rect; else { r.x=0; r.y=0; r.w=s->w; r.h=s->h; }
	int bpp=s->format->BytesPerPixel;
	for(int y=0;y<r.h;y++) memcpy((Uint8*)s->pixels+(size_t)(r.y+y)*s->pitch+(size_t)r.x*bpp, (const Uint8*)px+(size_t)y*pitch, (size_t)r.w*bpp);
	return 0;
}
int SDL_LockTexture(SDL_Texture *t, const SDL_Rect *rect, void **px, int *pitch){ (void)rect; if(!t||!t->surface)return -1; if(px)*px=t->surface->pixels; if(pitch)*pitch=t->surface->pitch; return 0; }
void SDL_UnlockTexture(SDL_Texture *t){ (void)t; }
int SDL_SetTextureBlendMode(SDL_Texture *t, int m){ if(t) t->blend=m; return 0; }
int SDL_GetTextureBlendMode(SDL_Texture *t, int *m){ if(t&&m)*m=t->blend; return 0; }
int SDL_SetTextureColorMod(SDL_Texture *t, Uint8 r, Uint8 g, Uint8 b){ if(t){t->cmod_r=r;t->cmod_g=g;t->cmod_b=b;} return 0; }
int SDL_SetTextureAlphaMod(SDL_Texture *t, Uint8 a){ if(t) t->amod=a; return 0; }
int SDL_QueryTexture(SDL_Texture *t, Uint32 *fmt, int *access, int *w, int *h){ if(!t)return -1; if(fmt)*fmt=t->format; if(access)*access=t->access; if(w)*w=t->surface->w; if(h)*h=t->surface->h; return 0; }
int SDL_GetRendererOutputSize(SDL_Renderer *r, int *w, int *h){ (void)r; fb_dims(w,h); return 0; }

/* ===================================================================== */
/* Timer                                                                  */
/* ===================================================================== */
Uint32 SDL_GetTicks(void){ return of_time_ms(); }
Uint64 SDL_GetTicks64(void){ return of_time_ms(); }
Uint64 SDL_GetPerformanceCounter(void){ return of_time_us(); }
Uint64 SDL_GetPerformanceFrequency(void){ return 1000000ULL; }
void SDL_Delay(Uint32 ms){
	audio_pump();
	while (ms-- > 0) { usleep(1000); audio_pump(); }
}
SDL_TimerID SDL_AddTimer(Uint32 i, SDL_TimerCallback cb, void *p){ (void)i;(void)cb;(void)p; return 0; }
SDL_bool SDL_RemoveTimer(SDL_TimerID id){ (void)id; return SDL_TRUE; }

/* ===================================================================== */
/* Input: keyboard state + event pump (gamepad -> SDL events)             */
/* ===================================================================== */
static Uint8  g_keystate[SDL_NUM_SCANCODES];
static Uint32 g_prev_buttons;
static int    g_prev_axes[4];
static int    g_mouse_x, g_mouse_y;
static Uint32 g_mouse_buttons;
static int    g_mouse_present;
static int    g_mouse_rel_x, g_mouse_rel_y;
/* Edges/motion consumed from firmware but not yet emitted as events; the
 * pump flushes them so state getters never grow the event queue (a getter
 * pushing events would keep the queue non-empty and starve the pump's
 * pad/keyboard synthesis in single-PollEvent-per-frame games). */
static Uint32 g_mouse_of_level;
static Uint32 g_mouse_pend_down, g_mouse_pend_up;
static int    g_mouse_pend_xrel, g_mouse_pend_yrel;
static int    g_text_input;
static SDL_Keymod g_modstate = KMOD_NONE;

/* Simple ring of synthesized events. */
#define EVQ 128
static SDL_Event g_evq[EVQ];
static int g_evhead, g_evtail;
static int evq_push(const SDL_Event *e){ int n=(g_evtail+1)%EVQ; if(n==g_evhead) return 0; g_evq[g_evtail]=*e; g_evtail=n; return 1; }
static int evq_pop(SDL_Event *e){ if(g_evhead==g_evtail) return 0; *e=g_evq[g_evhead]; g_evhead=(g_evhead+1)%EVQ; return 1; }

int SDL_PushEvent(SDL_Event *e){ return e ? evq_push(e) : 0; }
void SDL_PumpEvents(void){}

/* OF button bit -> SDL game-controller button. */
static SDL_GameControllerButton of_to_cbtn(uint32_t bit) {
	switch (bit) {
	case OF_BTN_A: return SDL_CONTROLLER_BUTTON_A;
	case OF_BTN_B: return SDL_CONTROLLER_BUTTON_B;
	case OF_BTN_X: return SDL_CONTROLLER_BUTTON_X;
	case OF_BTN_Y: return SDL_CONTROLLER_BUTTON_Y;
	case OF_BTN_L1: return SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
	case OF_BTN_R1: return SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
	case OF_BTN_L3: return SDL_CONTROLLER_BUTTON_LEFTSTICK;
	case OF_BTN_R3: return SDL_CONTROLLER_BUTTON_RIGHTSTICK;
	case OF_BTN_SELECT: return SDL_CONTROLLER_BUTTON_BACK;
	case OF_BTN_START: return SDL_CONTROLLER_BUTTON_START;
	case OF_BTN_UP: return SDL_CONTROLLER_BUTTON_DPAD_UP;
	case OF_BTN_DOWN: return SDL_CONTROLLER_BUTTON_DPAD_DOWN;
	case OF_BTN_LEFT: return SDL_CONTROLLER_BUTTON_DPAD_LEFT;
	case OF_BTN_RIGHT: return SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
	default: return SDL_CONTROLLER_BUTTON_INVALID;
	}
}
/* OF button bit -> SDL scancode (for the keyboard-event stream + keystate).
 * Default arcade-ish mapping; faithful enough for keyboard-driven ports. */
static SDL_Scancode of_to_scancode(uint32_t bit) {
	switch (bit) {
	case OF_BTN_UP: return SDL_SCANCODE_UP;
	case OF_BTN_DOWN: return SDL_SCANCODE_DOWN;
	case OF_BTN_LEFT: return SDL_SCANCODE_LEFT;
	case OF_BTN_RIGHT: return SDL_SCANCODE_RIGHT;
	case OF_BTN_A: return SDL_SCANCODE_LCTRL;   /* fire / select */
	case OF_BTN_B: return SDL_SCANCODE_SPACE;   /* jump / use */
	case OF_BTN_X: return SDL_SCANCODE_LALT;    /* strafe / alt */
	case OF_BTN_Y: return SDL_SCANCODE_LSHIFT;  /* run */
	case OF_BTN_L1: return SDL_SCANCODE_COMMA;
	case OF_BTN_R1: return SDL_SCANCODE_PERIOD;
	case OF_BTN_L2: return SDL_SCANCODE_PAGEDOWN;
	case OF_BTN_R2: return SDL_SCANCODE_PAGEUP;
	case OF_BTN_SELECT: return SDL_SCANCODE_TAB;
	case OF_BTN_START: return SDL_SCANCODE_ESCAPE;
	default: return SDL_SCANCODE_UNKNOWN;
	}
}

SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode s){
	if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return (SDL_Keycode)('a' + (s - SDL_SCANCODE_A));
	if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return (SDL_Keycode)('1' + (s - SDL_SCANCODE_1));
	switch (s) {
	case SDL_SCANCODE_0: return SDLK_0;
	case SDL_SCANCODE_RETURN: return SDLK_RETURN;
	case SDL_SCANCODE_ESCAPE: return SDLK_ESCAPE;
	case SDL_SCANCODE_BACKSPACE: return SDLK_BACKSPACE;
	case SDL_SCANCODE_TAB: return SDLK_TAB;
	case SDL_SCANCODE_SPACE: return SDLK_SPACE;
	default: return (SDL_Keycode)SDL_SCANCODE_TO_KEYCODE(s);
	}
}
SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode k){
	if (k >= 'a' && k <= 'z') return (SDL_Scancode)(SDL_SCANCODE_A + (k - 'a'));
	if (k & SDLK_SCANCODE_MASK) return (SDL_Scancode)(k & ~SDLK_SCANCODE_MASK);
	switch (k) {
	case SDLK_RETURN: return SDL_SCANCODE_RETURN;
	case SDLK_ESCAPE: return SDL_SCANCODE_ESCAPE;
	case SDLK_SPACE: return SDL_SCANCODE_SPACE;
	default: return SDL_SCANCODE_UNKNOWN;
	}
}

static void update_keystate(uint32_t buttons) {
	memset(g_keystate, 0, sizeof g_keystate);
	for (int bit = 0; bit < 16; bit++) {
		if (!(buttons & (1u << bit))) continue;
		SDL_Scancode sc = of_to_scancode(1u << bit);
		if (sc != SDL_SCANCODE_UNKNOWN) g_keystate[sc] = 1;
	}
}

/* OF mouse button index (0=L 1=R 2=M 3/4=extra) -> SDL button number.
 * Note the order difference: SDL numbers middle 2 and right 3. */
static const Uint8 of_to_mbtn[5] = {
	SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE, SDL_BUTTON_X1, SDL_BUTTON_X2,
};

/* Physical dock mouse -> SDL pointer model. The firmware read is
 * CONSUMING (dx/dy and the button edge masks clear on read), so this
 * must stay the ONLY caller of of_input_mouse_state(); every SDL entry
 * point shares the state cached here (cursor, button mask, relative
 * accumulator, pending events). Never-present mouse = no-op, so the
 * shim's behavior is unchanged on mouse-less setups. State only —
 * events are parked in g_mouse_pend_* for mouse_flush_events(). */
static void mouse_refresh(void) {
	of_mouse_state_t ms;
	of_input_mouse_state(&ms);
	if (!ms.present) {
		if (g_mouse_present) {
			/* Hot-unplug: the firmware latched release edges for whatever
			 * was held into this final read; without this the consumed
			 * edges vanish and the app sees the button held forever. */
			g_mouse_pend_down |= ms.buttons_pressed;
			g_mouse_pend_up   |= ms.buttons_released | g_mouse_of_level;
			g_mouse_of_level = 0;
			g_mouse_buttons = 0;
			g_mouse_present = 0;
		}
		return;
	}
	g_mouse_present = 1;
	g_mouse_pend_down |= ms.buttons_pressed;
	g_mouse_pend_up   |= ms.buttons_released;
	g_mouse_of_level = ms.buttons;
	Uint32 btns = 0;
	for (int i = 0; i < 5; i++)
		if (ms.buttons & (1u << i)) btns |= SDL_BUTTON(of_to_mbtn[i]);
	g_mouse_buttons = btns;
	if (ms.dx || ms.dy) {
		int w = g_window.w, h = g_window.h;
		if (w <= 0 || h <= 0) fb_dims(&w, &h);
		g_mouse_x += ms.dx; g_mouse_y += ms.dy;
		if (g_mouse_x < 0) g_mouse_x = 0; if (g_mouse_x >= w) g_mouse_x = w - 1;
		if (g_mouse_y < 0) g_mouse_y = 0; if (g_mouse_y >= h) g_mouse_y = h - 1;
		g_mouse_rel_x += ms.dx; g_mouse_rel_y += ms.dy;
		g_mouse_pend_xrel += ms.dx; g_mouse_pend_yrel += ms.dy;
	}
}

/* Emit the parked mouse events. Pump-only: getters refresh state but must
 * not push events (see g_mouse_pend_* comment). */
static void mouse_flush_events(void) {
	if (g_mouse_pend_xrel || g_mouse_pend_yrel) {
		SDL_Event e; memset(&e,0,sizeof e); e.type=SDL_MOUSEMOTION; e.motion.state=g_mouse_buttons;
		e.motion.x=g_mouse_x; e.motion.y=g_mouse_y; e.motion.xrel=g_mouse_pend_xrel; e.motion.yrel=g_mouse_pend_yrel; evq_push(&e);
		g_mouse_pend_xrel = g_mouse_pend_yrel = 0;
	}
	Uint32 down = g_mouse_pend_down, up = g_mouse_pend_up;
	g_mouse_pend_down = g_mouse_pend_up = 0;
	for (int i = 0; i < 5; i++) {
		Uint32 mask = 1u << i;
		SDL_Event e; memset(&e,0,sizeof e);
		e.button.button=of_to_mbtn[i]; e.button.clicks=1; e.button.x=g_mouse_x; e.button.y=g_mouse_y;
		/* Both edges in one interval: order by the final level, so a
		 * release+re-press ends DOWN and a sub-frame click ends UP. */
		if ((down & mask) && (g_mouse_of_level & mask)) {
			if (up & mask) { e.type=SDL_MOUSEBUTTONUP; e.button.state=SDL_RELEASED; evq_push(&e); }
			e.type=SDL_MOUSEBUTTONDOWN; e.button.state=SDL_PRESSED; evq_push(&e);
		} else {
			if (down & mask) { e.type=SDL_MOUSEBUTTONDOWN; e.button.state=SDL_PRESSED; evq_push(&e); }
			if (up & mask)   { e.type=SDL_MOUSEBUTTONUP;   e.button.state=SDL_RELEASED; evq_push(&e); }
		}
	}
}

static void poll_and_synthesize(void) {
	of_input_poll();
	of_input_state_t st; of_input_state(0, &st);
	uint32_t pressed = st.buttons & ~g_prev_buttons;
	uint32_t released = ~st.buttons & g_prev_buttons;
	for (int bit = 0; bit < 16; bit++) {
		uint32_t mask = 1u << bit;
#ifndef OF_SDL_NO_CONTROLLER_EVENTS
		SDL_GameControllerButton cb = of_to_cbtn(mask);
		if (cb != SDL_CONTROLLER_BUTTON_INVALID) {
			if (pressed & mask) { SDL_Event e; memset(&e,0,sizeof e); e.type=SDL_CONTROLLERBUTTONDOWN; e.cbutton.button=(Uint8)cb; e.cbutton.state=SDL_PRESSED; evq_push(&e); }
			if (released & mask){ SDL_Event e; memset(&e,0,sizeof e); e.type=SDL_CONTROLLERBUTTONUP;   e.cbutton.button=(Uint8)cb; e.cbutton.state=SDL_RELEASED; evq_push(&e); }
		}
#endif
#ifndef OF_SDL_NO_KEYBOARD_EVENTS
		SDL_Scancode sc = of_to_scancode(mask);
		if (sc != SDL_SCANCODE_UNKNOWN) {
			if (pressed & mask) { SDL_Event e; memset(&e,0,sizeof e); e.type=SDL_KEYDOWN; e.key.state=SDL_PRESSED; e.key.keysym.scancode=sc; e.key.keysym.sym=SDL_GetKeyFromScancode(sc); evq_push(&e); }
			if (released & mask){ SDL_Event e; memset(&e,0,sizeof e); e.type=SDL_KEYUP;   e.key.state=SDL_RELEASED; e.key.keysym.scancode=sc; e.key.keysym.sym=SDL_GetKeyFromScancode(sc); evq_push(&e); }
		}
#endif
	}
	/* Analog sticks -> controller axis events, only past a deadzone so the
	 * queue does not stay permanently non-empty (which would make a
	 * `while (PollEvent())` drain loop spin forever). */
	const int AXIS_EPS = 1024;
	int axv[4]; SDL_GameControllerAxis axid[4];
	axv[0]=st.joy_lx; axid[0]=SDL_CONTROLLER_AXIS_LEFTX;
	axv[1]=st.joy_ly; axid[1]=SDL_CONTROLLER_AXIS_LEFTY;
	axv[2]=st.joy_rx; axid[2]=SDL_CONTROLLER_AXIS_RIGHTX;
	axv[3]=st.joy_ry; axid[3]=SDL_CONTROLLER_AXIS_RIGHTY;
	for (int i = 0; i < 4; i++) {
		int d = axv[i] - g_prev_axes[i]; if (d < 0) d = -d;
		if (d <= AXIS_EPS) continue;
		g_prev_axes[i] = axv[i];
#ifndef OF_SDL_NO_CONTROLLER_EVENTS
		SDL_Event e; memset(&e,0,sizeof e); e.type=SDL_CONTROLLERAXISMOTION; e.caxis.axis=(Uint8)axid[i]; e.caxis.value=(Sint16)axv[i]; evq_push(&e);
#endif
	}
	g_prev_buttons = st.buttons;
	update_keystate(st.buttons);
	mouse_refresh();
	mouse_flush_events();
}

int SDL_PollEvent(SDL_Event *event) {
	/* Announce the virtual controller once so games that wait for
	 * SDL_CONTROLLERDEVICEADDED before opening it (DevilutionX) see it. */
	static int announced_pad = 0;
	if (!announced_pad) {
		announced_pad = 1;
#ifndef OF_SDL_NO_CONTROLLER_EVENTS
		SDL_Event added; memset(&added,0,sizeof added); added.type = SDL_CONTROLLERDEVICEADDED; added.cdevice.which = 0;
		evq_push(&added);
#endif
	}
	audio_pump();
	if (g_evhead == g_evtail) poll_and_synthesize();
	if (!event) { SDL_Event tmp; return evq_pop(&tmp); }
	return evq_pop(event);
}
int SDL_WaitEvent(SDL_Event *event){ for(;;){ if(SDL_PollEvent(event)) return 1; usleep(1000); } }
int SDL_WaitEventTimeout(SDL_Event *event, int timeout){
	uint32_t start = of_time_ms();
	do { if (SDL_PollEvent(event)) return 1; usleep(1000); } while ((int)(of_time_ms() - start) < timeout);
	return 0;
}
int SDL_PeepEvents(SDL_Event *events, int n, SDL_eventaction action, Uint32 mn, Uint32 mx){ (void)events;(void)n;(void)action;(void)mn;(void)mx; return 0; }
Uint32 SDL_RegisterEvents(int n){ static Uint32 next=SDL_USEREVENT; Uint32 r=next; next+=n; return r; }
SDL_bool SDL_HasEvent(Uint32 t){ (void)t; return (g_evhead!=g_evtail)?SDL_TRUE:SDL_FALSE; }
void SDL_FlushEvent(Uint32 t){ (void)t; }
void SDL_FlushEvents(Uint32 a, Uint32 b){ (void)a;(void)b; g_evhead=g_evtail=0; }
Uint8 SDL_EventState(Uint32 t, int s){ (void)t;(void)s; return 1; }
void SDL_SetEventFilter(SDL_EventFilter f, void *u){ (void)f;(void)u; }

const Uint8 *SDL_GetKeyboardState(int *numkeys){ if(numkeys)*numkeys=SDL_NUM_SCANCODES; return g_keystate; }
SDL_Keymod SDL_GetModState(void){ return g_modstate; }
void SDL_SetModState(SDL_Keymod m){ g_modstate = m; }
const char *SDL_GetKeyName(SDL_Keycode k){ (void)k; return ""; }
const char *SDL_GetScancodeName(SDL_Scancode s){ (void)s; return ""; }
void SDL_StartTextInput(void){ g_text_input=1; }
void SDL_StopTextInput(void){ g_text_input=0; }
SDL_bool SDL_IsTextInputActive(void){ return g_text_input?SDL_TRUE:SDL_FALSE; }
void SDL_SetTextInputRect(SDL_Rect *r){ (void)r; }
SDL_bool SDL_HasScreenKeyboardSupport(void){ return SDL_FALSE; }

/* ===================================================================== */
/* Mouse / cursor                                                         */
/* ===================================================================== */
/* Both state getters refresh first, so pure pollers that never (or
 * rarely) run the event pump still see live deltas; any events those
 * refreshes generate stay parked until the next pump. Without a mouse
 * the refresh is a no-op and g_mouse_buttons stays 0, so games see
 * exactly the old warp-echo / right-stick behavior. */
Uint32 SDL_GetMouseState(int *x, int *y){ mouse_refresh(); if(x)*x=g_mouse_x; if(y)*y=g_mouse_y; return g_mouse_buttons; }
Uint32 SDL_GetGlobalMouseState(int *x, int *y){ return SDL_GetMouseState(x,y); }
Uint32 SDL_GetRelativeMouseState(int *x, int *y){
	mouse_refresh();
	of_input_state_t st; of_input_state(0, &st);
	if (x) *x = (int)st.joy_rx / 4096 + g_mouse_rel_x;
	if (y) *y = (int)st.joy_ry / 4096 + g_mouse_rel_y;
	g_mouse_rel_x = 0; g_mouse_rel_y = 0;
	return g_mouse_buttons;
}
int SDL_SetRelativeMouseMode(SDL_bool enabled){ (void)enabled; return 0; }
void SDL_WarpMouseInWindow(SDL_Window *win, int x, int y){ (void)win; g_mouse_x=x; g_mouse_y=y; }
void SDL_WarpMouse(Uint16 x, Uint16 y){ g_mouse_x=x; g_mouse_y=y; }
void SDL_WarpMouseGlobal(int x, int y){ g_mouse_x=x; g_mouse_y=y; }
int SDL_ShowCursor(int toggle){ (void)toggle; return SDL_DISABLE; }
int SDL_CaptureMouse(SDL_bool e){ (void)e; return 0; }
static struct SDL_Cursor g_cursor;
SDL_Cursor *SDL_CreateCursor(const Uint8*d,const Uint8*m,int w,int h,int hx,int hy){ (void)d;(void)m;(void)w;(void)h;(void)hx;(void)hy; return &g_cursor; }
SDL_Cursor *SDL_CreateColorCursor(SDL_Surface *s,int hx,int hy){ (void)s;(void)hx;(void)hy; return &g_cursor; }
SDL_Cursor *SDL_CreateSystemCursor(SDL_SystemCursor id){ (void)id; return &g_cursor; }
void SDL_SetCursor(SDL_Cursor *c){ (void)c; }
SDL_Cursor *SDL_GetDefaultCursor(void){ return &g_cursor; }
void SDL_FreeCursor(SDL_Cursor *c){ (void)c; }

/* ===================================================================== */
/* Joystick / Game controller (single virtual pad = OF player 0)          */
/* ===================================================================== */
static struct SDL_GameController g_pad;
static struct SDL_Joystick g_joy;
int SDL_NumJoysticks(void){ return 1; }
SDL_bool SDL_IsGameController(int i){ return i==0?SDL_TRUE:SDL_FALSE; }
SDL_GameController *SDL_GameControllerOpen(int i){ (void)i; return &g_pad; }
void SDL_GameControllerClose(SDL_GameController *c){ (void)c; }
SDL_GameController *SDL_GameControllerFromInstanceID(SDL_JoystickID id){ (void)id; return &g_pad; }
const char *SDL_GameControllerName(SDL_GameController *c){ (void)c; return "Analogue Pocket"; }
SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *c){ (void)c; return &g_joy; }
SDL_bool SDL_GameControllerGetButton(SDL_GameController *c, SDL_GameControllerButton b){
	(void)c; of_input_state_t st; of_input_state(0,&st);
	switch(b){
	case SDL_CONTROLLER_BUTTON_A: return (st.buttons&OF_BTN_A)?SDL_TRUE:SDL_FALSE;
	case SDL_CONTROLLER_BUTTON_B: return (st.buttons&OF_BTN_B)?SDL_TRUE:SDL_FALSE;
	case SDL_CONTROLLER_BUTTON_X: return (st.buttons&OF_BTN_X)?SDL_TRUE:SDL_FALSE;
	case SDL_CONTROLLER_BUTTON_Y: return (st.buttons&OF_BTN_Y)?SDL_TRUE:SDL_FALSE;
	case SDL_CONTROLLER_BUTTON_BACK: return (st.buttons&OF_BTN_SELECT)?SDL_TRUE:SDL_FALSE;
	case SDL_CONTROLLER_BUTTON_START: return (st.buttons&OF_BTN_START)?SDL_TRUE:SDL_FALSE;
	case SDL_CONTROLLER_BUTTON_LEFTSTICK: return (st.buttons&OF_BTN_L3)?SDL_TRUE:SDL_FALSE;
	case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return (st.buttons&OF_BTN_R3)?SDL_TRUE:SDL_FALSE;
	case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return (st.buttons&OF_BTN_L1)?SDL_TRUE:SDL_FALSE;
	case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return (st.buttons&OF_BTN_R1)?SDL_TRUE:SDL_FALSE;
	case SDL_CONTROLLER_BUTTON_DPAD_UP: return (st.buttons&OF_BTN_UP)?SDL_TRUE:SDL_FALSE;
	case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return (st.buttons&OF_BTN_DOWN)?SDL_TRUE:SDL_FALSE;
	case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return (st.buttons&OF_BTN_LEFT)?SDL_TRUE:SDL_FALSE;
	case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return (st.buttons&OF_BTN_RIGHT)?SDL_TRUE:SDL_FALSE;
	default: return SDL_FALSE;
	}
}
Sint16 SDL_GameControllerGetAxis(SDL_GameController *c, SDL_GameControllerAxis a){
	(void)c; of_input_state_t st; of_input_state(0,&st);
	switch(a){
	case SDL_CONTROLLER_AXIS_LEFTX: return st.joy_lx; case SDL_CONTROLLER_AXIS_LEFTY: return st.joy_ly;
	case SDL_CONTROLLER_AXIS_RIGHTX: return st.joy_rx; case SDL_CONTROLLER_AXIS_RIGHTY: return st.joy_ry;
	case SDL_CONTROLLER_AXIS_TRIGGERLEFT: return (Sint16)st.trigger_l; case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return (Sint16)st.trigger_r;
	default: return 0;
	}
}
SDL_bool SDL_GameControllerHasButton(SDL_GameController *c, SDL_GameControllerButton b){ (void)c; (void)b; return SDL_TRUE; }
SDL_GameControllerButtonBind SDL_GameControllerGetBindForButton(SDL_GameController *c, SDL_GameControllerButton b){ (void)c;(void)b; SDL_GameControllerButtonBind r; memset(&r,0,sizeof r); r.bindType=SDL_CONTROLLER_BINDTYPE_BUTTON; return r; }
const char *SDL_GameControllerGetStringForButton(SDL_GameControllerButton b){ (void)b; return ""; }
SDL_GameControllerButton SDL_GameControllerGetButtonFromString(const char *s){ (void)s; return SDL_CONTROLLER_BUTTON_INVALID; }
const char *SDL_GameControllerGetStringForAxis(SDL_GameControllerAxis a){ (void)a; return ""; }
SDL_GameControllerAxis SDL_GameControllerGetAxisFromString(const char *s){ (void)s; return SDL_CONTROLLER_AXIS_INVALID; }
int SDL_GameControllerAddMapping(const char *m){ (void)m; return 0; }
void SDL_GameControllerUpdate(void){}
int SDL_GameControllerEventState(int state){ (void)state; return 1; }
SDL_GameControllerType SDL_GameControllerGetType(SDL_GameController *c){ (void)c; return SDL_CONTROLLER_TYPE_VIRTUAL; }
SDL_GameControllerType SDL_GameControllerTypeForIndex(int i){ (void)i; return SDL_CONTROLLER_TYPE_VIRTUAL; }
char *SDL_GameControllerMappingForGUID(SDL_JoystickGUID g){ (void)g; return NULL; }
char *SDL_GameControllerMapping(SDL_GameController *c){ (void)c; return NULL; }
SDL_Joystick *SDL_JoystickOpen(int i){ (void)i; return &g_joy; }
SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *j){ (void)j; return 0; }
const char *SDL_JoystickName(SDL_Joystick *j){ (void)j; return "Analogue Pocket"; }
const char *SDL_JoystickNameForIndex(int i){ (void)i; return "Analogue Pocket"; }
int SDL_JoystickNumButtons(SDL_Joystick *j){ (void)j; return 16; }
int SDL_JoystickNumAxes(SDL_Joystick *j){ (void)j; return 6; }
int SDL_JoystickNumHats(SDL_Joystick *j){ (void)j; return 1; }
Uint8 SDL_JoystickGetButton(SDL_Joystick *j, int b){ (void)j; return SDL_GameControllerGetButton(&g_pad, (SDL_GameControllerButton)b) ? 1 : 0; }
Sint16 SDL_JoystickGetAxis(SDL_Joystick *j, int a){ (void)j; return SDL_GameControllerGetAxis(&g_pad, (SDL_GameControllerAxis)a); }
Uint8 SDL_JoystickGetHat(SDL_Joystick *j, int h){
	(void)j;(void)h; of_input_state_t st; of_input_state(0,&st); Uint8 out=SDL_HAT_CENTERED;
	if (st.buttons&OF_BTN_UP) out|=SDL_HAT_UP; if (st.buttons&OF_BTN_DOWN) out|=SDL_HAT_DOWN;
	if (st.buttons&OF_BTN_LEFT) out|=SDL_HAT_LEFT; if (st.buttons&OF_BTN_RIGHT) out|=SDL_HAT_RIGHT;
	return out;
}
SDL_JoystickGUID SDL_JoystickGetGUID(SDL_Joystick *j){ (void)j; SDL_JoystickGUID g; memset(&g,0,sizeof g); return g; }
void SDL_JoystickClose(SDL_Joystick *j){ (void)j; }
int SDL_JoystickEventState(int state){ (void)state; return 1; }
void SDL_JoystickUpdate(void){}
SDL_bool SDL_JoystickGetAttached(SDL_Joystick *j){ (void)j; return SDL_TRUE; }

/* ===================================================================== */
/* Threads / mutex / cond / sem (cooperative, single core)                */
/* ===================================================================== */
SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data) {
	(void)name;
	SDL_Thread *t = (SDL_Thread *)calloc(1, sizeof *t);
	if (!t) return NULL;
	/* No preemptive threads: run the body synchronously. A long-running
	 * thread body therefore never returns -- games that spawn a worker
	 * loop must be structured to pump cooperatively. */
	if (fn) t->result = fn(data);
	t->done = 1;
	return t;
}
void SDL_WaitThread(SDL_Thread *t, int *status){ if(t){ if(status)*status=t->result; free(t);} }
void SDL_DetachThread(SDL_Thread *t){ free(t); }
SDL_threadID SDL_GetThreadID(SDL_Thread *t){ return (SDL_threadID)(uintptr_t)t; }
SDL_threadID SDL_ThreadID(void){ return 1; }
const char *SDL_GetThreadName(SDL_Thread *t){ (void)t; return ""; }

SDL_mutex *SDL_CreateMutex(void){ return (SDL_mutex*)calloc(1,sizeof(SDL_mutex)); }
int SDL_LockMutex(SDL_mutex *m){ if(m)m->locked++; return 0; }
int SDL_TryLockMutex(SDL_mutex *m){ if(m)m->locked++; return 0; }
int SDL_UnlockMutex(SDL_mutex *m){ if(m&&m->locked)m->locked--; return 0; }
void SDL_DestroyMutex(SDL_mutex *m){ free(m); }
SDL_cond *SDL_CreateCond(void){ return (SDL_cond*)calloc(1,sizeof(SDL_cond)); }
void SDL_DestroyCond(SDL_cond *c){ free(c); }
int SDL_CondSignal(SDL_cond *c){ (void)c; return 0; }
int SDL_CondBroadcast(SDL_cond *c){ (void)c; return 0; }
int SDL_CondWait(SDL_cond *c, SDL_mutex *m){ (void)c;(void)m; return 0; }
int SDL_CondWaitTimeout(SDL_cond *c, SDL_mutex *m, Uint32 ms){ (void)c;(void)m;(void)ms; return SDL_MUTEX_TIMEDOUT; }
SDL_sem *SDL_CreateSemaphore(Uint32 v){ SDL_sem*s=(SDL_sem*)calloc(1,sizeof(SDL_sem)); if(s)s->v=v; return s; }
void SDL_DestroySemaphore(SDL_sem *s){ free(s); }
int SDL_SemWait(SDL_sem *s){ if(s&&s->v)s->v--; return 0; }
int SDL_SemTryWait(SDL_sem *s){ if(s&&s->v){s->v--; return 0;} return SDL_MUTEX_TIMEDOUT; }
int SDL_SemWaitTimeout(SDL_sem *s, Uint32 ms){ (void)ms; return SDL_SemTryWait(s); }
int SDL_SemPost(SDL_sem *s){ if(s)s->v++; return 0; }
Uint32 SDL_SemValue(SDL_sem *s){ return s?s->v:0; }

/* ===================================================================== */
/* Hints / clipboard / messagebox / filesystem / cpu                      */
/* ===================================================================== */
SDL_bool SDL_SetHint(const char *n, const char *v){ (void)n;(void)v; return SDL_TRUE; }
SDL_bool SDL_SetHintWithPriority(const char *n, const char *v, SDL_HintPriority p){ (void)n;(void)v;(void)p; return SDL_TRUE; }
const char *SDL_GetHint(const char *n){ (void)n; return NULL; }
int SDL_SetClipboardText(const char *t){ (void)t; return 0; }
char *SDL_GetClipboardText(void){ return strdup(""); }
SDL_bool SDL_HasClipboardText(void){ return SDL_FALSE; }
int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *msg, SDL_Window *win){
	(void)flags;(void)win; printf("[msgbox] %s: %s\n", title?title:"", msg?msg:""); return 0;
}
char *SDL_GetBasePath(void){ return strdup("/"); }
char *SDL_GetPrefPath(const char *org, const char *app){ (void)org;(void)app; return strdup("/"); }
int SDL_GetCPUCount(void){ return 1; }
int SDL_GetSystemRAM(void){ const struct of_capabilities*c=of_get_caps(); return c?(int)(c->sdram_size/(1024*1024)):64; }
int SDL_EnableUNICODE(int e){ (void)e; return 0; }

/* ===================================================================== */
/* BMP (minimal: uncompressed 8/24/32-bit)                                */
/* ===================================================================== */
SDL_Surface *SDL_LoadBMP_RW(SDL_RWops *src, int freesrc) {
	SDL_Surface *out = NULL;
	Uint8 hdr[54];
	if (!src) return NULL;
	if (src->read(src, hdr, 1, 54) != 54) goto done;
	if (hdr[0] != 'B' || hdr[1] != 'M') goto done;
	{
		Uint32 dataoff = hdr[10]|(hdr[11]<<8)|(hdr[12]<<16)|((Uint32)hdr[13]<<24);
		int w = (int)(hdr[18]|(hdr[19]<<8)|(hdr[20]<<16)|((Uint32)hdr[21]<<24));
		int h = (int)(hdr[22]|(hdr[23]<<8)|(hdr[24]<<16)|((Uint32)hdr[25]<<24));
		int bpp = hdr[28]|(hdr[29]<<8);
		int top_down = 0; if (h < 0) { h = -h; top_down = 1; }
		if (w <= 0 || h <= 0 || (bpp != 8 && bpp != 24 && bpp != 32)) goto done;
		Uint32 fmt = bpp==8 ? SDL_PIXELFORMAT_INDEX8 : (bpp==32 ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_RGB888);
		out = new_surface(w, h, fmt, NULL, 0);
		if (!out) goto done;
		if (bpp == 8 && out->format->palette) {
			int ncol = (int)((dataoff - 54) / 4); if (ncol > 256) ncol = 256; if (ncol <= 0) ncol = 256;
			Uint8 pe[4];
			for (int i = 0; i < ncol; i++) { if (src->read(src, pe, 1, 4) != 4) break; SDL_Color c; c.b=pe[0]; c.g=pe[1]; c.r=pe[2]; c.a=255; out->format->palette->colors[i]=c; }
		}
		src->seek(src, dataoff, RW_SEEK_SET);
		int srcpitch = ((w * (bpp/8)) + 3) & ~3;
		Uint8 *row = (Uint8 *)malloc(srcpitch);
		if (!row) { SDL_FreeSurface(out); out=NULL; goto done; }
		for (int y = 0; y < h; y++) {
			if (src->read(src, row, 1, srcpitch) != (size_t)srcpitch) break;
			int dy = top_down ? y : (h-1-y);
			Uint8 *drow = (Uint8*)out->pixels + (size_t)dy*out->pitch;
			if (bpp == 8) memcpy(drow, row, w);
			else {
				Uint32 *d32 = (Uint32*)drow; int bytes = bpp/8;
				for (int x = 0; x < w; x++) { Uint8 *p = row + x*bytes; Uint32 a = bpp==32 ? p[3] : 255; d32[x] = (a<<24)|((Uint32)p[2]<<16)|((Uint32)p[1]<<8)|p[0]; }
			}
		}
		free(row);
	}
done:
	if (freesrc && src) src->close(src);
	return out;
}
int SDL_SaveBMP_RW(SDL_Surface *surface, SDL_RWops *dst, int freedst) {
	(void)surface;
	if (dst && freedst) dst->close(dst);
	return -1;  /* writing not implemented */
}

/* ===================================================================== */
/* Audio: callback (auto-pumped), queue, WAV, CVT, mix                     */
/* ===================================================================== */
static SDL_AudioCallback __sdl_audio_cb;
static void             *__sdl_audio_userdata;
static int               __sdl_audio_paused = 1;
static int               __sdl_audio_opened;

static void audio_pump(void) {
	if (!__sdl_audio_cb || __sdl_audio_paused) return;
	int16_t buf[2048];   /* up to 1024 stereo frames per write */
	for (int iter = 0; iter < 8; iter++) {
		int free_frames = of_audio_free();
		if (free_frames <= 0) break;
		int n = free_frames > 1024 ? 1024 : free_frames;
		__sdl_audio_cb(__sdl_audio_userdata, (Uint8 *)buf, n * 4);
		of_audio_write(buf, n);
		if (n < 1024) break;
	}
}
void SDL_AudioPump(void) { audio_pump(); }

/* Ring capacity in stereo pairs, measured at runtime: of_audio_free()
 * reports the full per-target ring depth while the stream voice is
 * inactive (i.e. right after of_audio_init).  The build-time
 * OF_AUDIO_RING_PAIRS constant only matches the pocket target (mister
 * and sim reserve smaller rings), so queued-size math must not use it. */
static int __sdl_audio_ring_pairs;
static void __sdl_audio_measure_ring(void) {
	if (!__sdl_audio_ring_pairs)
		__sdl_audio_ring_pairs = of_audio_free();
}

SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture,
        const SDL_AudioSpec *desired, SDL_AudioSpec *obtained, int allowed_changes) {
	(void)device; (void)iscapture; (void)allowed_changes;
	if (!desired) return 0;
	of_audio_init();
	__sdl_audio_measure_ring();
	/* Route the requested rate through the mixer's stream voice so it is
	 * resampled to the hardware 48 kHz; without this, 22 kHz output would
	 * pitch up. */
	if (desired->freq > 0) of_audio_stream_open(desired->freq);
	if (desired->callback) { __sdl_audio_cb = desired->callback; __sdl_audio_userdata = desired->userdata; }
	__sdl_audio_opened = 1;
	__sdl_audio_paused = 1;  /* SDL opens paused; SDL_PauseAudioDevice(dev,0) starts */
	if (obtained) {
		*obtained = *desired;
		obtained->channels = 2;
		obtained->format = AUDIO_S16SYS;
		if (obtained->samples == 0) obtained->samples = 1024;
		obtained->silence = 0;
		obtained->size = (Uint32)obtained->samples * 4u;
	}
	return 1;
}
void SDL_CloseAudioDevice(SDL_AudioDeviceID d) { (void)d; __sdl_audio_cb=0; __sdl_audio_paused=1; __sdl_audio_opened=0; }
void SDL_PauseAudioDevice(SDL_AudioDeviceID d, int pause_on) { (void)d; __sdl_audio_paused = pause_on ? 1 : 0; }
void SDL_LockAudioDevice(SDL_AudioDeviceID d) { (void)d; }
void SDL_UnlockAudioDevice(SDL_AudioDeviceID d) { (void)d; }
SDL_AudioStatus SDL_GetAudioDeviceStatus(SDL_AudioDeviceID d) { (void)d; return __sdl_audio_opened ? (__sdl_audio_paused ? SDL_AUDIO_PAUSED : SDL_AUDIO_PLAYING) : SDL_AUDIO_STOPPED; }
int SDL_GetNumAudioDevices(int cap){ (void)cap; return 1; }
const char *SDL_GetAudioDeviceName(int i,int cap){ (void)i;(void)cap; return "openfpga"; }

/* SDL 1.2 single-device audio API */
int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained) {
	SDL_AudioSpec tmp;
	SDL_AudioDeviceID id = SDL_OpenAudioDevice(NULL, 0, desired, obtained ? obtained : &tmp, 0);
	return id ? 0 : -1;
}
void SDL_CloseAudio(void) { SDL_CloseAudioDevice(1); }
void SDL_PauseAudio(int pause_on) { SDL_PauseAudioDevice(1, pause_on); }
void SDL_LockAudio(void) {}
void SDL_UnlockAudio(void) {}
SDL_AudioStatus SDL_GetAudioStatus(void) { return SDL_GetAudioDeviceStatus(1); }

int SDL_QueueAudio(SDL_AudioDeviceID d, const void *data, Uint32 len) {
	(void)d;
	if (!__sdl_audio_opened) { of_audio_init(); __sdl_audio_measure_ring(); __sdl_audio_opened = 1; __sdl_audio_paused = 0; }
	of_audio_write((const int16_t *)data, (int)(len / 4));
	return 0;
}
Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID d){ (void)d; int cap = __sdl_audio_ring_pairs ? __sdl_audio_ring_pairs : OF_AUDIO_RING_PAIRS; int f = of_audio_free(); int q = cap - (f>0?f:0); return (Uint32)(q>0?q:0) * 4u; }
void SDL_ClearQueuedAudio(SDL_AudioDeviceID d){ (void)d; }

static SDL_AudioSpec *load_wav_mem(const Uint8 *data, Uint32 size, SDL_AudioSpec *spec, Uint8 **buf, Uint32 *len) {
	of_codec_result_t res;
	if (!data || !spec || !buf || !len || of_codec_parse_wav(data, size, &res) < 0) return NULL;
	spec->freq = (int)res.sample_rate;
	spec->format = (res.bits_per_sample == 16) ? AUDIO_S16SYS : AUDIO_U8;
	spec->channels = res.channels; spec->silence = 0; spec->samples = 4096;
	spec->size = res.pcm_len; spec->callback = 0; spec->userdata = 0; spec->padding = 0;
	Uint8 *pcm = (Uint8 *)malloc(res.pcm_len ? res.pcm_len : 1);
	if (!pcm) return NULL;
	memcpy(pcm, res.pcm, res.pcm_len);
	*buf = pcm; *len = res.pcm_len;
	return spec;
}
SDL_AudioSpec *SDL_LoadWAV_RW(SDL_RWops *src, int freesrc, SDL_AudioSpec *spec, Uint8 **audio_buf, Uint32 *audio_len) {
	if (audio_buf) *audio_buf = 0; if (audio_len) *audio_len = 0;
	if (!src) return NULL;
	Sint64 sz = src->size ? src->size(src) : -1;
	SDL_AudioSpec *ret = NULL;
	if (sz > 0 && sz <= 16*1024*1024) {
		Uint8 *data = (Uint8 *)malloc((size_t)sz);
		if (data) {
			if (src->seek) src->seek(src, 0, RW_SEEK_SET);
			size_t got = src->read ? src->read(src, data, 1, (size_t)sz) : 0;
			if (got == (size_t)sz) ret = load_wav_mem(data, (Uint32)sz, spec, audio_buf, audio_len);
			free(data);
		}
	}
	if (freesrc && src->close) src->close(src);
	return ret;
}
void SDL_FreeWAV(Uint8 *buf) { free(buf); }

int SDL_BuildAudioCVT(SDL_AudioCVT *cvt, SDL_AudioFormat src_format, Uint8 src_channels, int src_rate,
                      SDL_AudioFormat dst_format, Uint8 dst_channels, int dst_rate) {
	if (!cvt) return -1;
	memset(cvt, 0, sizeof *cvt);
	cvt->src_format = src_format; cvt->dst_format = dst_format;
	cvt->len_mult = 1; cvt->len_ratio = 1.0; cvt->rate_incr = 1.0;
	cvt->needed = (src_format != dst_format || src_channels != dst_channels || src_rate != dst_rate);
	return cvt->needed;
}
int SDL_ConvertAudio(SDL_AudioCVT *cvt) {
	if (!cvt) return -1;
	cvt->len_cvt = cvt->len;  /* minimal: format already matches device path */
	return 0;
}
void SDL_MixAudioFormat(Uint8 *dst, const Uint8 *src, SDL_AudioFormat fmt, Uint32 len, int volume) {
	(void)fmt;
	const int16_t *s = (const int16_t *)src; int16_t *d = (int16_t *)dst;
	for (Uint32 i = 0; i < len/2; i++) {
		int32_t m = (int32_t)d[i] + (((int32_t)s[i] * volume) >> 7);
		if (m > 32767) m = 32767; if (m < -32768) m = -32768; d[i] = (int16_t)m;
	}
}
void SDL_MixAudio(Uint8 *dst, const Uint8 *src, Uint32 len, int volume) {
	SDL_MixAudioFormat(dst, src, AUDIO_S16SYS, len, volume);
}

/* ===================================================================== */
/* SDL_mixer (SFX via of_mixer, music via of_midi)                        */
/* ===================================================================== */
static int __mix_initialized;
static int __mix_max_channels = 8;
static of_mixer_handle_t __mix_voice_ids[MIX_CHANNELS];
static int __mix_channel_group[MIX_CHANNELS];
static int __mix_channel_volume[MIX_CHANNELS];
static int __mix_freq = OF_AUDIO_RATE;
static Uint16 __mix_format = AUDIO_S16SYS;
static int __mix_channels = 2;
static Mix_Music *__mix_current_music;

int Mix_Init(int flags) { return flags; }
void Mix_Quit(void) { __mix_initialized = 0; }
const char *Mix_GetError(void) { return SDL_GetError(); }
void Mix_SetError(const char *fmt, ...) { va_list a; va_start(a,fmt); vsnprintf(g_error,sizeof g_error,fmt,a); va_end(a); }

int Mix_OpenAudio(int freq, Uint16 fmt, int ch, int chunksize) {
	(void)chunksize;
	__mix_freq = freq > 0 ? freq : OF_AUDIO_RATE;
	__mix_format = fmt ? fmt : AUDIO_S16SYS;
	__mix_channels = ch > 0 ? ch : 2;
	if (!__mix_initialized) {
		of_audio_init();
		of_mixer_init(MIX_CHANNELS, OF_AUDIO_RATE);
		for (int i = 0; i < MIX_CHANNELS; i++) { __mix_voice_ids[i]=OF_MIXER_HANDLE_INVALID; __mix_channel_group[i]=-1; __mix_channel_volume[i]=MIX_MAX_VOLUME; }
		__mix_max_channels = MIX_CHANNELS;
		__mix_initialized = 1;
	}
	return 0;
}
int Mix_OpenAudioDevice(int freq, Uint16 fmt, int ch, int chunksize, const char *dev, int allowed) {
	(void)dev; (void)allowed; return Mix_OpenAudio(freq, fmt, ch, chunksize);
}
void Mix_CloseAudio(void) { if (__mix_initialized) of_mixer_stop_all(); __mix_initialized = 0; }
int Mix_QuerySpec(int *freq, Uint16 *fmt, int *ch) {
	if (freq) *freq = __mix_freq; if (fmt) *fmt = __mix_format; if (ch) *ch = __mix_channels;
	return __mix_initialized ? 1 : 0;
}
int Mix_AllocateChannels(int n) { if (n >= 0) __mix_max_channels = n > MIX_CHANNELS ? MIX_CHANNELS : n; return __mix_max_channels; }

static int16_t mix_rd_s16le(const Uint8 *p) { return (int16_t)((Uint16)p[0] | ((Uint16)p[1] << 8)); }
static Mix_Chunk *mix_chunk_from_audio(const SDL_AudioSpec *spec, const Uint8 *audio, Uint32 audio_len) {
	if (!spec || !audio || audio_len == 0) return NULL;
	int channels = spec->channels > 0 ? spec->channels : 1;
	int bytes = (spec->format == AUDIO_U8) ? 1 : 2;
	uint32_t nsmp = audio_len / (uint32_t)(bytes * channels);
	if (nsmp == 0) return NULL;
	Mix_Chunk *c = (Mix_Chunk *)calloc(1, sizeof *c);
	if (!c) return NULL;
	int16_t *pcm = (int16_t *)malloc(nsmp * sizeof(int16_t));
	if (!pcm) { free(c); return NULL; }
	if (spec->format == AUDIO_U8) { int step=channels; for (uint32_t i=0;i<nsmp;i++) pcm[i]=(int16_t)(((int)audio[i*step]-128)<<8); }
	else { int step=channels*2; for (uint32_t i=0;i<nsmp;i++) pcm[i]=mix_rd_s16le(audio+i*step); }
	c->allocated=1; c->abuf=(Uint8*)pcm; c->alen=(Uint32)(nsmp*sizeof(int16_t)); c->volume=MIX_MAX_VOLUME;
	c->pcm_s16=pcm; c->sample_count=nsmp; c->sample_rate=(uint32_t)(spec->freq>0?spec->freq:OF_AUDIO_RATE);
	return c;
}
Mix_Chunk *Mix_LoadWAV_RW(SDL_RWops *src, int freesrc) {
	SDL_AudioSpec spec; Uint8 *audio=NULL; Uint32 alen=0;
	if (!SDL_LoadWAV_RW(src, freesrc, &spec, &audio, &alen)) return NULL;
	Mix_Chunk *c = mix_chunk_from_audio(&spec, audio, alen);
	SDL_FreeWAV(audio);
	return c;
}
Mix_Chunk *Mix_LoadWAV(const char *file) { return Mix_LoadWAV_RW(SDL_RWFromFile(file, "rb"), 1); }
Mix_Chunk *Mix_QuickLoad_RAW(Uint8 *mem, Uint32 len) {
	Mix_Chunk *c=(Mix_Chunk*)calloc(1,sizeof*c); if(!c) return NULL;
	c->allocated=0; c->abuf=mem; c->alen=len; c->volume=MIX_MAX_VOLUME;
	c->pcm_s16=(int16_t*)mem; c->sample_count=len/2; c->sample_rate=OF_AUDIO_RATE; return c;
}
void Mix_FreeChunk(Mix_Chunk *c) { if (!c) return; if (c->allocated && c->abuf) free(c->abuf); free(c); }
int Mix_VolumeChunk(Mix_Chunk *c, int volume) { if (!c) return -1; int old=c->volume; if (volume>=0) c->volume=volume>MIX_MAX_VOLUME?MIX_MAX_VOLUME:volume; return old; }

int Mix_PlayChannelTimed(int channel, Mix_Chunk *chunk, int loops, int ticks) {
	(void)loops; (void)ticks;
	if (!chunk || !chunk->pcm_s16) return -1;
	if (!__mix_initialized && Mix_OpenAudio(__mix_freq, __mix_format, __mix_channels, 1024) < 0) return -1;
	if (channel < 0) {
		for (int i = 0; i < __mix_max_channels; i++)
			if (__mix_voice_ids[i]==OF_MIXER_HANDLE_INVALID || !of_mixer_handle_active(__mix_voice_ids[i])) { channel=i; break; }
		if (channel < 0) channel = 0;
	}
	int mixvol = (channel < MIX_CHANNELS) ? __mix_channel_volume[channel] : MIX_MAX_VOLUME;
	int vol = ((chunk->volume * mixvol) / MIX_MAX_VOLUME) * 255 / MIX_MAX_VOLUME;
	of_mixer_handle_t v = of_mixer_play_h((const uint8_t*)chunk->pcm_s16, chunk->sample_count, chunk->sample_rate, 0, vol);
	if (v == OF_MIXER_HANDLE_INVALID) return -1;
	if (channel < MIX_CHANNELS) __mix_voice_ids[channel] = v;
	return channel;
}
int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops) { return Mix_PlayChannelTimed(channel, chunk, loops, -1); }
void Mix_HaltChannel(int channel) {
	if (!__mix_initialized) return;
	if (channel < 0) { of_mixer_stop_all(); return; }
	if (channel < MIX_CHANNELS && __mix_voice_ids[channel] != OF_MIXER_HANDLE_INVALID) of_mixer_stop_h(__mix_voice_ids[channel]);
}
int Mix_Playing(int channel) {
	if (!__mix_initialized) return 0;
	if (channel < 0) { int n=0; for (int i=0;i<MIX_CHANNELS;i++) if (__mix_voice_ids[i]!=OF_MIXER_HANDLE_INVALID && of_mixer_handle_active(__mix_voice_ids[i])) n++; return n; }
	if (channel >= MIX_CHANNELS) return 0;
	return (__mix_voice_ids[channel]!=OF_MIXER_HANDLE_INVALID && of_mixer_handle_active(__mix_voice_ids[channel])) ? 1 : 0;
}
void Mix_Pause(int ch)  { (void)ch; }
void Mix_Resume(int ch) { (void)ch; }
int Mix_ReserveChannels(int num) { (void)num; return 0; }
int Mix_GroupChannel(int which, int tag) { if (which>=0 && which<MIX_CHANNELS) __mix_channel_group[which]=tag; return 1; }
int Mix_GroupChannels(int from, int to, int tag) {
	if (from < 0) from = 0; if (to >= MIX_CHANNELS) to = MIX_CHANNELS-1;
	for (int i=from;i<=to;i++) __mix_channel_group[i]=tag;
	return (to>=from)?(to-from+1):0;
}
int Mix_GroupAvailable(int tag) {
	for (int i=0;i<MIX_CHANNELS;i++)
		if (__mix_channel_group[i]==tag && (__mix_voice_ids[i]==OF_MIXER_HANDLE_INVALID || !of_mixer_handle_active(__mix_voice_ids[i]))) return i;
	return -1;
}
int Mix_GroupOldest(int tag) { for (int i=0;i<MIX_CHANNELS;i++) if (__mix_channel_group[i]==tag) return i; return -1; }
int Mix_Volume(int channel, int volume) {
	if (channel < 0) { for (int i=0;i<MIX_CHANNELS;i++) Mix_Volume(i, volume); return volume; }
	if (channel >= MIX_CHANNELS) return -1;
	int old = __mix_channel_volume[channel];
	if (volume >= 0) {
		if (volume > MIX_MAX_VOLUME) volume = MIX_MAX_VOLUME;
		__mix_channel_volume[channel] = volume;
		if (__mix_voice_ids[channel] != OF_MIXER_HANDLE_INVALID) of_mixer_set_volume_h(__mix_voice_ids[channel], (volume*255)/MIX_MAX_VOLUME);
	}
	return old;
}
int Mix_SetPanning(int channel, Uint8 left, Uint8 right) {
	if (channel < 0 || channel >= MIX_CHANNELS) return 0;
	if (__mix_voice_ids[channel] != OF_MIXER_HANDLE_INVALID) of_mixer_set_vol_lr_h(__mix_voice_ids[channel], left, right);
	return 1;
}
void Mix_ChannelFinished(void (*cb)(int)) { (void)cb; }
void Mix_SetPostMix(void (*f)(void*,Uint8*,int), void *arg) { (void)f; (void)arg; }

Mix_Music *Mix_LoadMUS_RW(SDL_RWops *src, int freesrc) {
	if (!src) return NULL;
	Sint64 sz = src->size ? src->size(src) : -1;
	if (sz <= 0 || sz > 4*1024*1024) { if (freesrc && src->close) src->close(src); return NULL; }
	Uint8 *data = (Uint8 *)malloc((size_t)sz);
	if (!data) { if (freesrc && src->close) src->close(src); return NULL; }
	if (src->seek) src->seek(src, 0, RW_SEEK_SET);
	size_t got = src->read ? src->read(src, data, 1, (size_t)sz) : 0;
	if (freesrc && src->close) src->close(src);
	if (got != (size_t)sz || sz < 14 || memcmp(data, "MThd", 4) != 0) { free(data); return NULL; }
	Mix_Music *m = (Mix_Music *)calloc(1, sizeof *m);
	if (!m) { free(data); return NULL; }
	m->data = data; m->len = (Uint32)sz;
	return m;
}
Mix_Music *Mix_LoadMUS(const char *file) { return Mix_LoadMUS_RW(SDL_RWFromFile(file, "rb"), 1); }
void Mix_FreeMusic(Mix_Music *m) { if (!m) return; if (__mix_current_music==m) __mix_current_music=NULL; free(m->data); free(m); }
int Mix_PlayMusic(Mix_Music *m, int loops) {
	if (!m || !m->data || !m->len) return -1;
	if (of_midi_init() < 0) return -1;
	if (of_midi_play(m->data, m->len, loops != 0) < 0) return -1;
	m->loop=loops; m->playing=1; m->paused=0; __mix_current_music=m;
	return 0;
}
int Mix_FadeInMusic(Mix_Music *m, int loops, int ms) { (void)ms; return Mix_PlayMusic(m, loops); }
int Mix_FadeOutMusic(int ms) { (void)ms; Mix_HaltMusic(); return 1; }
void Mix_HaltMusic(void) { of_midi_stop(); if (__mix_current_music) __mix_current_music->playing=0; __mix_current_music=NULL; }
void Mix_PauseMusic(void) { of_midi_pause(); if (__mix_current_music) __mix_current_music->paused=1; }
void Mix_ResumeMusic(void) { of_midi_resume(); if (__mix_current_music) __mix_current_music->paused=0; }
int Mix_PlayingMusic(void) { return of_midi_playing(); }
int Mix_PausedMusic(void) { return of_midi_paused(); }
int Mix_VolumeMusic(int volume) { if (volume>=0){ if(volume>MIX_MAX_VOLUME)volume=MIX_MAX_VOLUME; of_midi_set_volume((volume*255)/MIX_MAX_VOLUME);} return volume; }
void Mix_HookMusicFinished(void (*cb)(void)) { (void)cb; }
int Mix_SetSoundFonts(const char *paths) { (void)paths; return 1; }
double Mix_GetMusicPosition(Mix_Music *m) { (void)m; return 0.0; }
int Mix_SetMusicPosition(double pos) { (void)pos; return 0; }

#endif /* !OF_PC */
