//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * gpudemo: canonical GPU path showcase for openfpgaOS.
 *
 * The demo sticks to the production APIs: affine span groups, perspective
 * span groups, framebuffer clear/flip/fence, palette-lookup uploads, and
 * translucency-table uploads.
 *
 * It also models the canonical GPU bring-up order: of_get_caps() → gate on
 * the OF_HW_GPU_SPAN feature bit (+ gpu_base) → of_gpu_init(). Optional
 * features are gated individually: OF_HW_GPU_SPAN is the always-present
 * baseline (AFFINE / MASK / CMAP modes); the BLEND mode needs OF_HW_GPU_ALPHA
 * and the PERSP mode needs OF_HW_GPU_PERSP, so those modes only appear in the
 * cycle when the bit is set. On a core with no usable GPU the demo degrades
 * to a terminal notice. (CLAUDE.md also suggests a draw-and-read-back probe;
 * we don't, because reading the framebuffer back on the CPU assumes it lives
 * in the cached-SDRAM alias, which isn't guaranteed — the feature bit is.)
 *
 * Modes (A cycles): AFFINE, MASK, CMAP, BLEND (needs ALPHA), PERSP, and
 * MAZE — a Descent-lite fly-through whose walls/floor/ceiling are textured
 * quads split into perspective-correct triangles (long-form 0x48 persp span
 * groups, which decode on every variant): CPU does view transform, near
 * clip and edge walk; GPU does all pixels with palookup distance lighting;
 * painter's sort stands in for a Z buffer.
 *
 * Controls:
 *   A   next mode      B   toggle FPS/CPU/GPU stats overlay
 *   MAZE mode: d-pad LEFT/RIGHT turn, UP/DOWN fly
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <of.h>
#include <of_cache.h>
#include <of_gpu.h>

#define SCREEN_W 320
#define SCREEN_H 240

#define TEX_W 64
#define TEX_H 64
#define SPRITE_W 16
#define SPRITE_H 16

#define CMAP_ROWS 64
#define CMAP_SLOTS 2
#define MODE_COUNT 6
#define OVERLAY_H 18

static uint8_t *checker_tex;
static uint8_t *wall_tex;
static uint8_t *sprite_tex;
static uint8_t *persp_tex;
static uint8_t *blend_tex;

static uint8_t colormap[CMAP_SLOTS][CMAP_ROWS * 256];
static uint32_t pal_rgb[256];
static uint8_t translucency[256 * 256];
static int16_t sin_lut[256];
static int16_t cos_lut[256];

static unsigned stat_cpu_us;
static unsigned stat_gpu_us;
static unsigned stat_frames;
static unsigned last_fps_x10;
static unsigned last_cpu_pct;
static unsigned last_gpu_pct;
static int show_stats;

/* Optional GPU features, resolved once at startup via of_has_feature().
 * Modes whose feature bit is clear are dropped from the cycle. */
static unsigned scr_w, scr_h;     /* active video mode, shown in the overlay */
static int has_span_group;        /* OF_HW_GPU_SPAN_GROUP → AFFINE/MASK/CMAP/BLEND */
static int has_transluc;          /* OF_HW_GPU_ALPHA → BLEND mode */
static int has_persp;             /* OF_HW_GPU_PERSP → PERSP mode */
static int avail_modes[MODE_COUNT];
static int avail_count;
static int gpu_wedged;            /* set if a GPU fence never retires */

static const char *const mode_names[MODE_COUNT] = {
    "AFFINE",
    "MASK",
    "CMAP",
    "BLEND",
    "PERSP",
    "MAZE",
};

static inline uint32_t rgb(int r, int g, int b)
{
    return ((uint32_t)(r & 255) << 16) | ((uint32_t)(g & 255) << 8) | (uint32_t)(b & 255);
}

static inline int clamp_int(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static void build_luts(void)
{
    for (int i = 0; i < 256; ++i) {
        float a = (float)i * 6.28318530718f / 256.0f;
        sin_lut[i] = (int16_t)(sinf(a) * 32767.0f);
        cos_lut[i] = (int16_t)(cosf(a) * 32767.0f);
    }
}

static void build_palette(void)
{
    for (int i = 0; i < 256; ++i)
        pal_rgb[i] = rgb(0, 0, 0);

    pal_rgb[0] = rgb(0, 0, 0);
    pal_rgb[0x10] = rgb(18, 20, 28);

    for (int i = 0; i < 32; ++i) {
        pal_rgb[0x20 + i] = rgb(14 + i * 3, 20 + i * 4, 46 + i * 5);
        pal_rgb[0x40 + i] = rgb(32 + i * 5, 28 + i * 3, 24 + i * 2);
        pal_rgb[0x60 + i] = rgb(18 + i * 2, 54 + i * 4, 24 + i * 2);
        pal_rgb[0x80 + i] = rgb(88 + i * 4, 56 + i * 2, 28 + i);
        pal_rgb[0xa0 + i] = rgb(42 + i * 2, 36 + i * 3, 74 + i * 4);
        pal_rgb[0xc0 + i] = rgb(24 + i * 4, 70 + i * 3, 90 + i * 3);
        pal_rgb[0xe0 + i] = rgb(96 + i * 5, 66 + i * 4, 120 + i * 3);
    }

    for (int i = 0; i < 256; ++i)
        of_video_palette(i, pal_rgb[i]);
}

static uint8_t shade_same_band(uint8_t c, int light)
{
    if (c < 0x20)
        return c;

    int base = c & 0xe0;
    int level = c & 31;
    int lit = (level * (63 - light)) / 63;
    return (uint8_t)(base | clamp_int(lit, 0, 31));
}

static uint8_t shade_cool_band(uint8_t c, int light)
{
    if (c < 0x20)
        return c;

    int level = c & 31;
    int lit = (level * (63 - light)) / 63;
    if (light > 42)
        return (uint8_t)(0x20 | clamp_int(lit, 0, 31));
    return (uint8_t)(0xc0 | clamp_int(lit, 0, 31));
}

static void build_colormaps(void)
{
    for (int row = 0; row < CMAP_ROWS; ++row) {
        for (int c = 0; c < 256; ++c) {
            colormap[0][row * 256 + c] = shade_same_band((uint8_t)c, row);
            colormap[1][row * 256 + c] = shade_cool_band((uint8_t)c, row);
        }
    }
}

static uint8_t blend_index(uint8_t a, uint8_t b)
{
    uint32_t ca = pal_rgb[a];
    uint32_t cb = pal_rgb[b];
    int r = (int)(((ca >> 16) & 255) * 3 + ((cb >> 16) & 255) * 5) >> 3;
    int g = (int)(((ca >> 8) & 255) * 3 + ((cb >> 8) & 255) * 5) >> 3;
    int bl = (int)((ca & 255) * 3 + (cb & 255) * 5) >> 3;

    int best = 0;
    int best_d = INT32_MAX;
    for (int i = 0; i < 256; ++i) {
        int pr = (int)((pal_rgb[i] >> 16) & 255);
        int pg = (int)((pal_rgb[i] >> 8) & 255);
        int pb = (int)(pal_rgb[i] & 255);
        int dr = pr - r;
        int dg = pg - g;
        int db = pb - bl;
        int d = dr * dr + dg * dg + db * db;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return (uint8_t)best;
}

static void build_translucency(void)
{
    for (int dst = 0; dst < 256; ++dst) {
        for (int src = 0; src < 256; ++src)
            translucency[(dst << 8) | src] = blend_index((uint8_t)dst, (uint8_t)src);
    }
}

static void build_textures(void)
{
    for (int y = 0; y < TEX_H; ++y) {
        for (int x = 0; x < TEX_W; ++x) {
            int check = ((x >> 3) ^ (y >> 3)) & 1;
            int stripe = ((x + y) >> 4) & 1;
            checker_tex[y * TEX_W + x] = (uint8_t)(0x60 + ((check ? x : y) & 31));
            wall_tex[y * TEX_W + x] = (uint8_t)(0x80 + ((x * 3 + y * 5 + stripe * 12) & 31));
            persp_tex[y * TEX_W + x] = (uint8_t)(0xa0 + ((x ^ (y * 2) ^ (check ? 18 : 0)) & 31));
        }
    }

    for (int y = 0; y < SPRITE_H; ++y) {
        for (int x = 0; x < SPRITE_W; ++x) {
            int dx = x * 2 - (SPRITE_W - 1);
            int dy = y * 2 - (SPRITE_H - 1);
            int d2 = dx * dx + dy * dy;
            if (d2 > 220) {
                sprite_tex[y * SPRITE_W + x] = 0;
            } else if (d2 > 130) {
                sprite_tex[y * SPRITE_W + x] = (uint8_t)(0xe0 + ((x + y) & 15));
            } else {
                sprite_tex[y * SPRITE_W + x] = (uint8_t)(0x40 + ((x * 2 + y * 3) & 31));
            }
        }
    }

    blend_tex[0] = 0xe8;
}

static void flush_textures(void)
{
    /* The GPU's texture-fetch master reads these from SDRAM directly, so they
     * must be written all the way back — of_cache_clean_range (cbo.clean)
     * leaves dirty lines in L1 and the GPU reads stale/uncommitted bytes,
     * which on this no-HW-coherency target can stall the fetch.  Use
     * of_cache_flush_range (cbo.flush), like the game ports' GPU glue. */
    of_cache_flush_range(checker_tex, TEX_W * TEX_H);
    of_cache_flush_range(wall_tex, TEX_W * TEX_H);
    of_cache_flush_range(sprite_tex, SPRITE_W * SPRITE_H);
    of_cache_flush_range(persp_tex, TEX_W * TEX_H);
    of_cache_flush_range(blend_tex, 1);
}

static int alloc_assets(void)
{
    checker_tex = malloc(TEX_W * TEX_H);
    wall_tex = malloc(TEX_W * TEX_H);
    sprite_tex = malloc(SPRITE_W * SPRITE_H);
    persp_tex = malloc(TEX_W * TEX_H);
    blend_tex = malloc(1);

    return checker_tex && wall_tex && sprite_tex && persp_tex && blend_tex;
}

static void upload_tables(void)
{
    of_gpu_palookup_upload(0, colormap[0], sizeof(colormap[0]));
    of_gpu_palookup_upload(1, colormap[1], sizeof(colormap[1]));
    of_gpu_translucency_upload(translucency, sizeof(translucency));
}

static void set_framebuffer(int draw_idx)
{
    uint8_t *fb = of_video_buffer_addr(draw_idx);
    of_gpu_set_framebuffer((uint32_t)(uintptr_t)fb, SCREEN_W);
}

static volatile uint8_t *current_fb(int draw_idx)
{
    return of_video_buffer_addr(draw_idx);
}

static void clear_frame(uint32_t fb_addr, uint8_t color)
{
    of_gpu_clear_rect_strided(fb_addr, SCREEN_W, SCREEN_H, SCREEN_W, color);
}

static void emit_rows(uint32_t fb_addr,
                      int x,
                      int y,
                      int w,
                      int h,
                      const uint8_t *tex,
                      int tex_w,
                      int tex_w_mask,
                      int tex_h_mask,
                      uint8_t flags,
                      uint8_t colormap_id,
                      int32_t s_base,
                      int32_t t_base,
                      int32_t sstep,
                      int32_t row_t_step,
                      uint8_t light_base,
                      int light_step)
{
    if (x < 0) {
        int dx = -x;
        s_base += sstep * dx;
        w -= dx;
        x = 0;
    }
    if (x + w > SCREEN_W)
        w = SCREEN_W - x;
    if (y < 0) {
        int dy = -y;
        t_base += row_t_step * dy;
        light_base = (uint8_t)clamp_int((int)light_base + light_step * dy, 0, 63);
        h -= dy;
        y = 0;
    }
    if (y + h > SCREEN_H)
        h = SCREEN_H - y;
    if (w <= 0 || h <= 0)
        return;

    for (int row = 0; row < h; row += 4) {
        int lanes = h - row;
        if (lanes > 4)
            lanes = 4;

        of_gpu_affine_span_group_t g;
        memset(&g, 0, sizeof(g));
        g.lane_count = (uint8_t)lanes;
        g.flags = flags;
        g.tex_width = (uint16_t)tex_w;
        g.tex_w_mask = (uint16_t)tex_w_mask;
        g.tex_h_mask = (uint16_t)tex_h_mask;
        g.fb_step = 1;

        for (int lane = 0; lane < lanes; ++lane) {
            int yy = y + row + lane;
            g.fb_addr[lane] = fb_addr + (uint32_t)((yy * SCREEN_W + x) & 0x7fffffff);
            g.tex_addr[lane] = (uint32_t)(uintptr_t)tex;
            g.count[lane] = (uint16_t)w;
            g.s[lane] = s_base;
            g.t[lane] = t_base + row_t_step * (row + lane);
            g.sstep[lane] = sstep;
            g.tstep[lane] = 0;
            g.light[lane] = (uint8_t)clamp_int((int)light_base + light_step * (row + lane), 0, 63);
            g.colormap_id[lane] = colormap_id;
        }

        of_gpu_draw_affine_span_group(&g);
    }
}

static void emit_columns(uint32_t fb_addr,
                         int x,
                         int y,
                         int w,
                         int h,
                         const uint8_t *tex,
                         int tex_w,
                         int tex_w_mask,
                         int tex_h_mask,
                         uint8_t flags,
                         uint8_t colormap_id,
                         uint8_t light)
{
    if (y < 0)
        h += y, y = 0;
    if (y + h > SCREEN_H)
        h = SCREEN_H - y;
    if (x < 0)
        w += x, x = 0;
    if (x + w > SCREEN_W)
        w = SCREEN_W - x;
    if (w <= 0 || h <= 0)
        return;

    int32_t tstep = (int32_t)((tex_h_mask + 1) << 16) / h;
    for (int col = 0; col < w; col += 4) {
        int lanes = w - col;
        if (lanes > 4)
            lanes = 4;

        of_gpu_affine_span_group_t g;
        memset(&g, 0, sizeof(g));
        g.lane_count = (uint8_t)lanes;
        g.flags = flags;
        g.tex_width = (uint16_t)tex_w;
        g.tex_w_mask = (uint16_t)tex_w_mask;
        g.tex_h_mask = (uint16_t)tex_h_mask;
        g.fb_step = SCREEN_W;

        for (int lane = 0; lane < lanes; ++lane) {
            int xx = x + col + lane;
            g.fb_addr[lane] = fb_addr + (uint32_t)((y * SCREEN_W + xx) & 0x7fffffff);
            g.tex_addr[lane] = (uint32_t)(uintptr_t)tex;
            g.count[lane] = (uint16_t)h;
            g.s[lane] = (int32_t)(((int64_t)(col + lane) * (tex_w << 16)) / w);
            g.t[lane] = 0;
            g.sstep[lane] = 0;
            g.tstep[lane] = tstep;
            g.light[lane] = light;
            g.colormap_id[lane] = colormap_id;
        }

        of_gpu_draw_affine_span_group(&g);
    }
}

static void emit_affine_scene(uint32_t fb_addr, unsigned frame)
{
    clear_frame(fb_addr, 0x10);

    for (int y = 0; y < 96; y += 4) {
        int32_t drift = (int32_t)((frame * 256 + y * 384) & 0x003fffff);
        emit_rows(fb_addr, 0, y, SCREEN_W, 4, persp_tex, TEX_W, TEX_W - 1, TEX_H - 1,
                  OF_GPU_SPAN_COLORMAP, 0, drift, y << 16, 0x18000, 0x14000, 20, 0);
    }
    /* Kick the batch out in chunks rather than submitting a whole frame at
     * once — a frame-sized single submit intermittently wedges this GPU's
     * fence, while the game ports stay reliable by flushing as they go. */
    of_gpu_kick();

    for (int y = 96; y < SCREEN_H; y += 4) {
        int row = y - 96;
        int32_t stretch = 0x9000 + row * 640;
        int32_t drift = (int32_t)((frame * 320 + row * 512) & 0x003fffff);
        emit_rows(fb_addr, 0, y, SCREEN_W, 4, checker_tex, TEX_W, TEX_W - 1, TEX_H - 1,
                  OF_GPU_SPAN_COLORMAP, 0, drift, row << 15, stretch, 0x7000, 18, 0);
    }
    of_gpu_kick();

    emit_columns(fb_addr, 28, 68, 52, 128, wall_tex, TEX_W, TEX_W - 1, TEX_H - 1,
                 OF_GPU_SPAN_COLORMAP, 0, 18);
    emit_columns(fb_addr, 118, 44, 76, 166, wall_tex, TEX_W, TEX_W - 1, TEX_H - 1,
                 OF_GPU_SPAN_COLORMAP, 0, 6);
    emit_columns(fb_addr, 238, 78, 48, 116, wall_tex, TEX_W, TEX_W - 1, TEX_H - 1,
                 OF_GPU_SPAN_COLORMAP, 0, 28);
    of_gpu_kick();
}

static void emit_sprite_post(uint32_t fb_addr, int cx, int base_y, int scale)
{
    int w = SPRITE_W * scale;
    int h = SPRITE_H * scale;
    int x = cx - w / 2;
    int y = base_y - h;

    if (scale <= 0 || x >= SCREEN_W || y >= SCREEN_H || x + w <= 0 || y + h <= 0)
        return;

    if (y < 0)
        h += y, y = 0;
    if (y + h > SCREEN_H)
        h = SCREEN_H - y;

    int first_col = 0;
    if (x < 0) {
        first_col = -x;
        w += x;
        x = 0;
    }
    if (x + w > SCREEN_W)
        w = SCREEN_W - x;
    if (w <= 0 || h <= 0)
        return;

    int32_t tstep = (SPRITE_H << 16) / h;
    for (int col = 0; col < w; col += 4) {
        int lanes = w - col;
        if (lanes > 4)
            lanes = 4;

        of_gpu_affine_span_group_t g;
        memset(&g, 0, sizeof(g));
        g.lane_count = (uint8_t)lanes;
        g.flags = OF_GPU_SPAN_SKIP_ZERO;
        g.tex_width = SPRITE_W;
        g.tex_w_mask = SPRITE_W - 1;
        g.tex_h_mask = SPRITE_H - 1;
        g.fb_step = SCREEN_W;

        for (int lane = 0; lane < lanes; ++lane) {
            int dst_col = first_col + col + lane;
            int src_col = dst_col / scale;
            int xx = x + col + lane;
            g.fb_addr[lane] = fb_addr + (uint32_t)((y * SCREEN_W + xx) & 0x7fffffff);
            g.tex_addr[lane] = (uint32_t)(uintptr_t)sprite_tex;
            g.count[lane] = (uint16_t)h;
            g.s[lane] = src_col << 16;
            g.t[lane] = 0;
            g.sstep[lane] = 0;
            g.tstep[lane] = tstep;
            g.light[lane] = 0;
            g.colormap_id[lane] = 0;
        }

        of_gpu_draw_affine_span_group(&g);
    }
}

static void emit_colormap_panel(uint32_t fb_addr, int x, int y, int w, int h, uint8_t colormap_id)
{
    for (int row = 0; row < h; row += 4) {
        int lanes = h - row;
        if (lanes > 4)
            lanes = 4;

        of_gpu_affine_span_group_t g;
        memset(&g, 0, sizeof(g));
        g.lane_count = (uint8_t)lanes;
        g.flags = OF_GPU_SPAN_COLORMAP;
        g.tex_width = TEX_W;
        g.tex_w_mask = TEX_W - 1;
        g.tex_h_mask = TEX_H - 1;
        g.fb_step = 1;

        for (int lane = 0; lane < lanes; ++lane) {
            int yy = y + row + lane;
            int light = ((row + lane) * 63) / (h > 1 ? h - 1 : 1);
            g.fb_addr[lane] = fb_addr + (uint32_t)((yy * SCREEN_W + x) & 0x7fffffff);
            g.tex_addr[lane] = (uint32_t)(uintptr_t)wall_tex;
            g.count[lane] = (uint16_t)w;
            g.s[lane] = 0;
            g.t[lane] = (row + lane) << 16;
            g.sstep[lane] = 0x18000;
            g.tstep[lane] = 0;
            g.light[lane] = (uint8_t)light;
            g.colormap_id[lane] = colormap_id;
        }

        of_gpu_draw_affine_span_group(&g);
    }
}

static void emit_blend_rect(uint32_t fb_addr, int x, int y, int w, int h)
{
    emit_rows(fb_addr, x, y, w, h, blend_tex, 1, 0, 0, OF_GPU_SPAN_TRANSLUC, 0,
              0, 0, 0, 0, 0, 0);
}

typedef struct {
    float x;
    float y;
    float z;
    int32_t sdivz;
    int32_t tdivz;
    int32_t zi;
} persp_vert_t;

/* IMPORTANT: everything in the per-triangle solve below is SINGLE precision.
 * This CPU is rv32imafc — the F extension only; doubles are soft-float
 * library calls costing hundreds of cycles each, and the maze runs this
 * solve for a few hundred triangles per frame.  Float's 24-bit mantissa is
 * ample for 64-texel textures (relative error ~6e-8 on values ≤ 2e9), and
 * it is what Quake's gradient setup uses too. */

static inline int32_t clamp_float_to_i32(float v)
{
    if (v <= -2147483648.0f)
        return INT32_MIN;
    if (v >= 2147483520.0f)         /* largest float below 2^31 */
        return INT32_MAX;
    return (int32_t)v;
}

static int32_t fdiv16(int32_t a, float b)
{
    if (b == 0.0f)
        return 0;
    return clamp_float_to_i32((float)a * 65536.0f / b);
}

static float gradient_x_f(const persp_vert_t *v, float denom, int32_t a0, int32_t a1, int32_t a2)
{
    float num = (float)a0 * (v[1].y - v[2].y) +
                (float)a1 * (v[2].y - v[0].y) +
                (float)a2 * (v[0].y - v[1].y);
    return num / denom;
}

static float gradient_y_f(const persp_vert_t *v, float denom, int32_t a0, int32_t a1, int32_t a2)
{
    float num = (float)a0 * (v[2].x - v[1].x) +
                (float)a1 * (v[0].x - v[2].x) +
                (float)a2 * (v[1].x - v[0].x);
    return num / denom;
}

/* The GPU's plane arithmetic is two's-complement and therefore MODULAR: the
 * header base (plane value at screen x=0) and per-row/per-pixel steps may
 * wrap mod 2^32 freely, as long as the value at every pixel the GPU actually
 * evaluates is a true int32 — wrapped accumulation then recovers it exactly.
 * Quake's own rebase helpers (gpu_i32_sub_mul in vid_of.c) rely on the same
 * property.  Inside the triangle the plane value is a barycentric blend of
 * the three vertex values, so it always fits; only the 1-2 px of edge slop
 * from the floor/ceil span widening can overshoot, which a bound on the
 * GRADIENTS (not the base) protects against.  So: wrap the base, bound the
 * gradients — the triangle stays drawable right down to a sliver. */
static inline int32_t wrap_float_to_i32(float v)
{
    return (int32_t)(uint32_t)(int64_t)v;   /* truncate mod 2^32 */
}

static inline int32_t mad_wrap_i32(int32_t base, int32_t step, int k)
{
    return (int32_t)((uint32_t)base + (uint32_t)step * (uint32_t)k);
}

/* Compute all three texture planes (s/z, t/z, 1/z) for a screen triangle.
 * Returns 0 (skip) only when the gradients are too steep for the edge-slop
 * overshoot to stay representable, or the numbers are not finite. */
static int persp_planes_solve(const persp_vert_t v[3], float denom,
                              int y0, int y1,
                              int32_t *sx, int32_t *sy, int32_t *s0,
                              int32_t *tx, int32_t *ty, int32_t *t0,
                              int32_t *zx, int32_t *zy, int32_t *z0)
{
    (void)y0; (void)y1;
    const float grad_lim = 5.0e8f;   /* vertex max ~4.6e8 + 2px slop < 2^31 */
    const float base_lim = 1.0e15f;  /* int64-conversion safety only */

    float sxd = gradient_x_f(v, denom, v[0].sdivz, v[1].sdivz, v[2].sdivz);
    float syd = gradient_y_f(v, denom, v[0].sdivz, v[1].sdivz, v[2].sdivz);
    float txd = gradient_x_f(v, denom, v[0].tdivz, v[1].tdivz, v[2].tdivz);
    float tyd = gradient_y_f(v, denom, v[0].tdivz, v[1].tdivz, v[2].tdivz);
    float zxd = gradient_x_f(v, denom, v[0].zi, v[1].zi, v[2].zi);
    float zyd = gradient_y_f(v, denom, v[0].zi, v[1].zi, v[2].zi);

    if (fabsf(sxd) > grad_lim || fabsf(syd) > grad_lim ||
        fabsf(txd) > grad_lim || fabsf(tyd) > grad_lim ||
        fabsf(zxd) > grad_lim || fabsf(zyd) > grad_lim)
        return 0;

    float s0d = (float)v[0].sdivz - sxd * v[0].x - syd * v[0].y;
    float t0d = (float)v[0].tdivz - txd * v[0].x - tyd * v[0].y;
    float z0d = (float)v[0].zi - zxd * v[0].x - zyd * v[0].y;

    if (fabsf(s0d) > base_lim || fabsf(t0d) > base_lim || fabsf(z0d) > base_lim)
        return 0;

    *sx = wrap_float_to_i32(sxd); *sy = wrap_float_to_i32(syd);
    *s0 = wrap_float_to_i32(s0d);
    *tx = wrap_float_to_i32(txd); *ty = wrap_float_to_i32(tyd);
    *t0 = wrap_float_to_i32(t0d);
    *zx = wrap_float_to_i32(zxd); *zy = wrap_float_to_i32(zyd);
    *z0 = wrap_float_to_i32(z0d);
    return 1;
}

static int edge_span_at_y(const persp_vert_t *v, int y, int *x0, int *x1)
{
    float scan_y = (float)y + 0.5f;
    float xs[3];
    int count = 0;

    for (int i = 0; i < 3; ++i) {
        const persp_vert_t *a = &v[i];
        const persp_vert_t *b = &v[(i + 1) % 3];
        float ay = a->y;
        float by = b->y;
        if (ay == by)
            continue;
        float min_y = ay < by ? ay : by;
        float max_y = ay > by ? ay : by;
        if (scan_y < min_y || scan_y >= max_y)
            continue;
        float t = (scan_y - ay) / (by - ay);
        xs[count++] = a->x + (b->x - a->x) * t;
        if (count == 3)
            break;
    }

    if (count < 2)
        return 0;

    float left = xs[0] < xs[1] ? xs[0] : xs[1];
    float right = xs[0] > xs[1] ? xs[0] : xs[1];
    int lx = (int)floorf(left);
    int rx = (int)ceilf(right);
    if (rx < 0 || lx >= SCREEN_W)
        return 0;
    if (lx < 0)
        lx = 0;
    if (rx >= SCREEN_W)
        rx = SCREEN_W - 1;
    if (rx < lx)
        return 0;

    *x0 = lx;
    *x1 = rx;
    return 1;
}

static int emit_perspective_wedge(uint32_t fb_addr, unsigned frame)
{
    float ang = (float)(frame & 255) * 6.28318530718f / 256.0f;
    float ca = cosf(ang);
    float sa = sinf(ang);
    /* Vertex positions relative to a pivot at (0, 0, 2.45), rotated RIGIDLY
     * about the vertical axis.  (The previous transform was a non-uniform
     * shear — a fake wobble that deformed the triangle itself, so the
     * correctly-mapped texture appeared to warp with it.)  A rigid orbit
     * swings vertices as close as z ≈ 1.1, so the fixed-point z-unit scale
     * is raised 128 → 512 to keep sdivz/tdivz inside int32 there; the scale
     * cancels in the GPU's sdivz/zi divide. */
    const float src[3][3] = {
        {-1.25f, -0.85f,  0.25f},
        { 1.25f, -0.70f,  0.00f},
        {-0.20f,  1.05f, -0.45f},
    };
    const int tex_s[3] = {2, 61, 30};
    const int tex_t[3] = {4, 10, 60};

    persp_vert_t v[3];
    for (int i = 0; i < 3; ++i) {
        float x =  src[i][0] * ca + src[i][2] * sa;
        float z = 2.45f - src[i][0] * sa + src[i][2] * ca;
        float y = src[i][1];
        float inv = 86.0f / z;
        float z_units = z * 512.0f;
        v[i].x = 160.0f + x * inv;
        v[i].y = 122.0f + y * inv;
        v[i].z = z;
        v[i].sdivz = fdiv16(tex_s[i] << 16, z_units);
        v[i].tdivz = fdiv16(tex_t[i] << 16, z_units);
        v[i].zi = fdiv16(1 << 16, z_units);
    }

    float denom = (v[1].x - v[0].x) * (v[2].y - v[0].y) -
                  (v[2].x - v[0].x) * (v[1].y - v[0].y);
    if (denom > -8.0f && denom < 8.0f)
        return 0;

    int min_y = SCREEN_H - 1;
    int max_y = 0;
    for (int i = 0; i < 3; ++i) {
        if ((int)v[i].y < min_y)
            min_y = (int)floorf(v[i].y);
        if ((int)v[i].y > max_y)
            max_y = (int)ceilf(v[i].y);
    }
    min_y = clamp_int(min_y, 0, SCREEN_H - 1);
    max_y = clamp_int(max_y, 0, SCREEN_H - 1);
    if (max_y < min_y)
        return 0;

    int32_t sx, sy, s0, tx, ty, t0, zx, zy, z0;
    if (!persp_planes_solve(v, denom, min_y, max_y,
                            &sx, &sy, &s0, &tx, &ty, &t0, &zx, &zy, &z0))
        return 0;       /* near-edge-on: plane would saturate int32 — skip */

    of_gpu_persp_span_group_t groups[32];
    int group_count = 0;

    for (int y = min_y; y <= max_y && group_count < (int)(sizeof(groups) / sizeof(groups[0])); y += 8) {
        int lanes = max_y - y + 1;
        if (lanes > 8)
            lanes = 8;

        of_gpu_persp_span_group_t g;
        memset(&g, 0, sizeof(g));
        g.fb_addr = fb_addr + (uint32_t)(y * SCREEN_W);
        g.tex_addr = (uint32_t)(uintptr_t)persp_tex;
        g.lane_count = (uint8_t)lanes;
        g.flags = OF_GPU_SPAN_COLORMAP;
        g.colormap_id = 0;
        g.major_fb_step = SCREEN_W;
        g.minor_fb_step = 1;
        g.tex_width = TEX_W;
        g.tex_w_mask = TEX_W - 1;
        g.tex_h_mask = TEX_H - 1;
        g.sdivz = mad_wrap_i32(s0, sy, y);      /* wrapping: see solver note */
        g.tdivz = mad_wrap_i32(t0, ty, y);
        g.zi_persp = mad_wrap_i32(z0, zy, y);
        g.sdivz_major_step = sy;
        g.tdivz_major_step = ty;
        g.zi_major_step = zy;
        g.sdivz_minor_step = sx;
        g.tdivz_minor_step = tx;
        g.zi_minor_step = zx;
        g.light = 8;
        g.light_major_step = 1;
        g.light_minor_step = 0;

        int any = 0;
        for (int lane = 0; lane < lanes; ++lane) {
            int left, right;
            if (edge_span_at_y(v, y + lane, &left, &right)) {
                g.start[lane] = (int16_t)left;
                g.count[lane] = (uint16_t)(right - left + 1);
                any = 1;
            }
        }

        if (any)
            groups[group_count++] = g;
    }

    if (group_count > 0)
        of_gpu_draw_persp_span_group_batch(groups, group_count);
    return group_count;
}

/* ================================================================
 * MAZE mode — Descent-lite fly-through.
 *
 * A grid maze whose walls, floors and ceilings are textured quads, each
 * split into two TRIANGLES and rasterized with perspective-correct span
 * groups (the long-form 0x48, which decodes on every core variant).
 * CPU does view transform + near-plane clip + edge walk; the GPU does
 * every pixel: perspective texture stepping + palookup lighting, with a
 * per-surface light that fades with distance for the depth cue.
 * Painter's order (far → near quad sort) stands in for a Z buffer.
 *
 * Controls (while in MAZE mode): d-pad LEFT/RIGHT turn, UP/DOWN fly.
 * ================================================================ */

#define MAZE_W     12
#define MAZE_H     12
#define MZ_NEAR    0.10f
#define MZ_FAR     7.5f
#define MZ_FOCAL   160.0f
/* Fixed-point z-unit scale.  The GPU recovers texels as
 * s = sdivz * 0x10000 / zi_persp (see Quake d_scan.c), so this scale cancels
 * exactly — but the int32 fields must not saturate.  The wedge's z*128 is
 * fine at z≈2-3; maze walls come as close as MZ_NEAR, where
 * sdivz = s*2^32/(K*z) needs K ≥ ~1600 to stay inside int32.  4096 leaves
 * 2.5x headroom at the near plane and zi is still ~140k at MZ_FAR. */
#define MZ_ZUNITS  4096.0f
#define MZ_MAX_QUADS 320

static const char maze_map[MAZE_H][MAZE_W + 1] = {
    "############",
    "#....#.....#",
    "#.##.#.###.#",
    "#.#..#...#.#",
    "#.#.####.#.#",
    "#.#......#.#",
    "#.#.####.#.#",
    "#...#..#...#",
    "###.#..#.###",
    "#...####...#",
    "#.........##",
    "############",
};

static float cam_x = 1.5f, cam_z = 1.5f, cam_a = 0.9f;

static int maze_solid(float x, float z)
{
    int ix = (int)x, iz = (int)z;
    if (ix < 0 || iz < 0 || ix >= MAZE_W || iz >= MAZE_H)
        return 1;
    return maze_map[iz][ix] == '#';
}

/* View-space vertex: x right, y up, z forward, s/t in texels. */
typedef struct {
    float x, y, z, s, t;
} mz_vert_t;

/* World → view: rotate around the camera so +z is the look direction. */
static mz_vert_t mz_xform(float wx, float wy, float wz, float s, float t)
{
    float dx = wx - cam_x, dz = wz - cam_z;
    float sn = sinf(cam_a), cs = cosf(cam_a);
    mz_vert_t v;
    v.x = dx * cs - dz * sn;
    v.z = dx * sn + dz * cs;
    v.y = wy - 0.5f;            /* eye height: mid-cell */
    v.s = s;
    v.t = t;
    return v;
}

/* Project one clipped view-space triangle and emit perspective span groups.
 * Same fixed-point conventions as emit_perspective_wedge (z_units = z*128,
 * 16.16 s/t), but with a flat per-surface light. */
static void emit_maze_tri(uint32_t fb_addr, const mz_vert_t *t0,
                          const mz_vert_t *t1, const mz_vert_t *t2,
                          const uint8_t *tex, uint8_t light)
{
    const mz_vert_t *src[3] = { t0, t1, t2 };
    persp_vert_t v[3];

    for (int i = 0; i < 3; ++i) {
        float z = src[i]->z;
        float inv = MZ_FOCAL / z;
        float z_units = z * MZ_ZUNITS;
        v[i].x = 160.0f + src[i]->x * inv;
        v[i].y = 120.0f - src[i]->y * inv;
        v[i].z = z;
        v[i].sdivz = fdiv16((int32_t)(src[i]->s * 65536.0f), z_units);
        v[i].tdivz = fdiv16((int32_t)(src[i]->t * 65536.0f), z_units);
        v[i].zi = fdiv16(1 << 16, z_units);
    }

    float denom = (v[1].x - v[0].x) * (v[2].y - v[0].y) -
                  (v[2].x - v[0].x) * (v[1].y - v[0].y);
    if (denom > -8.0f && denom < 8.0f)      /* degenerate / sub-pixel */
        return;

    int min_y = SCREEN_H - 1, max_y = 0;
    for (int i = 0; i < 3; ++i) {
        int lo = (int)floorf(v[i].y), hi = (int)ceilf(v[i].y);
        if (lo < min_y) min_y = lo;
        if (hi > max_y) max_y = hi;
    }
    min_y = clamp_int(min_y, 0, SCREEN_H - 1);
    max_y = clamp_int(max_y, 0, SCREEN_H - 1);
    if (max_y < min_y)
        return;

    int32_t sx, sy, s0, tx, ty, t0b, zx, zy, z0;
    if (!persp_planes_solve(v, denom, min_y, max_y,
                            &sx, &sy, &s0, &tx, &ty, &t0b, &zx, &zy, &z0))
        return;         /* near-edge-on: plane would saturate int32 — skip */

    of_gpu_persp_span_group_t groups[32];
    int group_count = 0;

    for (int y = min_y; y <= max_y && group_count < 32; y += 8) {
        int lanes = max_y - y + 1;
        if (lanes > 8)
            lanes = 8;

        of_gpu_persp_span_group_t g;
        memset(&g, 0, sizeof(g));
        g.fb_addr = fb_addr + (uint32_t)(y * SCREEN_W);
        g.tex_addr = (uint32_t)(uintptr_t)tex;
        g.lane_count = (uint8_t)lanes;
        g.flags = OF_GPU_SPAN_COLORMAP;
        g.colormap_id = 0;
        g.major_fb_step = SCREEN_W;
        g.minor_fb_step = 1;
        g.tex_width = TEX_W;
        g.tex_w_mask = TEX_W - 1;
        g.tex_h_mask = TEX_H - 1;
        g.sdivz = mad_wrap_i32(s0, sy, y);      /* wrapping: see solver note */
        g.tdivz = mad_wrap_i32(t0b, ty, y);
        g.zi_persp = mad_wrap_i32(z0, zy, y);
        g.sdivz_major_step = sy;
        g.tdivz_major_step = ty;
        g.zi_major_step = zy;
        g.sdivz_minor_step = sx;
        g.tdivz_minor_step = tx;
        g.zi_minor_step = zx;
        g.light = light;

        int any = 0;
        for (int lane = 0; lane < lanes; ++lane) {
            int left, right;
            if (edge_span_at_y(v, y + lane, &left, &right)) {
                g.start[lane] = (int16_t)left;
                g.count[lane] = (uint16_t)(right - left + 1);
                any = 1;
            }
        }
        if (any)
            groups[group_count++] = g;
    }

    if (group_count > 0)
        of_gpu_draw_persp_span_group_batch(groups, group_count);
}

/* Clip a view-space polygon against the plane a·x + b·y + c·z + d >= 0,
 * interpolating all five vertex fields (exact for plane clips in view space).
 * Returns the new vertex count. */
static int mz_clip_poly(const mz_vert_t *in, int n_in, mz_vert_t *out,
                        float a, float b, float c, float d)
{
    int n = 0;
    for (int i = 0; i < n_in; ++i) {
        const mz_vert_t *cur = &in[i];
        const mz_vert_t *nxt = &in[(i + 1) % n_in];
        float dc = a * cur->x + b * cur->y + c * cur->z + d;
        float dn = a * nxt->x + b * nxt->y + c * nxt->z + d;
        int cin = dc >= 0.0f, nin = dn >= 0.0f;
        if (cin)
            out[n++] = *cur;
        if (cin != nin) {
            float f = dc / (dc - dn);
            mz_vert_t m;
            m.x = cur->x + (nxt->x - cur->x) * f;
            m.y = cur->y + (nxt->y - cur->y) * f;
            m.z = cur->z + (nxt->z - cur->z) * f;
            m.s = cur->s + (nxt->s - cur->s) * f;
            m.t = cur->t + (nxt->t - cur->t) * f;
            out[n++] = m;
        }
    }
    return n;
}

/* Clip a view-space quad against the FULL view frustum (near + 4 side planes
 * with a small guard band), fan-triangulate, emit.  Side clipping matters
 * beyond just culling: an oblique wall clipped only at the near plane
 * projects vertices hundreds of pixels off-screen, and rebasing the texture
 * plane to screen origin through such extremes saturates the int32 gradient
 * fields — which shows up as angle-dependent texture warping.  Keeping every
 * vertex within ~48 px of the screen bounds keeps all plane values in range. */
static void emit_maze_quad(uint32_t fb_addr, const mz_vert_t q[4],
                           const uint8_t *tex)
{
    const float gx = (160.0f + 48.0f) / MZ_FOCAL;   /* x guard slope */
    const float gy = (120.0f + 48.0f) / MZ_FOCAL;   /* y guard slope */
    mz_vert_t buf_a[12], buf_b[12];
    int n;

    n = mz_clip_poly(q, 4, buf_a, 0.0f, 0.0f, 1.0f, -MZ_NEAR);   /* near   */
    if (n < 3) return;
    n = mz_clip_poly(buf_a, n, buf_b, -1.0f, 0.0f, gx, 0.0f);    /* right  */
    if (n < 3) return;
    n = mz_clip_poly(buf_b, n, buf_a, 1.0f, 0.0f, gx, 0.0f);     /* left   */
    if (n < 3) return;
    n = mz_clip_poly(buf_a, n, buf_b, 0.0f, -1.0f, gy, 0.0f);    /* top    */
    if (n < 3) return;
    n = mz_clip_poly(buf_b, n, buf_a, 0.0f, 1.0f, gy, 0.0f);     /* bottom */
    if (n < 3) return;

    mz_vert_t *out = buf_a;

    /* Distance light: farther surfaces pick darker palookup rows. */
    float zavg = 0.0f;
    for (int i = 0; i < n; ++i)
        zavg += out[i].z;
    zavg /= (float)n;
    uint8_t light = (uint8_t)clamp_int(4 + (int)(zavg * 7.0f), 4, 54);

    for (int i = 1; i + 1 < n; ++i)
        emit_maze_tri(fb_addr, &out[0], &out[i], &out[i + 1], tex, light);
}

/* One quad of the maze: kind 0 = floor, 1 = ceiling, 2..5 = wall N/S/W/E. */
typedef struct {
    float zc;                 /* view-space centre depth, for the sort */
    uint8_t i, j, kind;
} mz_quad_t;

static void maze_quad_corners(const mz_quad_t *q, mz_vert_t out[4])
{
    float x0 = (float)q->i, x1 = x0 + 1.0f;
    float z0 = (float)q->j, z1 = z0 + 1.0f;
    float S = (float)(TEX_W - 1);

    switch (q->kind) {
    case 0:  /* floor, y=0, seen from above */
        out[0] = mz_xform(x0, 0.0f, z0, 0, 0);
        out[1] = mz_xform(x1, 0.0f, z0, S, 0);
        out[2] = mz_xform(x1, 0.0f, z1, S, S);
        out[3] = mz_xform(x0, 0.0f, z1, 0, S);
        break;
    case 1:  /* ceiling, y=1 */
        out[0] = mz_xform(x0, 1.0f, z0, 0, 0);
        out[1] = mz_xform(x1, 1.0f, z0, S, 0);
        out[2] = mz_xform(x1, 1.0f, z1, S, S);
        out[3] = mz_xform(x0, 1.0f, z1, 0, S);
        break;
    case 2:  /* wall on north edge (z = j) */
        out[0] = mz_xform(x0, 1.0f, z0, 0, 0);
        out[1] = mz_xform(x1, 1.0f, z0, S, 0);
        out[2] = mz_xform(x1, 0.0f, z0, S, S);
        out[3] = mz_xform(x0, 0.0f, z0, 0, S);
        break;
    case 3:  /* wall on south edge (z = j+1) */
        out[0] = mz_xform(x0, 1.0f, z1, 0, 0);
        out[1] = mz_xform(x1, 1.0f, z1, S, 0);
        out[2] = mz_xform(x1, 0.0f, z1, S, S);
        out[3] = mz_xform(x0, 0.0f, z1, 0, S);
        break;
    case 4:  /* wall on west edge (x = i) */
        out[0] = mz_xform(x0, 1.0f, z0, 0, 0);
        out[1] = mz_xform(x0, 1.0f, z1, S, 0);
        out[2] = mz_xform(x0, 0.0f, z1, S, S);
        out[3] = mz_xform(x0, 0.0f, z0, 0, S);
        break;
    default: /* wall on east edge (x = i+1) */
        out[0] = mz_xform(x1, 1.0f, z0, 0, 0);
        out[1] = mz_xform(x1, 1.0f, z1, S, 0);
        out[2] = mz_xform(x1, 0.0f, z1, S, S);
        out[3] = mz_xform(x1, 0.0f, z0, 0, S);
        break;
    }
}

static void emit_maze_scene(uint32_t fb_addr)
{
    static mz_quad_t quads[MZ_MAX_QUADS];
    int nq = 0;
    float sn = sinf(cam_a), cs = cosf(cam_a);

    /* Collect visible quads: every empty cell contributes floor + ceiling and
     * one wall per solid neighbour.  Cull by view-space centre: behind the
     * camera, beyond MZ_FAR, or far outside the horizontal frustum. */
    for (int j = 0; j < MAZE_H; ++j) {
        for (int i = 0; i < MAZE_W; ++i) {
            if (maze_map[j][i] == '#')
                continue;
            float dx = (float)i + 0.5f - cam_x;
            float dz = (float)j + 0.5f - cam_z;
            float zc = dx * sn + dz * cs;
            float xc = dx * cs - dz * sn;
            if (zc < -1.0f || zc > MZ_FAR)
                continue;
            if (xc < -(zc + 1.5f) || xc > (zc + 1.5f))
                continue;

            uint8_t kinds[6];
            int nk = 0;
            kinds[nk++] = 0;                                   /* floor   */
            kinds[nk++] = 1;                                   /* ceiling */
            if (maze_map[j - 1][i] == '#') kinds[nk++] = 2;    /* north   */
            if (maze_map[j + 1][i] == '#') kinds[nk++] = 3;    /* south   */
            if (maze_map[j][i - 1] == '#') kinds[nk++] = 4;    /* west    */
            if (maze_map[j][i + 1] == '#') kinds[nk++] = 5;    /* east    */

            for (int k = 0; k < nk && nq < MZ_MAX_QUADS; ++k) {
                quads[nq].zc = zc;
                quads[nq].i = (uint8_t)i;
                quads[nq].j = (uint8_t)j;
                quads[nq].kind = kinds[k];
                ++nq;
            }
        }
    }

    /* Painter's order: far → near (insertion sort; the list is small and
     * mostly sorted frame-to-frame coherent). */
    for (int a = 1; a < nq; ++a) {
        mz_quad_t key = quads[a];
        int b = a - 1;
        while (b >= 0 && quads[b].zc < key.zc) {
            quads[b + 1] = quads[b];
            --b;
        }
        quads[b + 1] = key;
    }

    for (int a = 0; a < nq; ++a) {
        mz_vert_t c[4];
        const uint8_t *tex =
            quads[a].kind == 0 ? checker_tex :
            quads[a].kind == 1 ? persp_tex : wall_tex;
        maze_quad_corners(&quads[a], c);
        emit_maze_quad(fb_addr, c, tex);
        if ((a & 15) == 15)
            of_gpu_kick();      /* keep the GPU fed in chunks */
    }
    of_gpu_kick();
}

/* D-pad navigation, called once per frame while MAZE mode is active. */
static void maze_input(void)
{
    const float turn = 0.05f, step = 0.06f, margin = 0.22f;

    if (of_btn(OF_BTN_LEFT))  cam_a -= turn;
    if (of_btn(OF_BTN_RIGHT)) cam_a += turn;

    float fx = sinf(cam_a), fz = cosf(cam_a);
    float mx = 0.0f, mz = 0.0f;
    if (of_btn(OF_BTN_UP))   { mx += fx * step; mz += fz * step; }
    if (of_btn(OF_BTN_DOWN)) { mx -= fx * step; mz -= fz * step; }
    if (mx == 0.0f && mz == 0.0f)
        return;

    /* Per-axis slide with a wall margin so corners don't snag. */
    float nx = cam_x + mx;
    if (!maze_solid(nx + (mx > 0.0f ? margin : -margin), cam_z))
        cam_x = nx;
    float nz = cam_z + mz;
    if (!maze_solid(cam_x, nz + (mz > 0.0f ? margin : -margin)))
        cam_z = nz;
}

static uint32_t draw_begin(int draw_idx, uint32_t *fb_addr)
{
    volatile uint8_t *fb = current_fb(draw_idx);
    *fb_addr = (uint32_t)(uintptr_t)fb;
    set_framebuffer(draw_idx);
    return of_time_us();
}

static void draw_finish(uint32_t t0)
{
    uint32_t before_finish = of_time_us();
    /* Bounded equivalent of of_gpu_finish(): submit + kick, then poll the
     * fence with a timeout so a GPU whose command/fence path is wedged
     * degrades gracefully instead of spinning forever. */
    uint32_t token = of_gpu_submit();
    while (!of_gpu_fence_reached(token)) {
        if ((uint32_t)(of_time_us() - before_finish) > 500000u) {  /* 500 ms */
            gpu_wedged = 1;
            /* Dump the GPU's internal state at the freeze: the fence it
             * actually reached, whether the ring drained (rd vs wr), and the
             * status bits — says what the GPU is stuck on, for the OS/RTL side. */
            of_gpu_debug_snapshot_t s;
            of_gpu_debug_snapshot(&s, 0);
            printf("[gpudemo] WEDGE waiting fence %u: status=%08x rd=%u wr=%u "
                   "fence_reached=%u busy=%d dma_busy=%d ring_empty=%d "
                   "dma_waits=%u ring_free=%u\n",
                   (unsigned)token, (unsigned)s.status,
                   (unsigned)s.rdptr, (unsigned)s.wrptr, (unsigned)s.fence_reached,
                   (int)(s.status & 0x1u), (int)((s.status >> 2) & 0x1u),
                   (int)((s.status >> 1) & 0x1u),
                   (unsigned)s.dma_waits, (unsigned)s.ring_free);
            break;
        }
    }
    uint32_t after_finish = of_time_us();
    stat_cpu_us += (unsigned)(before_finish - t0);
    stat_gpu_us += (unsigned)(after_finish - before_finish);
}

static void draw_mode(int mode, int draw_idx, unsigned frame)
{
    uint32_t fb_addr;
    uint32_t t0 = draw_begin(draw_idx, &fb_addr);

    switch (mode) {
    case 0:
        emit_affine_scene(fb_addr, frame);
        break;
    case 1:
        emit_affine_scene(fb_addr, frame);
        emit_sprite_post(fb_addr, 84 + ((sin_lut[(frame * 2) & 255] * 18) >> 15), 202, 5);
        emit_sprite_post(fb_addr, 164 + ((sin_lut[(frame * 3 + 64) & 255] * 24) >> 15), 214, 6);
        emit_sprite_post(fb_addr, 244 + ((sin_lut[(frame * 2 + 128) & 255] * 14) >> 15), 194, 4);
        break;
    case 2:
        clear_frame(fb_addr, 0x10);
        emit_colormap_panel(fb_addr, 22, 34, 128, 172, 0);
        emit_colormap_panel(fb_addr, 170, 34, 128, 172, 1);
        break;
    case 3: {
        emit_affine_scene(fb_addr, frame);
        int x = 92 + ((cos_lut[(frame * 2) & 255] * 38) >> 15);
        int y = 82 + ((sin_lut[(frame * 3) & 255] * 26) >> 15);
        emit_blend_rect(fb_addr, x, y, 136, 72);
        break;
    }
    case 4:
        clear_frame(fb_addr, 0x10);
        emit_rows(fb_addr, 0, 160, SCREEN_W, 80, checker_tex, TEX_W, TEX_W - 1, TEX_H - 1,
                  OF_GPU_SPAN_COLORMAP, 0, frame << 8, 0, 0x12000, 0x8000, 28, 0);
        emit_perspective_wedge(fb_addr, frame);
        break;
    case 5:
    default:
        clear_frame(fb_addr, 0x10);
        emit_maze_scene(fb_addr);
        break;
    }

    draw_finish(t0);
}

static void put_pixel(volatile uint8_t *fb, int x, int y, uint8_t color)
{
    if ((unsigned)x >= SCREEN_W || (unsigned)y >= SCREEN_H)
        return;
    fb[y * SCREEN_W + x] = color;
}

static uint16_t glyph_bits(char c)
{
    switch (c) {
    case '0': return 0x7b6f;
    case '1': return 0x2492;
    case '2': return 0x73e7;
    case '3': return 0x73cf;
    case '4': return 0x5bc9;
    case '5': return 0x79cf;
    case '6': return 0x79ef;
    case '7': return 0x7249;
    case '8': return 0x7bef;
    case '9': return 0x7bcf;
    case 'A': return 0x5bef;
    case 'B': return 0x7bcf;
    case 'C': return 0x7927;
    case 'D': return 0x7b6f;
    case 'E': return 0x79e7;
    case 'F': return 0x79e4;
    case 'G': return 0x79af;
    case 'I': return 0x7497;
    case 'K': return 0x5b6d;
    case 'L': return 0x4927;
    case 'M': return 0x5f6d;
    case 'N': return 0x5fed;
    case 'P': return 0x7be4;
    case 'R': return 0x7bed;
    case 'S': return 0x79cf;
    case 'T': return 0x7484;
    case 'U': return 0x5b6f;
    case 'X': return 0x5aad;
    case '%': return 0x5265;
    case ':': return 0x0410;
    case '.': return 0x0002;
    case ' ': return 0x0000;
    default: return 0x0000;
    }
}

static void draw_text(volatile uint8_t *fb, int x, int y, const char *text, uint8_t color)
{
    for (const char *p = text; *p; ++p, x += 4) {
        uint16_t bits = glyph_bits(*p);
        for (int row = 0; row < 5; ++row) {
            for (int col = 0; col < 3; ++col) {
                if (bits & (1u << (14 - (row * 3 + col))))
                    put_pixel(fb, x + col, y + row, color);
            }
        }
    }
}

static void draw_overlay(int draw_idx, int mode)
{
    volatile uint8_t *fb = current_fb(draw_idx);
    char line[56];

    for (int y = 0; y < OVERLAY_H; ++y) {
        volatile uint8_t *row = fb + y * SCREEN_W;
        for (int x = 0; x < SCREEN_W; ++x)
            row[x] = (x < 132 && y < 9) ? 0x10 : row[x];
    }

    snprintf(line, sizeof(line), "%d %s %uX%u", mode, mode_names[mode],
             scr_w, scr_h);
    draw_text(fb, 4, 2, line, 0xef);

    if (show_stats) {
        snprintf(line, sizeof(line), "%u.%u FPS C%u%% G%u%%",
                 last_fps_x10 / 10, last_fps_x10 % 10, last_cpu_pct, last_gpu_pct);
        draw_text(fb, 4, 10, line, 0xdf);
    }

    of_cache_clean_range((void *)fb, SCREEN_W * OVERLAY_H);
}

static void update_stats(uint32_t now, uint32_t *last_tick)
{
    ++stat_frames;
    uint32_t elapsed = now - *last_tick;
    if (elapsed < 1000000)
        return;

    last_fps_x10 = (unsigned)((uint64_t)stat_frames * 10000000ull / elapsed);
    last_cpu_pct = (unsigned)((uint64_t)stat_cpu_us * 100ull / elapsed);
    last_gpu_pct = (unsigned)((uint64_t)stat_gpu_us * 100ull / elapsed);

    stat_cpu_us = 0;
    stat_gpu_us = 0;
    stat_frames = 0;
    *last_tick = now;
}

/* No usable GPU: a real game would switch to its software renderer here.
 * We don't have one, so report on the OS terminal (visible on-screen and
 * over UART) and park — graceful degradation without poking the framebuffer
 * or palette, whose state we no longer control once the GPU is out. */
static void cpu_fallback(void)
{
    of_video_set_display_mode(OF_DISPLAY_TERMINAL);
    printf("\033[2J\033[H");
    printf("  gpudemo: no usable GPU on this core\n");
    printf("  (OF_HW_GPU_SPAN / gpu_base unavailable)\n");
    for (;;) {
        of_input_poll_p0();
        usleep(16 * 1000);
    }
}

/* Bounded fence wait; on timeout dumps the GPU's internal state (which fence
 * it reached, ring rd/wr, status bits) so a stall is diagnosable from UART. */
static int bounded_fence(const char *what, uint32_t timeout_us)
{
    uint32_t tok = of_gpu_submit();
    uint32_t t0 = of_time_us();
    while (!of_gpu_fence_reached(tok)) {
        if ((uint32_t)(of_time_us() - t0) > timeout_us) {
            of_gpu_debug_snapshot_t s;
            of_gpu_debug_snapshot(&s, 0);
            printf("[gpudemo] %s STALL fence=%u reached=%u status=%08x rd=%u wr=%u ring_free=%u\n",
                   what, (unsigned)tok, (unsigned)s.fence_reached,
                   (unsigned)s.status, (unsigned)s.rdptr, (unsigned)s.wrptr,
                   (unsigned)s.ring_free);
            return 0;
        }
    }
    printf("[gpudemo] %s ok\n", what);
    return 1;
}

/* One real 1-lane affine span through the same path the scene uses.  On os30
 * these drained as no-ops, so this is only meaningful on a span-group core —
 * it is the first time actual span EXECUTION is verified. */
static int probe_one_span(uint32_t fb_addr)
{
    of_gpu_affine_span_group_t g;
    memset(&g, 0, sizeof g);
    g.lane_count  = 1;
    g.flags       = OF_GPU_SPAN_COLORMAP;
    g.tex_width   = TEX_W;
    g.tex_w_mask  = TEX_W - 1;
    g.tex_h_mask  = TEX_H - 1;
    g.fb_step     = 1;
    g.fb_addr[0]  = fb_addr + 100u * SCREEN_W + 8u;
    g.tex_addr[0] = (uint32_t)(uintptr_t)checker_tex;
    g.count[0]    = 64;
    g.sstep[0]    = 0x10000;
    g.light[0]    = 20;
    of_gpu_draw_affine_span_group(&g);
    return bounded_fence("probe span", 300000u);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!alloc_assets()) {
        printf("[gpudemo] asset allocation failed\n");
        return 1;
    }

    build_luts();
    build_palette();
    build_colormaps();
    build_textures();
    flush_textures();
    build_translucency();

    /* Canonical bring-up: read caps, gate on the GPU feature bit, then init.
     * (CLAUDE.md also suggests a draw-and-read-back probe, but reading the
     * framebuffer back on the CPU assumes it lives in the cached-SDRAM alias,
     * which isn't guaranteed — so we gate on OF_HW_GPU_SPAN, which is.) */
    const struct of_capabilities *caps = of_get_caps();
    if (!(caps->hw_features & OF_HW_GPU_SPAN) || caps->gpu_base == 0) {
        printf("[gpudemo] caps report no GPU span unit\n");
        cpu_fallback();
    }

    of_gpu_init();
    upload_tables();

    int draw_idx = of_video_acquire_next(-1, 0);

    /* Gate each mode on the exact GPU feature it needs.  AFFINE/MASK/CMAP
     * (and the BLEND overlay) emit the 0x48 compact-direct span-group form,
     * which requires OF_HW_GPU_SPAN_GROUP — it is CLEAR on lean cores (e.g.
     * Pocket OS30, the Quake2-only GPU), where that form is unsupported, so
     * those modes must self-gate on the bit rather than assume the baseline
     * OF_HW_GPU_SPAN.  BLEND additionally needs alpha; PERSP uses the
     * perspective span group (OF_HW_GPU_PERSP, decodes on every variant).
     * Modes whose bit is clear are dropped from the cycle. */
    has_span_group = of_has_feature(OF_HW_GPU_SPAN_GROUP);
    has_transluc   = of_has_feature(OF_HW_GPU_ALPHA);
    has_persp      = of_has_feature(OF_HW_GPU_PERSP);
    avail_count = 0;
    if (has_span_group) {
        avail_modes[avail_count++] = 0;                   /* AFFINE */
        avail_modes[avail_count++] = 1;                   /* MASK   */
        avail_modes[avail_count++] = 2;                   /* CMAP   */
        if (has_transluc) avail_modes[avail_count++] = 3; /* BLEND (needs ALPHA) */
        if (has_persp)    avail_modes[avail_count++] = 4; /* PERSP (also draws an affine ground) */
    }
    /* MAZE uses only perspective span groups (long-form 0x48, decodes on
     * every variant) — available even without the compact span-group bit. */
    if (has_persp) avail_modes[avail_count++] = 5;
    if (avail_count == 0) {
        /* No affine span groups → every gpudemo mode is unrenderable.  This is
         * a lean core (e.g. os30/Quake2, which cuts OF_HW_GPU_SPAN_GROUP);
         * gpudemo needs the full os25 Pocket bitstream (VARIANT=os25). */
        printf("[gpudemo] no supported GPU draw modes (SPAN_GROUP=%d) — needs "
               "the os25 Pocket bitstream; os30 is Quake2-only.\n", has_span_group);
        cpu_fallback();
    }

    int mi = 0;
    unsigned frame = 0;
    uint32_t stat_tick = of_time_us();

    /* Real-execution probe: one clear + ONE actual affine span, each with a
     * bounded fence.  (All earlier diagnostics ran on os30 where span groups
     * drain as no-ops, so genuine span execution was never verified.) */
    {
        uint32_t fb0 = (uint32_t)(uintptr_t)current_fb(draw_idx);
        set_framebuffer(draw_idx);
        clear_frame(fb0, 0x10);
        if (!bounded_fence("probe clear", 300000u))
            cpu_fallback();
        if (!probe_one_span(fb0))
            cpu_fallback();
    }

    /* Show the app framebuffer — scanout may still be on the boot terminal
     * otherwise, leaving GPU-rendered frames invisible. */
    of_video_set_display_mode(OF_DISPLAY_FRAMEBUFFER);

    /* Active video mode, for the overlay HUD. */
    {
        of_video_mode_t vm;
        of_video_get_mode(&vm);
        scr_w = vm.width;
        scr_h = vm.height;
    }

    printf("[gpudemo] ready (SPAN_GROUP=%d ALPHA=%d PERSP=%d, %d modes)\n",
           has_span_group, has_transluc, has_persp, avail_count);

    while (1) {
        of_input_poll_p0();

        if (of_btn_pressed(OF_BTN_A))
            mi = (mi + 1) % avail_count;
        if (of_btn_pressed(OF_BTN_B))
            show_stats = !show_stats;

        int mode = avail_modes[mi];
        if (mode == 5)
            maze_input();       /* d-pad: turn + fly through the maze */
        draw_mode(mode, draw_idx, frame);
        if (gpu_wedged) {
            printf("[gpudemo] GPU fence timeout on first render — the GPU "
                   "command/fence path is not completing (OS/bitstream issue, "
                   "not the demo). Dropping to terminal.\n");
            cpu_fallback();
        }
        draw_overlay(draw_idx, mode);

        uint32_t token = of_gpu_flip_to(draw_idx);
        of_gpu_kick();
        draw_idx = of_video_acquire_next(draw_idx, token);

        update_stats(of_time_us(), &stat_tick);
        ++frame;
    }
}
