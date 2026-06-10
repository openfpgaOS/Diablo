//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * gpudemo: current GPU path showcase for openfpgaOS.
 *
 * The demo intentionally sticks to the production APIs:
 * affine span groups, perspective span groups, framebuffer clear/flip/fence,
 * palette lookup uploads, and translucency table uploads.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#define MODE_COUNT 5
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

static const char *const mode_names[MODE_COUNT] = {
    "AFFINE",
    "MASK",
    "CMAP",
    "BLEND",
    "PERSP",
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

static inline int32_t clamp_double_to_i32(double v)
{
    if (v < (double)INT32_MIN)
        return INT32_MIN;
    if (v > (double)INT32_MAX)
        return INT32_MAX;
    return (int32_t)v;
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
    of_cache_clean_range(checker_tex, TEX_W * TEX_H);
    of_cache_clean_range(wall_tex, TEX_W * TEX_H);
    of_cache_clean_range(sprite_tex, SPRITE_W * SPRITE_H);
    of_cache_clean_range(persp_tex, TEX_W * TEX_H);
    of_cache_clean_range(blend_tex, 1);
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

    for (int y = 96; y < SCREEN_H; y += 4) {
        int row = y - 96;
        int32_t stretch = 0x9000 + row * 640;
        int32_t drift = (int32_t)((frame * 320 + row * 512) & 0x003fffff);
        emit_rows(fb_addr, 0, y, SCREEN_W, 4, checker_tex, TEX_W, TEX_W - 1, TEX_H - 1,
                  OF_GPU_SPAN_COLORMAP, 0, drift, row << 15, stretch, 0x7000, 18, 0);
    }

    emit_columns(fb_addr, 28, 68, 52, 128, wall_tex, TEX_W, TEX_W - 1, TEX_H - 1,
                 OF_GPU_SPAN_COLORMAP, 0, 18);
    emit_columns(fb_addr, 118, 44, 76, 166, wall_tex, TEX_W, TEX_W - 1, TEX_H - 1,
                 OF_GPU_SPAN_COLORMAP, 0, 6);
    emit_columns(fb_addr, 238, 78, 48, 116, wall_tex, TEX_W, TEX_W - 1, TEX_H - 1,
                 OF_GPU_SPAN_COLORMAP, 0, 28);
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

static int32_t fdiv16(int32_t a, float b)
{
    if (b == 0.0f)
        return 0;
    return clamp_double_to_i32((double)a * 65536.0 / (double)b);
}

static int32_t gradient_x(const persp_vert_t *v, double denom, int32_t a0, int32_t a1, int32_t a2)
{
    double num = (double)a0 * (double)(v[1].y - v[2].y) +
                 (double)a1 * (double)(v[2].y - v[0].y) +
                 (double)a2 * (double)(v[0].y - v[1].y);
    return clamp_double_to_i32(num / denom);
}

static int32_t gradient_y(const persp_vert_t *v, double denom, int32_t a0, int32_t a1, int32_t a2)
{
    double num = (double)a0 * (double)(v[2].x - v[1].x) +
                 (double)a1 * (double)(v[0].x - v[2].x) +
                 (double)a2 * (double)(v[1].x - v[0].x);
    return clamp_double_to_i32(num / denom);
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
    const float src[3][3] = {
        {-1.25f, -0.85f, 2.70f},
        {1.25f, -0.70f, 2.45f},
        {-0.20f, 1.05f, 2.00f},
    };
    const int tex_s[3] = {2, 61, 30};
    const int tex_t[3] = {4, 10, 60};

    persp_vert_t v[3];
    for (int i = 0; i < 3; ++i) {
        float x = src[i][0] * ca - src[i][2] * 0.18f * sa;
        float z = src[i][2] + src[i][0] * 0.35f * sa;
        float y = src[i][1];
        float inv = 86.0f / z;
        float z_units = z * 128.0f;
        v[i].x = 160.0f + x * inv;
        v[i].y = 122.0f + y * inv;
        v[i].z = z;
        v[i].sdivz = fdiv16(tex_s[i] << 16, z_units);
        v[i].tdivz = fdiv16(tex_t[i] << 16, z_units);
        v[i].zi = fdiv16(1 << 16, z_units);
    }

    double denom = (double)(v[1].x - v[0].x) * (double)(v[2].y - v[0].y) -
                   (double)(v[2].x - v[0].x) * (double)(v[1].y - v[0].y);
    if (denom > -8.0 && denom < 8.0)
        return 0;

    int32_t sx = gradient_x(v, denom, v[0].sdivz, v[1].sdivz, v[2].sdivz);
    int32_t sy = gradient_y(v, denom, v[0].sdivz, v[1].sdivz, v[2].sdivz);
    int32_t tx = gradient_x(v, denom, v[0].tdivz, v[1].tdivz, v[2].tdivz);
    int32_t ty = gradient_y(v, denom, v[0].tdivz, v[1].tdivz, v[2].tdivz);
    int32_t zx = gradient_x(v, denom, v[0].zi, v[1].zi, v[2].zi);
    int32_t zy = gradient_y(v, denom, v[0].zi, v[1].zi, v[2].zi);

    int32_t s0 = clamp_double_to_i32((double)v[0].sdivz - (double)sx * v[0].x - (double)sy * v[0].y);
    int32_t t0 = clamp_double_to_i32((double)v[0].tdivz - (double)tx * v[0].x - (double)ty * v[0].y);
    int32_t z0 = clamp_double_to_i32((double)v[0].zi - (double)zx * v[0].x - (double)zy * v[0].y);

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
        g.sdivz = s0 + sy * y;
        g.tdivz = t0 + ty * y;
        g.zi_persp = z0 + zy * y;
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
    of_gpu_finish();
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
    default:
        clear_frame(fb_addr, 0x10);
        emit_rows(fb_addr, 0, 160, SCREEN_W, 80, checker_tex, TEX_W, TEX_W - 1, TEX_H - 1,
                  OF_GPU_SPAN_COLORMAP, 0, frame << 8, 0, 0x12000, 0x8000, 28, 0);
        emit_perspective_wedge(fb_addr, frame);
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

    snprintf(line, sizeof(line), "%d %s", mode, mode_names[mode]);
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

    of_gpu_init();
    upload_tables();

    int draw_idx = of_video_acquire_next(-1, 0);
    int mode = 0;
    unsigned frame = 0;
    uint32_t stat_tick = of_time_us();

    printf("[gpudemo] ready\n");

    while (1) {
        of_input_poll_p0();

        if (of_btn_pressed(OF_BTN_A))
            mode = (mode + 1) % MODE_COUNT;
        if (of_btn_pressed(OF_BTN_B))
            show_stats = !show_stats;

        draw_mode(mode, draw_idx, frame);
        draw_overlay(draw_idx, mode);

        uint32_t token = of_gpu_flip_to(draw_idx);
        of_gpu_kick();
        draw_idx = of_video_acquire_next(draw_idx, token);

        update_stats(of_time_us(), &stat_tick);
        ++frame;
    }
}
