/*
 * of_sdl2.cpp -- implementation of the SDL2 shim (SDL.h) on openfpgaOS.
 *
 * One translation unit holds all SDL state (window surface, palette,
 * input, event queue) so DevilutionX's many TUs share it. The window
 * surface is an 8-bit indexed buffer at the size DevilutionX requests
 * (640x480); at video init the shim requests the matching 640x480 8-bit OS
 * source mode (of_video_set_mode). When that succeeds, the SDL window
 * surface aliases the OS draw buffer directly and present only calls
 * of_video_flip(); nearest-neighbor copy/scale is kept as a fallback if the
 * OS rejects the mode. FB geometry comes from the live of_video_get_mode, not
 * the boot-time of_get_caps() snapshot.
 *
 * Scope: this is the "first compile+link" implementation. Real paths are
 * provided for video, surfaces/blits, palette, timer, error/log, RWops,
 * and a basic controller->event pump; peripheral APIs (threads, audio,
 * clipboard, messagebox) are cooperative/no-op stubs to be refined.
 */
#include "SDL.h"

#include "of.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

/* Audio mixer pump (implemented in of_aulib.cpp). There is no audio thread
 * on this platform, so the SDL shim drives it from the main loop. */
extern "C" void of_aulib_pump(void);

/* Diablo platform provisioning (of_platform.c): maps DevilutionX's
 * data/config/save filenames to APF slots. */
extern "C" void of_platform_init(void);
extern "C" const char *of_platform_base_path(void);
extern "C" const char *of_platform_pref_path(void);

/* ===================================================================== */
/* Internal surface metadata (surface->map points here)                   */
/* ===================================================================== */
struct SDL_BlitMap {
	int    has_colorkey;
	Uint32 colorkey;
	int    blendmode;
	Uint8  alphamod;
	int    owns_pixels; /* free(pixels) on SDL_FreeSurface */
};

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

void SDL_LogMessageV(int cat, SDL_LogPriority pri, const char *fmt, va_list ap) {
	(void)cat; (void)pri;
	vprintf(fmt, ap);
	printf("\n");
}
static void of_log(const char *fmt, va_list ap) { vprintf(fmt, ap); printf("\n"); }
void SDL_Log(const char *fmt, ...)            { va_list a; va_start(a,fmt); of_log(fmt,a); va_end(a); }
void SDL_LogVerbose(int c,const char*f,...)   { (void)c; va_list a; va_start(a,f); of_log(f,a); va_end(a); }
void SDL_LogDebug(int c,const char*f,...)     { (void)c; va_list a; va_start(a,f); of_log(f,a); va_end(a); }
void SDL_LogInfo(int c,const char*f,...)      { (void)c; va_list a; va_start(a,f); of_log(f,a); va_end(a); }
void SDL_LogWarn(int c,const char*f,...)      { (void)c; va_list a; va_start(a,f); of_log(f,a); va_end(a); }
void SDL_LogError(int c,const char*f,...)     { (void)c; va_list a; va_start(a,f); of_log(f,a); va_end(a); }
void SDL_LogCritical(int c,const char*f,...)  { (void)c; va_list a; va_start(a,f); of_log(f,a); va_end(a); }
void SDL_LogMessage(int c,SDL_LogPriority p,const char*f,...){ (void)c;(void)p; va_list a; va_start(a,f); of_log(f,a); va_end(a); }
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
/* Version                                                                */
/* ===================================================================== */
void SDL_GetVersion(SDL_version *v) {
	/* Report SDL 2.0.10 (not 2.0.16) so HardwareCursorSupported() in
	 * devilutionx/Source/options.cpp returns false. That makes DevilutionX
	 * pick the SOFTWARE cursor render path -- the shim has no real HW
	 * cursor (SDL_ShowCursor / SDL_CreateColorCursor / SDL_SetCursor are
	 * all stubs), so with the HW-cursor default the cursor is configured
	 * but never actually drawn anywhere. With the SW path, DrawCursor
	 * blits the sprite into the back buffer like any other UI element
	 * and the cursor is visible as soon as ShouldShowCursor() is true
	 * (ControlMode == KeyboardAndMouse, inventory open, etc). */
	if (v) { v->major = 2; v->minor = 0; v->patch = 10; }
}
const char *SDL_GetRevision(void) { return "openfpga-shim"; }

/* ===================================================================== */
/* Init / Quit                                                            */
/* ===================================================================== */
static int g_video_inited;
int SDL_Init(Uint32 flags) { return SDL_InitSubSystem(flags); }
int SDL_InitSubSystem(Uint32 flags) {
	of_platform_init();
	if ((flags & SDL_INIT_VIDEO) && !g_video_inited) {
		of_video_init(); g_video_inited = 1;
		/* The OS boots at 320x240; request DevilutionX's native 640x480
		 * 8-bit indexed source mode so the window surface presents 1:1.
		 * If the OS rejects it (rc<0) we stay on the active mode and
		 * present_screen() falls back to nearest-neighbor scaling. */
		of_video_mode_t want = { 640, 480, 0, OF_VIDEO_MODE_8BIT, 0 };
		int rc = of_video_set_mode(&want);
		of_video_mode_t got; of_video_get_mode(&got);
		const struct of_capabilities *c = of_get_caps();
		printf("[of] video: req 640x480 rc=%d -> active %ux%u stride=%u; heap=%uKB sdram=%uMB\n",
		    rc, (unsigned)got.width, (unsigned)got.height, (unsigned)got.stride,
		    c ? (unsigned)(c->heap_size / 1024u) : 0u,
		    c ? (unsigned)(c->sdram_size / (1024u * 1024u)) : 0u);
	}
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
	case SDL_PIXELFORMAT_RGB888:
	case SDL_PIXELFORMAT_RGBX8888: *bpp=32; *r=0xFF0000;*g=0xFF00;*b=0xFF;*a=0; return SDL_TRUE;
	case SDL_PIXELFORMAT_ARGB8888: *bpp=32; *a=0xFF000000;*r=0xFF0000;*g=0xFF00;*b=0xFF; return SDL_TRUE;
	case SDL_PIXELFORMAT_RGBA8888: *bpp=32; *r=0xFF000000;*g=0xFF0000;*b=0xFF00;*a=0xFF; return SDL_TRUE;
	case SDL_PIXELFORMAT_ABGR8888: *bpp=32; *a=0xFF000000;*b=0xFF0000;*g=0xFF00;*r=0xFF; return SDL_TRUE;
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
	p->ncolors = ncolors; p->colors = (SDL_Color *)calloc(ncolors, sizeof(SDL_Color)); p->refcount = 1;
	return p;
}
void SDL_FreePalette(SDL_Palette *p) {
	/* Refcount-correct free: SDL2's SDL_Palette is reference-counted (refcount
	 * starts at 1 in SDL_AllocPalette, SDL_SetSurfacePalette does p->refcount++
	 * so multiple surfaces can share it). Only release when the count hits 0;
	 * otherwise the first FreeSurface destroys the palette and the next one
	 * double-frees it (musl detects the metadata corruption and traps). */
	if (!p) return;
	if (--p->refcount > 0) return;
	free(p->colors);
	free(p);
}

/* The window surface palette doubles as the hardware palette. */
static SDL_Surface *g_screen;       /* the window surface (8-bit) */
static bool g_screen_palette_is_render_palette;
/* The palette DevilutionX actually renders with: it attaches this to its
 * back buffer via SDL_SetSurfacePalette and mutates it via SDL_SetPaletteColors.
 * In surface mode the back buffer is blitted to the window surface pixels-only
 * (the blit doesn't carry the palette), so present_screen() pushes THIS to the
 * hardware palette each time it changes. */
static SDL_Palette *g_render_palette;
extern "C" void of_sdl_set_screen_palette_is_render(int enabled) {
	g_screen_palette_is_render_palette = enabled != 0;
	if (g_screen_palette_is_render_palette && g_screen
	    && g_screen->format && g_screen->format->palette) {
		g_render_palette = g_screen->format->palette;
	}
}
int SDL_SetPaletteColors(SDL_Palette *palette, const SDL_Color *colors, int first, int ncolors) {
	if (!palette) return -1;
	for (int i = 0; i < ncolors && (first+i) < palette->ncolors; i++)
		palette->colors[first+i] = colors[i];
	palette->version++;
	/* NO immediate HW push: the hardware palette is global, so pushing here
	 * recolors whatever frame is CURRENTLY on glass. SVid movies write the
	 * new scene's palette before decoding the new frame -- an immediate push
	 * showed the old frame in the new palette for the whole decode. The
	 * version bump above makes present_screen() push it at the flip that
	 * shows the matching pixels. */
	/* NOTE: do NOT track g_render_palette here. DevilutionX sub-systems like
	 * LoadPotionArt (called once per level transition from InitVirtualGamepadGFX)
	 * call SDLC_SetSurfaceAndPaletteColors on a TEMP palette during level loads;
	 * tracking that switches g_render_palette to a transient palette and leaves
	 * it stuck there if no further SetPaletteColors fire on the main Palette.
	 * Tracking happens in SDL_UpperBlit instead: the source palette of the last
	 * blit before present_screen is always the palette of the pixels actually on
	 * screen, since DevilutionX's render loop ends with PalSurface -> g_screen. */
	return 0;
}
SDL_PixelFormat *SDL_AllocFormat(Uint32 fmt) {
	SDL_PixelFormat *f = (SDL_PixelFormat *)calloc(1, sizeof *f);
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
	s->format = SDL_AllocFormat(fmt);
	s->w = w; s->h = h;
	s->pitch = pitch ? pitch : w * s->format->BytesPerPixel;
	SDL_BlitMap *m = (SDL_BlitMap *)calloc(1, sizeof *m);
	s->map = m;
	if (pixels) { s->pixels = pixels; m->owns_pixels = 0; }
	else { s->pixels = calloc(1, (size_t)s->pitch * h); m->owns_pixels = 1; }
	s->clip_rect = (SDL_Rect){0, 0, w, h};
	s->refcount = 1;
	return s;
}
SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int w, int h, int depth,
    Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask) {
	(void)flags;
	Uint32 fmt = SDL_PIXELFORMAT_ARGB8888;
	if (depth == 8) fmt = SDL_PIXELFORMAT_INDEX8;
	else if (Amask) fmt = SDL_PIXELFORMAT_ARGB8888;
	else fmt = SDL_PIXELFORMAT_RGB888;
	(void)Rmask; (void)Gmask; (void)Bmask;
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
	/* SDL2 refcounts surfaces too (new_surface sets refcount=1; consumers may
	 * increment to share). Decrement and only release when count hits 0 --
	 * mirrors the palette fix and the standard SDL2 contract. */
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
	if (s->format->palette == p) return 0; /* SDL2 semantics; also stops a
	    refcount leak from repeated attaches (SVid re-attaches its palette
	    on every mid-movie palette change). */
	if (s->format->palette) SDL_FreePalette(s->format->palette);
	s->format->palette = p; if (p) p->refcount++;
	if (s == g_screen && p && g_screen_palette_is_render_palette) {
		g_render_palette = p;
	}
	/* NOTE: only the output surface is special-cased above. Attaching a
	 * palette to an arbitrary temporary surface doesn't mean it is the
	 * palette being rendered with -- see the matching comment in
	 * SDL_SetPaletteColors. Tracking every attach captured fresh,
	 * never-populated palettes and blanked the screen. */
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
	if (r) { SDL_Rect full={0,0,s->w,s->h}; SDL_IntersectRect(r,&full,&s->clip_rect); }
	else s->clip_rect = (SDL_Rect){0,0,s->w,s->h};
	return 0;
}
void SDL_GetClipRect(SDL_Surface *s, SDL_Rect *r) { if (s&&r) *r = s->clip_rect; }

int SDL_FillRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color) {
	if (!dst) return -1;
	SDL_Rect r = rect ? *rect : (SDL_Rect){0,0,dst->w,dst->h};
	SDL_Rect clip; SDL_Rect full={0,0,dst->w,dst->h};
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

/* Generic same-bpp blit with optional colorkey + clipping.
 *
 * BRAM annotation removed for now while debugging an mcause=7 store fault
 * inside memcpy called from this path. Putting it back is one OF_FASTTEXT
 * away once the fault is sorted out (the perf win is real but the
 * relocation has more surface area than I want during a bisection). */
int SDL_UpperBlit(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect) {
	if (!src || !dst) return -1;
	/* [of] Track the palette for pixels that will be presented. In the
	 * normal back-buffer path, the final BltFast(PalSurface -> g_screen)
	 * supplies the source palette. In direct-output mode, g_screen's own
	 * palette is authoritative because raw renderer writes do not pass
	 * through SDL blits. */
	if (dst == g_screen && g_screen_palette_is_render_palette
	    && dst->format && dst->format->palette) {
		g_render_palette = dst->format->palette;
	} else if (src->format && src->format->palette) {
		g_render_palette = src->format->palette;
	}
	SDL_Rect sr = srcrect ? *srcrect : (SDL_Rect){0,0,src->w,src->h};
	int dx = dstrect ? dstrect->x : 0, dy = dstrect ? dstrect->y : 0;
	/* clip source to src bounds */
	if (sr.x < 0) { dx -= sr.x; sr.w += sr.x; sr.x = 0; }
	if (sr.y < 0) { dy -= sr.y; sr.h += sr.y; sr.y = 0; }
	if (sr.x + sr.w > src->w) sr.w = src->w - sr.x;
	if (sr.y + sr.h > src->h) sr.h = src->h - sr.y;
	/* clip dest to dst clip_rect */
	const SDL_Rect &cl = dst->clip_rect;
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
			/* mixed-format fallback via map/get (rare in DevilutionX) */
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
/* Nearest-neighbor scaled blit. */
int SDL_UpperBlitScaled(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect) {
	if (!src || !dst) return -1;
	if (dst == g_screen && g_screen_palette_is_render_palette
	    && dst->format && dst->format->palette) {
		g_render_palette = dst->format->palette;
	} else if (src->format && src->format->palette) {
		g_render_palette = src->format->palette;
	}
	SDL_Rect sr = srcrect ? *srcrect : (SDL_Rect){0,0,src->w,src->h};
	SDL_Rect dr = dstrect ? *dstrect : (SDL_Rect){0,0,dst->w,dst->h};
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
		}
	}
	return 0;
}
int SDL_SoftStretch(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, const SDL_Rect *dstrect) {
	SDL_Rect dr = dstrect ? *dstrect : (SDL_Rect){0,0,dst->w,dst->h};
	return SDL_UpperBlitScaled(src, srcrect, dst, &dr);
}
SDL_Surface *SDL_ConvertSurface(SDL_Surface *src, const SDL_PixelFormat *fmt, Uint32 flags) {
	(void)flags;
	SDL_Surface *d = new_surface(src->w, src->h, fmt->format, NULL, 0);
	if (fmt->palette && d->format->palette)
		SDL_SetPaletteColors(d->format->palette, fmt->palette->colors, 0, fmt->palette->ncolors);
	SDL_Rect r{0,0,src->w,src->h};
	SDL_UpperBlit(src, &r, d, &r);
	return d;
}
SDL_Surface *SDL_ConvertSurfaceFormat(SDL_Surface *src, Uint32 pixel_format, Uint32 flags) {
	(void)flags;
	SDL_PixelFormat tmp; fill_format(&tmp, pixel_format); tmp.palette = NULL;
	return SDL_ConvertSurface(src, &tmp, 0);
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
	if(sz==0||n==0) return 0; size_t avail=(c->hidden.mem.stop-c->hidden.mem.here)/sz; if(n>avail)n=avail;
	memcpy(p,c->hidden.mem.here,n*sz); c->hidden.mem.here+=n*sz; return n;
}
static size_t rw_mem_write(SDL_RWops *c, const void*p, size_t sz, size_t n){
	size_t avail=(c->hidden.mem.stop-c->hidden.mem.here)/sz; if(n>avail)n=avail;
	memcpy(c->hidden.mem.here,p,n*sz); c->hidden.mem.here+=n*sz; return n;
}
static int rw_mem_close(SDL_RWops *c){ free(c); return 0; }

SDL_RWops *SDL_AllocRW(void){ return (SDL_RWops*)calloc(1,sizeof(SDL_RWops)); }
void SDL_FreeRW(SDL_RWops *a){ free(a); }
SDL_RWops *SDL_RWFromFile(const char *file, const char *mode) {
	FILE *f = fopen(file, mode); if (!f) { SDL_SetError("open %s failed", file); return NULL; }
	SDL_RWops *c = SDL_AllocRW(); c->type=SDL_RWOPS_STDIO; c->hidden.stdio.fp=f;
	c->size=rw_stdio_size; c->seek=rw_stdio_seek; c->read=rw_stdio_read; c->write=rw_stdio_write; c->close=rw_stdio_close;
	return c;
}
SDL_RWops *SDL_RWFromMem(void *mem, int size) {
	SDL_RWops *c=SDL_AllocRW(); c->type=SDL_RWOPS_MEMORY;
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
struct SDL_Window { int w, h; };
static SDL_Window g_window;
/* When true, g_screen->pixels IS the of_video back buffer (one of three
 * triple-buffered SDRAM pages owned by the OS). DevilutionX's BltFast
 * therefore writes straight into the buffer that of_video_flip() will
 * present, eliminating the 300 KB memcpy that present_screen would
 * otherwise do. Requires g_screen dims to match the live video mode so
 * the surface and the buffer have identical pitch. After each flip we
 * re-bind g_screen->pixels to the new draw buffer (flip returns it). */
static bool g_screen_aliases_fb = false;

static void fb_dims(int *w, int *h) {
	/* Live mode (reflects of_video_set_mode), not the boot caps snapshot. */
	of_video_mode_t m; of_video_get_mode(&m);
	*w = m.width  ? (int)m.width  : OF_SCREEN_W;
	*h = m.height ? (int)m.height : OF_SCREEN_H;
}
static SDL_Surface *make_screen(int w, int h) {
	int fw, fh; fb_dims(&fw, &fh);
	of_video_mode_t m; of_video_get_mode(&m);
	int fstride = m.stride ? (int)m.stride : fw;
	if (w == fw && h == fh) {
		/* Alias path: hand new_surface() the current draw buffer as
		 * its pixel store. m->owns_pixels stays 0 so SDL_FreeSurface
		 * never tries to free OS-owned memory. */
		SDL_Surface *s = new_surface(w, h, SDL_PIXELFORMAT_INDEX8, of_video_surface(), fstride);
		g_screen_palette_is_render_palette = false;
		g_screen_aliases_fb = true;
		return s;
	}
	/* Mismatched dims (e.g. running 640x480 game on a 320x240 mode):
	 * keep the old private-buffer path; present_screen() will scale. */
	SDL_Surface *s = new_surface(w, h, SDL_PIXELFORMAT_INDEX8, NULL, 0);
	g_screen_palette_is_render_palette = false;
	g_screen_aliases_fb = false;
	return s;
}
SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags) {
	(void)title;(void)x;(void)y;(void)flags;
	if (!g_video_inited) { of_video_init(); g_video_inited = 1; }
	if (w <= 0) w = 640; if (h <= 0) h = 480;
	g_window.w = w; g_window.h = h;
	if (!g_screen) g_screen = make_screen(w, h);
	printf("[of] SDL_CreateWindow %dx%d -> screen %p\n", w, h, (void *)g_screen);
	return &g_window;
}
void SDL_DestroyWindow(SDL_Window *win) { (void)win; }
SDL_Surface *SDL_GetWindowSurface(SDL_Window *win) { (void)win; if (!g_screen) g_screen = make_screen(g_window.w?g_window.w:640, g_window.h?g_window.h:480); return g_screen; }
/* Used by upstream storm_svid's generic BlitFrame branch (compiled out under
 * OPENFPGAOS, which has its own 8-bit path); kept for API completeness. */
Uint32 SDL_GetWindowPixelFormat(SDL_Window *win) { (void)win; return SDL_PIXELFORMAT_INDEX8; }
SDL_Surface *SDL_GetVideoSurface(void) { return SDL_GetWindowSurface(&g_window); }

/* ---- frame-time telemetry (build with `make PERF=1`) ----
 * One serial line every 2 s: fps, average world-draw / SVid-decode ms
 * (of_perf_add_draw_us from scrollrt.cpp / storm_svid.cpp), average flip ms
 * (of_video_flip = cache clean + page swap), minimum buffered audio (aud=,
 * ~0 = ring ran dry), and injected-silence (gap=, the push-decoder zero-fill
 * that the ring level cannot see).  This instrumentation diagnosed the movie
 * pacing freeze, the palette-fade stall, and the audio-gap injection -- keep
 * it one flag away. */
#ifdef OF_PERF_TRACE
static unsigned g_perf_draw_us_acc, g_perf_draw_n;
extern int g_perf_aud_min_pairs;                    /* of_aulib.cpp */
extern "C" unsigned of_svid_zero_fill_samples;      /* push_aulib_decoder.cpp */
extern "C" void of_perf_add_draw_us(unsigned us) {
	g_perf_draw_us_acc += us;
	g_perf_draw_n++;
}
#endif

/* Push the rendered frame to the hardware. When g_screen aliases the
 * of_video back buffer (matching dims, the common Pocket case), this
 * skips a 300 KB memcpy: DevilutionX's BltFast already wrote straight
 * into the buffer of_video_flip() will present. Otherwise (mismatched
 * dims) we keep the nearest-neighbor scaling copy. */
static void present_screen(void) {
	if (!g_screen) return;
	of_aulib_pump(); /* feed audio before the frame copy/flip can stall */
	of_video_mode_t m; of_video_get_mode(&m);
	int fw = m.width  ? (int)m.width  : OF_SCREEN_W;
	int fh = m.height ? (int)m.height : OF_SCREEN_H;
	int fstride = m.stride ? (int)m.stride : fw;   /* FB bytes per row */
	const uint8_t *src = (const uint8_t *)g_screen->pixels;
	int sw = g_screen->w, sh = g_screen->h, sp = g_screen->pitch;
	static int first_present = 1;
	if (first_present) {
		first_present = 0;
		printf("[of] first present: fb=%dx%d stride=%d surf=%dx%d alias=%d\n",
		    fw, fh, fstride, sw, sh, (int)g_screen_aliases_fb);
		if (!g_screen_aliases_fb)
			printf("[of] WARNING: SDL surface is not aliased to OS triple buffer; present will copy/scale\n");
	}
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
	if (g_screen_palette_is_render_palette && g_screen->format
	    && g_screen->format->palette) {
		g_render_palette = g_screen->format->palette;
	}
	/* The 8-bit pixels just copied index DevilutionX's render palette, which
	 * the back-buffer->window blit does not carry over. Push it to the hardware
	 * palette (only when it changed) before flipping. */
	if (g_render_palette) {
		static unsigned last_pal_ver = ~0u;
		static SDL_Palette *last_pal_ptr = nullptr;
		if (g_render_palette->version != last_pal_ver || g_render_palette != last_pal_ptr) {
			last_pal_ver = g_render_palette->version;
			last_pal_ptr = g_render_palette;
			uint32_t pal[256];
			int n = g_render_palette->ncolors > 256 ? 256 : g_render_palette->ncolors;
			for (int i = 0; i < n; i++) {
				SDL_Color c = g_render_palette->colors[i];
				pal[i] = ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
			}
			/* [of] Zero-fill entries beyond the palette's own size. The
			 * loading-screen palette has nc=20 -- if we only push 20 HW
			 * entries, indices 20-255 keep WHATEVER the previous palette
			 * left them at (probably the menu's), and any pixel using
			 * those indices renders in the wrong color. Pushing a full
			 * 256 with zeros for the tail guarantees the only colors
			 * actually visible are the ones the active palette defines. */
			for (int i = n; i < 256; i++) pal[i] = 0;
			of_video_palette_bulk(pal, 256);
		}
	}
#ifndef OF_PERF_TRACE
	of_video_flip();
#else
	{
		static unsigned perf_frames, perf_flip_us;
		static unsigned perf_window_start_ms;
		static bool perf_window_started;
		const unsigned t0 = of_time_us();
		of_video_flip();
		perf_flip_us += of_time_us() - t0;
		perf_frames++;
		const unsigned now_ms = of_time_ms();
		if (!perf_window_started) {
			perf_window_started = true;
			perf_window_start_ms = now_ms;
		}
		const unsigned span = now_ms - perf_window_start_ms;
		if (span >= 2000) {
			printf("[of] perf: fps=%u.%u draw=%u.%ums flip=%u.%ums aud=%dms gap=%ums\n",
			    (perf_frames * 1000u) / span,
			    ((perf_frames * 10000u) / span) % 10u,
			    g_perf_draw_n ? g_perf_draw_us_acc / g_perf_draw_n / 1000u : 0u,
			    g_perf_draw_n ? (g_perf_draw_us_acc / g_perf_draw_n / 100u) % 10u : 0u,
			    perf_flip_us / perf_frames / 1000u,
			    (perf_flip_us / perf_frames / 100u) % 10u,
			    g_perf_aud_min_pairs >= 0 ? g_perf_aud_min_pairs / 48 : -1,
			    of_svid_zero_fill_samples / 22u);
			of_svid_zero_fill_samples = 0;
			perf_frames = 0;
			perf_flip_us = 0;
			g_perf_draw_us_acc = 0;
			g_perf_draw_n = 0;
			g_perf_aud_min_pairs = -1;
			perf_window_start_ms = now_ms;
		}
	}
#endif /* OF_PERF_TRACE */
	if (g_screen_aliases_fb) {
		/* of_video uses triple buffering: after flip, the current
		 * draw buffer is the next free page (not the one we just
		 * presented, not the one queued for vsync). Re-bind so the
		 * next frame's blits land in the right buffer. PalSurface,
		 * if it points at g_screen via RenderDirectlyToOutputSurface,
		 * picks this up implicitly (same SDL_Surface struct). The OS
		 * SDK header types of_video_flip as void, so we re-query
		 * the new draw buffer via of_video_surface(). */
		g_screen->pixels = of_video_surface();
	}
	of_aulib_pump(); /* keep the audio FIFO fed every presented frame */
}
int SDL_UpdateWindowSurface(SDL_Window *win) { (void)win; present_screen(); return 0; }
int SDL_UpdateWindowSurfaceRects(SDL_Window *win, const SDL_Rect *r, int n) { (void)win;(void)r;(void)n; present_screen(); return 0; }
void SDL_SetWindowTitle(SDL_Window *win, const char *t) { (void)win;(void)t; }
void SDL_GetWindowSize(SDL_Window *win, int *w, int *h) { if(w)*w=win?win->w:g_window.w; if(h)*h=win?win->h:g_window.h; }
void SDL_SetWindowSize(SDL_Window *win, int w, int h) { if(win){win->w=w;win->h=h;} }
Uint32 SDL_GetWindowFlags(SDL_Window *win) { (void)win; return SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN; }
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

static void mode_from_fb(SDL_DisplayMode *m){ int w,h; fb_dims(&w,&h); m->format=SDL_PIXELFORMAT_INDEX8; m->w=w; m->h=h; m->refresh_rate=60; m->driverdata=NULL; }
int SDL_GetCurrentDisplayMode(int d, SDL_DisplayMode *m){ (void)d; if(m) mode_from_fb(m); return 0; }
int SDL_GetDesktopDisplayMode(int d, SDL_DisplayMode *m){ (void)d; if(m) mode_from_fb(m); return 0; }
int SDL_GetDisplayMode(int d, int i, SDL_DisplayMode *m){ (void)d;(void)i; if(m) mode_from_fb(m); return 0; }
int SDL_GetWindowDisplayMode(SDL_Window *win, SDL_DisplayMode *m){ (void)win; if(m) mode_from_fb(m); return 0; }
int SDL_SetWindowDisplayMode(SDL_Window *win, const SDL_DisplayMode *m){ (void)win;(void)m; return 0; }
int SDL_GetDisplayBounds(int d, SDL_Rect *r){ (void)d; if(r){int w,h;fb_dims(&w,&h);*r=(SDL_Rect){0,0,w,h};} return 0; }
int SDL_GetNumVideoDisplays(void){ return 1; }
int SDL_GetNumDisplayModes(int d){ (void)d; return 1; }
int SDL_GetDisplayDPI(int d, float *ddpi, float *hdpi, float *vdpi){ (void)d; if(ddpi)*ddpi=96; if(hdpi)*hdpi=96; if(vdpi)*vdpi=96; return 0; }
const char *SDL_GetCurrentVideoDriver(void){ return "openfpga"; }

/* ===================================================================== */
/* Renderer / Texture (software, backed by the window surface)            */
/* ===================================================================== */
struct SDL_Renderer { int logical_w, logical_h; };
struct SDL_Texture  { SDL_Surface *surface; Uint32 format; int access; };
static SDL_Renderer g_renderer;
SDL_Renderer *SDL_CreateRenderer(SDL_Window *win, int idx, Uint32 flags){ (void)win;(void)idx;(void)flags; printf("[of] SDL_CreateRenderer\n"); g_renderer.logical_w=0; g_renderer.logical_h=0; return &g_renderer; }
void SDL_DestroyRenderer(SDL_Renderer *r){ (void)r; }
int SDL_GetRendererInfo(SDL_Renderer *r, SDL_RendererInfo *info){ (void)r; if(info){ memset(info,0,sizeof*info); info->name="openfpga"; info->flags=SDL_RENDERER_SOFTWARE; } return 0; }
int SDL_RenderSetLogicalSize(SDL_Renderer *r, int w, int h){ if(r){r->logical_w=w;r->logical_h=h;} return 0; }
void SDL_RenderGetLogicalSize(SDL_Renderer *r, int *w, int *h){ if(w)*w=r?r->logical_w:0; if(h)*h=r?r->logical_h:0; }
int SDL_RenderSetIntegerScale(SDL_Renderer *r, SDL_bool e){ (void)r;(void)e; return 0; }
void SDL_RenderGetScale(SDL_Renderer *r, float *sx, float *sy){ (void)r; if(sx)*sx=1.0f; if(sy)*sy=1.0f; }
void SDL_RenderGetViewport(SDL_Renderer *r, SDL_Rect *rect){ (void)r; if(rect){int w,h;fb_dims(&w,&h);*rect=(SDL_Rect){0,0,w,h};} }
int SDL_RenderSetViewport(SDL_Renderer *r, const SDL_Rect *rect){ (void)r;(void)rect; return 0; }
int SDL_SetRenderDrawColor(SDL_Renderer *r, Uint8 a,Uint8 b,Uint8 c,Uint8 d){ (void)r;(void)a;(void)b;(void)c;(void)d; return 0; }
int SDL_RenderClear(SDL_Renderer *r){ (void)r; if(g_screen) SDL_FillRect(g_screen,NULL,0); return 0; }
int SDL_RenderCopy(SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *src, const SDL_Rect *dst){
	(void)r; if(!t||!t->surface||!g_screen) return 0;
	SDL_Rect d = dst ? *dst : (SDL_Rect){0,0,g_screen->w,g_screen->h};
	return SDL_UpperBlitScaled(t->surface, src, g_screen, &d);
}
void SDL_RenderPresent(SDL_Renderer *r){ (void)r; present_screen(); }
int SDL_RenderReadPixels(SDL_Renderer *r, const SDL_Rect *rect, Uint32 fmt, void *px, int pitch){ (void)r;(void)rect;(void)fmt;(void)px;(void)pitch; return 0; }
SDL_Texture *SDL_CreateTexture(SDL_Renderer *r, Uint32 format, int access, int w, int h){
	(void)r; SDL_Texture *t=(SDL_Texture*)calloc(1,sizeof*t); t->format=format; t->access=access; t->surface=new_surface(w,h,format,NULL,0); return t;
}
SDL_Texture *SDL_CreateTextureFromSurface(SDL_Renderer *r, SDL_Surface *s){
	(void)r; SDL_Texture *t=(SDL_Texture*)calloc(1,sizeof*t); t->format=s->format->format; t->surface=SDL_ConvertSurface(s,s->format,0); return t;
}
void SDL_DestroyTexture(SDL_Texture *t){ if(t){ SDL_FreeSurface(t->surface); free(t); } }
int SDL_UpdateTexture(SDL_Texture *t, const SDL_Rect *rect, const void *px, int pitch){
	if(!t||!t->surface) return -1; SDL_Surface*s=t->surface;
	SDL_Rect r=rect?*rect:(SDL_Rect){0,0,s->w,s->h}; int bpp=s->format->BytesPerPixel;
	for(int y=0;y<r.h;y++) memcpy((Uint8*)s->pixels+(size_t)(r.y+y)*s->pitch+(size_t)r.x*bpp, (const Uint8*)px+(size_t)y*pitch, (size_t)r.w*bpp);
	return 0;
}
int SDL_LockTexture(SDL_Texture *t, const SDL_Rect *rect, void **px, int *pitch){ (void)rect; if(!t||!t->surface)return -1; if(px)*px=t->surface->pixels; if(pitch)*pitch=t->surface->pitch; return 0; }
void SDL_UnlockTexture(SDL_Texture *t){ (void)t; }
int SDL_SetTextureBlendMode(SDL_Texture *t, int m){ (void)t;(void)m; return 0; }
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
	of_aulib_pump();
	while (ms-- > 0) {
		usleep(1000);
		of_aulib_pump();
	}
}
SDL_TimerID SDL_AddTimer(Uint32 i, SDL_TimerCallback cb, void *p){ (void)i;(void)cb;(void)p; return 0; }
SDL_bool SDL_RemoveTimer(SDL_TimerID id){ (void)id; return SDL_TRUE; }

/* ===================================================================== */
/* Input: keyboard state + event pump (controller -> SDL events)          */
/* ===================================================================== */
static Uint8 g_keystate[SDL_NUM_SCANCODES];
static Uint32 g_prev_buttons;
static int g_mouse_x, g_mouse_y;
static Uint32 g_mouse_buttons;
static int g_text_input;

/* Simple ring of synthesized events. */
#define EVQ 64
static SDL_Event g_evq[EVQ]; static int g_evhead, g_evtail;
static int evq_push(const SDL_Event *e){ int n=(g_evtail+1)%EVQ; if(n==g_evhead) return 0; g_evq[g_evtail]=*e; g_evtail=n; return 1; }
static int evq_pop(SDL_Event *e){ if(g_evhead==g_evtail) return 0; *e=g_evq[g_evhead]; g_evhead=(g_evhead+1)%EVQ; return 1; }

int SDL_PushEvent(SDL_Event *e){ return evq_push(e); }
void SDL_PumpEvents(void){}

/* Map an OF button bit to an SDL game-controller button. */
static SDL_GameControllerButton of_to_cbtn(uint32_t bit) {
	switch (bit) {
	case OF_BTN_A: return SDL_CONTROLLER_BUTTON_A;
	case OF_BTN_B: return SDL_CONTROLLER_BUTTON_B;
	case OF_BTN_X: return SDL_CONTROLLER_BUTTON_X;
	case OF_BTN_Y: return SDL_CONTROLLER_BUTTON_Y;
	case OF_BTN_L1: return SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
	case OF_BTN_R1: return SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
	case OF_BTN_SELECT: return SDL_CONTROLLER_BUTTON_BACK;
	case OF_BTN_START: return SDL_CONTROLLER_BUTTON_START;
	case OF_BTN_UP: return SDL_CONTROLLER_BUTTON_DPAD_UP;
	case OF_BTN_DOWN: return SDL_CONTROLLER_BUTTON_DPAD_DOWN;
	case OF_BTN_LEFT: return SDL_CONTROLLER_BUTTON_DPAD_LEFT;
	case OF_BTN_RIGHT: return SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
	default: return SDL_CONTROLLER_BUTTON_INVALID;
	}
}
static int g_prev_axes[4];   /* last emitted LX,LY,RX,RY (deadzone-gated) */
static void poll_and_synthesize(void) {
	of_input_poll();
	of_input_state_t st; of_input_state(0, &st);
	uint32_t pressed = st.buttons & ~g_prev_buttons;
	uint32_t released = ~st.buttons & g_prev_buttons;
	for (int bit = 0; bit < 16; bit++) {
		uint32_t mask = 1u << bit;
		SDL_GameControllerButton cb = of_to_cbtn(mask);
		if (cb == SDL_CONTROLLER_BUTTON_INVALID) continue;
		if (pressed & mask) { SDL_Event e{}; e.type=SDL_CONTROLLERBUTTONDOWN; e.cbutton.button=(Uint8)cb; e.cbutton.state=SDL_PRESSED; evq_push(&e); }
		if (released & mask){ SDL_Event e{}; e.type=SDL_CONTROLLERBUTTONUP;   e.cbutton.button=(Uint8)cb; e.cbutton.state=SDL_RELEASED; evq_push(&e); }
	}
	/* Analog sticks -> controller axis events, emitted ONLY when a value
	 * changes beyond a deadzone. Emitting every poll would keep the event
	 * queue permanently non-empty, so SDL_PollEvent() would never return 0
	 * and callers that drain `while (PollEvent() != 0)` (the title screen
	 * and menus) would spin forever. */
	const int AXIS_EPS = 1024;   /* ~3% of full scale; absorbs stick jitter */
	struct { SDL_GameControllerAxis ax; int v; } axes[] = {
		{ SDL_CONTROLLER_AXIS_LEFTX, st.joy_lx }, { SDL_CONTROLLER_AXIS_LEFTY, st.joy_ly },
		{ SDL_CONTROLLER_AXIS_RIGHTX, st.joy_rx }, { SDL_CONTROLLER_AXIS_RIGHTY, st.joy_ry },
	};
	for (int i = 0; i < 4; i++) {
		int d = axes[i].v - g_prev_axes[i]; if (d < 0) d = -d;
		if (d <= AXIS_EPS) continue;
		g_prev_axes[i] = axes[i].v;
		SDL_Event e{}; e.type=SDL_CONTROLLERAXISMOTION; e.caxis.axis=(Uint8)axes[i].ax; e.caxis.value=(Sint16)axes[i].v; evq_push(&e);
	}
	g_prev_buttons = st.buttons;
}

int SDL_PollEvent(SDL_Event *event) {
	/* On the first poll, announce the virtual game controller so
	 * DevilutionX's event handler (controller.cpp:95) calls
	 * GameController::Add(0) and opens it. DevilutionX's SDL2 path does
	 * NOT enumerate joysticks at startup -- the Add(0) in display.cpp:324
	 * is USE_SDL1+__SWITCH__-only -- so without this event the
	 * controllers_ list stays empty, GameController::Get(event) returns
	 * nullptr for every CONTROLLERBUTTON, and the menus ignore input. */
	static bool announced_pad = false;
	if (!announced_pad) {
		announced_pad = true;
		SDL_Event added{}; added.type = SDL_CONTROLLERDEVICEADDED; added.cdevice.which = 0;
		evq_push(&added);
	}
	/* Keep the audio FIFO topped up at poll cadence too -- present alone
	 * is once per frame and the mixer pump is a cheap no-op when the FIFO
	 * is already full, so the worst case is harmless. */
	of_aulib_pump();
	if (g_evhead == g_evtail) poll_and_synthesize();
	if (!event) { SDL_Event tmp; return evq_pop(&tmp); }
	return evq_pop(event);
}
int SDL_WaitEvent(SDL_Event *event){ for(;;){ if(SDL_PollEvent(event)) return 1; usleep(1000); } }
int SDL_WaitEventTimeout(SDL_Event *event, int timeout){ (void)timeout; return SDL_PollEvent(event); }
int SDL_PeepEvents(SDL_Event *events, int n, SDL_eventaction action, Uint32 mn, Uint32 mx){ (void)events;(void)n;(void)action;(void)mn;(void)mx; return 0; }
Uint32 SDL_RegisterEvents(int n){ (void)n; static Uint32 next=SDL_USEREVENT; Uint32 r=next; next+=n; return r; }
SDL_bool SDL_HasEvent(Uint32 t){ (void)t; return (g_evhead!=g_evtail)?SDL_TRUE:SDL_FALSE; }
void SDL_FlushEvent(Uint32 t){ (void)t; }
void SDL_FlushEvents(Uint32 a, Uint32 b){ (void)a;(void)b; g_evhead=g_evtail=0; }
Uint8 SDL_EventState(Uint32 t, int s){ (void)t;(void)s; return 1; }
void SDL_SetEventFilter(SDL_EventFilter f, void *u){ (void)f;(void)u; }

const Uint8 *SDL_GetKeyboardState(int *numkeys){ if(numkeys)*numkeys=SDL_NUM_SCANCODES; return g_keystate; }
SDL_Keymod SDL_GetModState(void){ return KMOD_NONE; }
void SDL_SetModState(SDL_Keymod m){ (void)m; }
SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode s){ return (SDL_Keycode)s; }
SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode k){ return (SDL_Scancode)(k & ~SDLK_SCANCODE_MASK); }
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
Uint32 SDL_GetMouseState(int *x, int *y){ if(x)*x=g_mouse_x; if(y)*y=g_mouse_y; return g_mouse_buttons; }
Uint32 SDL_GetGlobalMouseState(int *x, int *y){ return SDL_GetMouseState(x,y); }
void SDL_WarpMouseInWindow(SDL_Window *win, int x, int y){ (void)win; g_mouse_x=x; g_mouse_y=y; }
void SDL_WarpMouse(Uint16 x, Uint16 y){ g_mouse_x=x; g_mouse_y=y; }
int SDL_ShowCursor(int toggle){ (void)toggle; return SDL_DISABLE; }
int SDL_CaptureMouse(SDL_bool e){ (void)e; return 0; }
SDL_Cursor *SDL_CreateCursor(const Uint8*d,const Uint8*m,int w,int h,int hx,int hy){ (void)d;(void)m;(void)w;(void)h;(void)hx;(void)hy; return (SDL_Cursor*)1; }
SDL_Cursor *SDL_CreateColorCursor(SDL_Surface *s,int hx,int hy){ (void)s;(void)hx;(void)hy; return (SDL_Cursor*)1; }
SDL_Cursor *SDL_CreateSystemCursor(SDL_SystemCursor id){ (void)id; return (SDL_Cursor*)1; }
void SDL_SetCursor(SDL_Cursor *c){ (void)c; }
SDL_Cursor *SDL_GetDefaultCursor(void){ return (SDL_Cursor*)1; }
void SDL_FreeCursor(SDL_Cursor *c){ (void)c; }

/* ===================================================================== */
/* Joystick / Game controller (single virtual pad = OF player 0)          */
/* ===================================================================== */
struct SDL_GameController { int unused; };
struct SDL_Joystick { int unused; };
static SDL_GameController g_pad; static SDL_Joystick g_joy;
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
SDL_bool SDL_GameControllerHasButton(SDL_GameController *c, SDL_GameControllerButton b){ (void)c; return of_to_cbtn(0)==b?SDL_FALSE:SDL_TRUE; }
SDL_GameControllerButtonBind SDL_GameControllerGetBindForButton(SDL_GameController *c, SDL_GameControllerButton b){ (void)c;(void)b; SDL_GameControllerButtonBind r{}; r.bindType=SDL_CONTROLLER_BINDTYPE_BUTTON; return r; }
const char *SDL_GameControllerGetStringForButton(SDL_GameControllerButton b){ (void)b; return ""; }
SDL_GameControllerButton SDL_GameControllerGetButtonFromString(const char *s){ (void)s; return SDL_CONTROLLER_BUTTON_INVALID; }
const char *SDL_GameControllerGetStringForAxis(SDL_GameControllerAxis a){ (void)a; return ""; }
SDL_GameControllerAxis SDL_GameControllerGetAxisFromString(const char *s){ (void)s; return SDL_CONTROLLER_AXIS_INVALID; }
int SDL_GameControllerAddMapping(const char *m){ (void)m; return 0; }
void SDL_GameControllerUpdate(void){}
SDL_GameControllerType SDL_GameControllerGetType(SDL_GameController *c){ (void)c; return SDL_CONTROLLER_TYPE_UNKNOWN; }
SDL_GameControllerType SDL_GameControllerTypeForIndex(int i){ (void)i; return SDL_CONTROLLER_TYPE_UNKNOWN; }
char *SDL_GameControllerMappingForGUID(SDL_JoystickGUID g){ (void)g; return NULL; }
char *SDL_GameControllerMapping(SDL_GameController *c){ (void)c; return NULL; }
SDL_Joystick *SDL_JoystickOpen(int i){ (void)i; return &g_joy; }
SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *j){ (void)j; return 0; }
const char *SDL_JoystickName(SDL_Joystick *j){ (void)j; return "Analogue Pocket"; }
const char *SDL_JoystickNameForIndex(int i){ (void)i; return "Analogue Pocket"; }
int SDL_JoystickNumButtons(SDL_Joystick *j){ (void)j; return 16; }
int SDL_JoystickNumAxes(SDL_Joystick *j){ (void)j; return 6; }
int SDL_JoystickNumHats(SDL_Joystick *j){ (void)j; return 0; }
Uint8 SDL_JoystickGetButton(SDL_Joystick *j, int b){ (void)j;(void)b; return 0; }
Sint16 SDL_JoystickGetAxis(SDL_Joystick *j, int a){ (void)j;(void)a; return 0; }
Uint8 SDL_JoystickGetHat(SDL_Joystick *j, int h){ (void)j;(void)h; return 0; }
SDL_JoystickGUID SDL_JoystickGetGUID(SDL_Joystick *j){ (void)j; SDL_JoystickGUID g{}; return g; }
void SDL_JoystickClose(SDL_Joystick *j){ (void)j; }

/* ===================================================================== */
/* Threads / mutexes / cond / sem (cooperative, single core)              */
/* ===================================================================== */
struct SDL_Thread { int done; int result; };
SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data) {
	SDL_Thread *t = (SDL_Thread *)calloc(1, sizeof *t);
	/* No preemptive threads: run the body synchronously now. If a thread
	 * body is a long-running loop, this never returns -> apparent hang. */
	printf("[of] SDL_CreateThread '%s' (running synchronously)\n", name ? name : "?");
	if (fn) t->result = fn(data);
	t->done = 1;
	return t;
}
void SDL_WaitThread(SDL_Thread *t, int *status){ if(t){ if(status)*status=t->result; free(t);} }
void SDL_DetachThread(SDL_Thread *t){ free(t); }
SDL_threadID SDL_GetThreadID(SDL_Thread *t){ return (SDL_threadID)(uintptr_t)t; }
SDL_threadID SDL_ThreadID(void){ return 1; }
const char *SDL_GetThreadName(SDL_Thread *t){ (void)t; return ""; }

struct SDL_mutex { int locked; };
SDL_mutex *SDL_CreateMutex(void){ return (SDL_mutex*)calloc(1,sizeof(SDL_mutex)); }
int SDL_LockMutex(SDL_mutex *m){ if(m)m->locked++; return 0; }
int SDL_TryLockMutex(SDL_mutex *m){ if(m)m->locked++; return 0; }
int SDL_UnlockMutex(SDL_mutex *m){ if(m&&m->locked)m->locked--; return 0; }
void SDL_DestroyMutex(SDL_mutex *m){ free(m); }
struct SDL_cond { int x; };
SDL_cond *SDL_CreateCond(void){ return (SDL_cond*)calloc(1,sizeof(SDL_cond)); }
void SDL_DestroyCond(SDL_cond *c){ free(c); }
int SDL_CondSignal(SDL_cond *c){ (void)c; return 0; }
int SDL_CondBroadcast(SDL_cond *c){ (void)c; return 0; }
int SDL_CondWait(SDL_cond *c, SDL_mutex *m){ (void)c;(void)m; return 0; }
int SDL_CondWaitTimeout(SDL_cond *c, SDL_mutex *m, Uint32 ms){ (void)c;(void)m;(void)ms; return SDL_MUTEX_TIMEDOUT; }
struct SDL_sem { Uint32 v; };
SDL_sem *SDL_CreateSemaphore(Uint32 v){ SDL_sem*s=(SDL_sem*)calloc(1,sizeof(SDL_sem)); s->v=v; return s; }
void SDL_DestroySemaphore(SDL_sem *s){ free(s); }
int SDL_SemWait(SDL_sem *s){ if(s&&s->v)s->v--; return 0; }
int SDL_SemTryWait(SDL_sem *s){ if(s&&s->v){s->v--; return 0;} return SDL_MUTEX_TIMEDOUT; }
int SDL_SemWaitTimeout(SDL_sem *s, Uint32 ms){ (void)ms; return SDL_SemTryWait(s); }
int SDL_SemPost(SDL_sem *s){ if(s)s->v++; return 0; }
Uint32 SDL_SemValue(SDL_sem *s){ return s?s->v:0; }

/* ===================================================================== */
/* Hints / clipboard / messagebox / filesystem / cpu / audio stubs        */
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
char *SDL_GetBasePath(void){ of_platform_init(); return strdup(of_platform_base_path()); }
char *SDL_GetPrefPath(const char *org, const char *app){ (void)org;(void)app; of_platform_init(); return strdup(of_platform_pref_path()); }
int SDL_GetCPUCount(void){ return 1; }
int SDL_GetSystemRAM(void){ const struct of_capabilities*c=of_get_caps(); return c?(int)(c->sdram_size/(1024*1024)):64; }
void SDL_WarpMouseGlobal(int x, int y){ g_mouse_x=x; g_mouse_y=y; }
int SDL_EnableUNICODE(int e){ (void)e; return 0; }

/* Audio (NOSOUND build path does not call these; provided for link safety) */
SDL_AudioDeviceID SDL_OpenAudioDevice(const char *d,int cap,const SDL_AudioSpec*want,SDL_AudioSpec*got,int chg){ (void)d;(void)cap;(void)chg; if(want&&got)*got=*want; return 1; }
void SDL_CloseAudioDevice(SDL_AudioDeviceID d){ (void)d; }
void SDL_PauseAudioDevice(SDL_AudioDeviceID d,int p){ (void)d;(void)p; }
void SDL_LockAudioDevice(SDL_AudioDeviceID d){ (void)d; }
void SDL_UnlockAudioDevice(SDL_AudioDeviceID d){ (void)d; }
int SDL_QueueAudio(SDL_AudioDeviceID d,const void*p,Uint32 l){ (void)d;(void)p;(void)l; return 0; }
Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID d){ (void)d; return 0; }
void SDL_ClearQueuedAudio(SDL_AudioDeviceID d){ (void)d; }
int SDL_GetNumAudioDevices(int cap){ (void)cap; return 0; }
const char *SDL_GetAudioDeviceName(int i,int cap){ (void)i;(void)cap; return "openfpga"; }
