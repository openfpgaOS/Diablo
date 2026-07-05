//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------
//
// of_texture.h -- portable GPU texture management (complete abstraction).
//
// The app deals only in opaque of_texture_t handles and a static/dynamic
// distinction.  It NEVER names a memory: which textures land in a dedicated
// fast texture chip vs SDRAM, the upload mechanism, the global fetch-routing
// switch, the colormap co-location, and the byte addressing are ALL hidden
// here.  The same source builds and runs on Pocket (dedicated fast texture
// memory present) and MiSTer (textures in SDRAM) with no #ifdef.
//
//     of_texture_init();                                  // once, after of_gpu_init()
//     of_texture_set_colormap(colormap, colormap_bytes);  // once per palette
//
//     of_texture_t wall;
//     of_texture_create(&wall, pixels, w, h, nbytes);     // static: best memory
//
//     of_texture_t sky;
//     of_texture_create_dynamic(&sky, sky_sdram, w, h);   // mutated every frame
//     ... mutate sky_sdram ...; of_texture_update(&sky, nbytes);
//
//     of_texture_bind(&wall);  of_gpu_draw_*(...);
//     of_texture_bind(&sky);   of_gpu_draw_*(...);         // domain flip handled
//
// of_texture_create() packs static textures into fast memory (if present and
// they fit) — so a frame of static textures shares one memory and never pays a
// switch.  Dynamic textures stay in SDRAM so per-frame updates are a cheap
// cache-clean, not a re-upload; of_texture_bind() flips the GPU fetch domain
// (and moves the colormap with it) only when it actually changes.
//
// STATE: like of_gpu.h, the manager is header-only static — drive textures from
// the SAME single translation unit that drives the GPU.  Not reentrant.
//------------------------------------------------------------------------------

#ifndef OF_TEXTURE_H
#define OF_TEXTURE_H

#include <stdint.h>
#include "of_gpu.h"
#include "of_caps.h"
#include "of_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* width/height are public (UV math); the rest is INTERNAL — use the
     * of_texture_* functions, never read these directly. */
    uint16_t width;
    uint16_t height;
    uint32_t _addr;     /* GPU texture base (fast byte offset, or SDRAM GPU addr) */
    const void *_src;   /* SDRAM source, for dynamic-texture of_texture_update()  */
    uint8_t  _flags;    /* bit0 = in fast memory, bit1 = dynamic */
} of_texture_t;

#define _OF_TEX_FAST    0x1u
#define _OF_TEX_DYNAMIC 0x2u

#ifndef OF_PC

/* ---- header-only manager state (one TU — see header note) ---- */
static uint32_t _of_tex_fast_top;     /* next free byte offset in fast memory  */
static uint32_t _of_tex_fast_size;    /* 0 = no fast memory (everything SDRAM)  */
static int      _of_tex_bound;        /* current fetch domain: -1/0(SDRAM)/1(fast) */
static uint32_t _of_tex_cmap_fast;    /* colormap copy in fast memory (byte off) */
static uint32_t _of_tex_cmap_sdram;   /* colormap GPU addr in SDRAM             */
static int      _of_tex_cmap_has_fast;

static inline uint32_t _of_tex_gpu_addr(const void *p) {
    const struct of_capabilities *c = of_get_caps();
    uint32_t a = (uint32_t)(uintptr_t)p;
    if (c->sdram_uncached_base && a >= c->sdram_uncached_base)
        return a - c->sdram_uncached_base;
    return a - c->sdram_base;
}

/* Call once after of_gpu_init(). */
static inline void of_texture_init(void) {
    const struct of_capabilities *c = of_get_caps();
    _of_tex_fast_size   = (c && c->version >= 3) ? c->tex_fast_size : 0u;
    _of_tex_fast_top    = 0u;
    _of_tex_bound       = -1;
    _of_tex_cmap_fast   = 0u;
    _of_tex_cmap_sdram  = 0u;
    _of_tex_cmap_has_fast = 0;
}

/* 1 if the target has a dedicated fast texture tier (else SDRAM only). */
static inline int of_texture_has_fast_mem(void) { return _of_tex_fast_size != 0u; }

/* GPU base address of a texture, for renderers that pack tex_addr directly into
 * span/column records (instead of of_texture_bind's SET_TEXTURE).  Add the
 * intra-texture byte offset (column / mip level) to it — of_texture_create
 * uploads pixels contiguously, so sub-offsets are preserved.  Memory-agnostic:
 * it's just "where the GPU finds this texture".  ALL textures whose addresses
 * share one batched draw must be in the same tier (see of_texture_in_fast_mem);
 * of_texture_create packs statics together so that holds automatically. */
static inline uint32_t of_texture_gpu_addr(const of_texture_t *t) { return t->_addr; }

/* 1 if this texture lives in the fast tier (use to group batches by domain). */
static inline int of_texture_in_fast_mem(const of_texture_t *t) {
    return (t->_flags & _OF_TEX_FAST) != 0;
}

/* Bytes of the fast tier still free (0 if none / exhausted). */
static inline uint32_t of_texture_budget_free(void) {
    return (_of_tex_fast_size > _of_tex_fast_top)
         ? (_of_tex_fast_size - _of_tex_fast_top) : 0u;
}

/* Internal: bump-allocate + upload `nbytes` of `src` into fast memory. */
static inline uint32_t _of_tex_fast_put(const void *src, uint32_t nbytes) {
    uint32_t off = _of_tex_fast_top;
    _of_gpu_fast_tex_upload(off, (const uint32_t *)src, (nbytes + 3u) >> 2);
    _of_tex_fast_top = (off + nbytes + 15u) & ~15u;   /* 16-byte align next */
    return off;
}

/* Set the active colormap (palookup).  The colormap is fetched through the same
 * GPU port as textures, so it must live wherever the textures do — this stores
 * it in BOTH fast memory (if present) and SDRAM, and of_texture_bind() points
 * the GPU at the right copy as the fetch domain flips.  `cm` must be SDRAM. */
static inline void of_texture_set_colormap(const void *cm, uint32_t nbytes) {
    _of_tex_cmap_sdram = _of_tex_gpu_addr(cm);
    if (_of_tex_fast_size && (_of_tex_fast_top + ((nbytes + 15u) & ~15u)) <= _of_tex_fast_size) {
        _of_tex_cmap_fast     = _of_tex_fast_put(cm, nbytes);
        _of_tex_cmap_has_fast = 1;
    } else {
        _of_tex_cmap_has_fast = 0;
    }
    /* Prime PALOOKUP_BASE for whatever domain is (or will be) active. */
    GPU_PALOOKUP_BASE = (_of_tex_bound == 1 && _of_tex_cmap_has_fast)
                      ? _of_tex_cmap_fast : _of_tex_cmap_sdram;
}

/* Create a STATIC texture from `pixels` (`nbytes`, w x h).  Uploaded to fast
 * memory if present and it fits; else pointed at `pixels` in SDRAM in place
 * (no copy — `pixels` must stay SDRAM-resident).  Returns 0; -1 never happens
 * while SDRAM placement is possible. */
static inline int of_texture_create(of_texture_t *tex, const void *pixels,
                                    uint16_t w, uint16_t h, uint32_t nbytes) {
    tex->width = w; tex->height = h; tex->_src = pixels;
    if (_of_tex_fast_size &&
        ((_of_tex_fast_top + ((nbytes + 15u) & ~15u)) <= _of_tex_fast_size)) {
        tex->_addr  = _of_tex_fast_put(pixels, nbytes);
        tex->_flags = _OF_TEX_FAST;
        return 0;
    }
    tex->_addr  = _of_tex_gpu_addr(pixels);
    tex->_flags = 0;
    return 0;
}

/* Create a DYNAMIC texture whose pixels change often (e.g. an animated sky).
 * It stays in SDRAM at `sdram_pixels` so updates are cheap; mutate that buffer
 * in place and call of_texture_update() before binding. */
static inline int of_texture_create_dynamic(of_texture_t *tex, void *sdram_pixels,
                                            uint16_t w, uint16_t h) {
    tex->width = w; tex->height = h; tex->_src = sdram_pixels;
    tex->_addr  = _of_tex_gpu_addr(sdram_pixels);
    tex->_flags = _OF_TEX_DYNAMIC;            /* SDRAM, not fast */
    return 0;
}

/* Make a dynamic texture's freshly-mutated `nbytes` visible to the GPU. */
static inline void of_texture_update(const of_texture_t *tex, uint32_t nbytes) {
    if (tex->_flags & _OF_TEX_DYNAMIC)
        of_cache_clean_range((void *)(uintptr_t)tex->_src, nbytes);
}

/* Reset the fast allocator + colormap (e.g. on level change).  Re-create
 * textures and re-set the colormap afterwards; old handles become stale. */
static inline void of_texture_reset(void) {
    _of_tex_fast_top = 0u;
    _of_tex_cmap_has_fast = 0;
    _of_tex_bound = -1;
}

/* Bind for subsequent draws.  Flips the GPU texture-fetch domain (and moves the
 * colormap with it) only when it changes, draining first.  of_texture_create()
 * packs statics together, so the common path is one flip per frame. */
static inline void of_texture_bind(const of_texture_t *tex) {
    int want_fast = (tex->_flags & _OF_TEX_FAST) ? 1 : 0;
    if (_of_tex_fast_size && want_fast != _of_tex_bound) {
        of_gpu_finish();                          /* drain before domain switch */
        _of_gpu_route_fast_tex(want_fast);
        GPU_PALOOKUP_BASE = (want_fast && _of_tex_cmap_has_fast)
                          ? _of_tex_cmap_fast : _of_tex_cmap_sdram;
        _of_tex_bound = want_fast;
    }
    of_gpu_texture_t gt;
    gt.addr   = tex->_addr;
    gt.width  = tex->width;
    gt.height = tex->height;
    of_gpu_bind_texture(&gt);
}

#else /* OF_PC — host shim renders via SDL; keep app code compiling. */

/* Internal state some ports' emit layers (of_emit_q2.c) read directly.  On
 * the desktop there is no fast texture memory, so fast_size stays 0 and the
 * colormap-domain routing those TUs do simply early-returns. */
static uint32_t _of_tex_fast_size;     /* 0 = no fast mem on desktop */
static int      _of_tex_bound;
static uint32_t _of_tex_cmap_fast;
static uint32_t _of_tex_cmap_sdram;
static int      _of_tex_cmap_has_fast;

static inline void     of_texture_init(void) { }
static inline int      of_texture_has_fast_mem(void) { return 0; }
static inline uint32_t of_texture_gpu_addr(const of_texture_t *t) { return t->_addr; }
static inline int      of_texture_in_fast_mem(const of_texture_t *t) { (void)t; return 0; }
static inline uint32_t of_texture_budget_free(void) { return 0u; }
static inline void     of_texture_set_colormap(const void *cm, uint32_t n) { (void)cm; (void)n; }
static inline int      of_texture_create(of_texture_t *t, const void *p,
                                         uint16_t w, uint16_t h, uint32_t n) {
    (void)p; (void)n; t->width = w; t->height = h; t->_addr = 0; t->_src = p; t->_flags = 0; return 0;
}
static inline int      of_texture_create_dynamic(of_texture_t *t, void *p, uint16_t w, uint16_t h) {
    t->width = w; t->height = h; t->_addr = 0; t->_src = p; t->_flags = _OF_TEX_DYNAMIC; return 0;
}
static inline void     of_texture_update(const of_texture_t *t, uint32_t n) { (void)t; (void)n; }
static inline void     of_texture_reset(void) { }
static inline void     of_texture_bind(const of_texture_t *t) { (void)t; }

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_TEXTURE_H */
