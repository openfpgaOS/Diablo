//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * triangles: the canonical "drive this GPU well" example — CPU geometry feeding
 * the os30 hardware vert-tri raster.
 *
 * A grid of spinning brick-textured cubes.  The os30 bitstream builds the
 * vert-tri rasteriser (VERT_TRI + VCOLOR/DIRECT_COLOR + PERSP) but deliberately
 * does NOT build the transform-and-light front-end (no INCLUDE_XFORM /
 * OF_HW_GPU_XFORM_RGB), so the CPU does the per-vertex math itself: for each
 * visible face vertex it applies the shared model->camera matrix, perspective-
 * projects to screen, and forms the perspective texture terms (raw texel s/t
 * plus a per-triangle-scaled 1/w in zi) and the decoupled z-buffer depth.  It
 * then streams screen-space, perspective-correct, depth-tested, truecolour
 * triangles via GPU_CMD_DRAW_VERT_TRI_RGB (0x4E / of_gpu_draw_vert_tri_rgb) on
 * top of a sticky 0x4A surface state.  The GPU edge-walks, perspective-divides
 * texel = interp(s*zi)/interp(zi), samples the brick texture, modulates by the
 * per-vertex shade colour, and z-tests against an SDRAM z-buffer.
 *
 * Lighting is flat per face: every cube shares ONE orientation, so the
 * model->camera rotation and the object-space light direction are computed
 * ONCE per frame; the face N.L (normals are object-space, the light is rotated
 * into object space to match) gives a single RGB565 shade applied to all six
 * vertices of the face's two triangles.  A cube costs only: a frustum test
 * and — per visible face — a back-face cull plus four projected corners and two
 * triangles.  Off-screen and behind-camera cubes are skipped whole.  All
 * framebuffer writes (incl. the HUD) go through the GPU; the frame is paced by
 * the flip fence with triple buffering, so CPU geometry for frame N+1 overlaps
 * GPU raster of frame N — no blocking fence wait.
 *
 * Controls:
 *   d-pad UP / DOWN   more / fewer cubes (grid grows N x N)
 *   d-pad LEFT/RIGHT  320x240 / 640x480
 *   A  pause/resume       B  toggle HUD
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
#include <of_texture.h>

#include "brick_tex.h"

#define TEX_W BRICK_W            /* 64 */
#define TEX_H BRICK_H            /* 64 */

#define MAX_W 640                /* buffers sized for the largest mode */
#define MAX_H 480

#define MIN_SIDE 1
#define MAX_SIDE 16              /* up to 16x16 = 256 cubes */
#define HALF     0.7f           /* cube half-extent */
#define SPACING  2.2f
#define CAM_NEAR 6.0f           /* camera distance = CAM_NEAR + extent*CAM_K */
#define CAM_K    2.2f
#define SCENE_PITCH 0.55f

#define HUD_W    128            /* power-of-two: wraps cleanly */
#define HUD_H    16
#define HUD_X    3
#define HUD_Y    3

#define ZBUF_ELEM 2             /* RTL-hardwired 16-bit z entries */
#define FB_BPP    2             /* RGB565 truecolour framebuffer: 2 bytes/pixel */

/* float -> Q16.16 (the wire format for verts, normals, matrices). */
#define Q16(f) ((int32_t)lrintf((f) * 65536.0f))

/* 0x00RRGGBB -> RGB565 (the truecolour texel / vertex-colour format). */
static inline uint16_t rgb565_888(uint32_t c)
{
    uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static uint16_t *cube_tex;       /* RGB565 brick texels */
static uint16_t *hud_tex;        /* RGB565 HUD plate + glyphs */
static uint16_t *zbuf;

/* Fast texture memory (os30 `OF_HW_GPU_FAST_TEX`).  When both RGB565 textures
 * fit, everything lives in the fast tier and the GPU fetch domain never flips —
 * see the main() setup.  Otherwise tex_fast stays 0 and the SDRAM path
 * (CPU-address tex_addr) is used, exactly as on a core without fast memory. */
static of_texture_t cube_th, hud_th;
static int          tex_fast;

/* HUD colours (RGB565), derived from the brick palette plate/text entries. */
static uint16_t hud_plate, hud_text_col;

/* Active video mode (runtime). */
static int   vw = 320, vh = 240, fb_stride = 320 * FB_BPP, hires = 0;
static float cx = 160.0f, cy = 120.0f, focal = 300.0f;

static int  has_zbuffer;
static int  grid_side = 4;       /* N x N cubes */
static int  paused = 0;
static int  show_hud = 1;

static unsigned stat_frames, stat_tris_last;
static unsigned last_fps_x10;
static char hud_l1[40], hud_l2[40];

static inline int clamp_int(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ================================================================
 * Assets — RGB565 brick texture (no palette / colormap: truecolour)
 * ================================================================ */

static void load_texture(void)
{
    for (int i = 0; i < TEX_W * TEX_H; ++i)
        cube_tex[i] = rgb565_888(brick_pal[brick_pix[i]]);
    of_cache_flush_range(cube_tex, TEX_W * TEX_H * FB_BPP);
}

/* ================================================================
 * GPU-composited HUD (RGB565 texture, drawn as a screen-space quad)
 * ================================================================ */

static uint16_t glyph_bits(char ch)
{
    switch (ch) {
    case '0': return 0x7b6f; case '1': return 0x2492; case '2': return 0x73e7;
    case '3': return 0x73cf; case '4': return 0x5bc9; case '5': return 0x79cf;
    case '6': return 0x79ef; case '7': return 0x7249; case '8': return 0x7bef;
    case '9': return 0x7bcf;
    case 'A': return 0x5bef; case 'B': return 0x7bcf; case 'C': return 0x7927;
    case 'E': return 0x79e7; case 'F': return 0x79e4; case 'G': return 0x79af;
    case 'I': return 0x7497; case 'L': return 0x4927; case 'N': return 0x5fed;
    case 'P': return 0x7be4; case 'R': return 0x7bed; case 'S': return 0x79cf;
    case 'T': return 0x7484; case 'U': return 0x5b6f; case 'X': return 0x5aad;
    case '%': return 0x5265; case ':': return 0x0410; case '.': return 0x0002;
    case ' ': return 0x0000;
    default:  return 0x0000;
    }
}

static void hud_text(int x, int y, const char *s, uint16_t color)
{
    for (const char *p = s; *p; ++p, x += 4) {
        uint16_t bits = glyph_bits(*p);
        for (int row = 0; row < 5; ++row)
            for (int col = 0; col < 3; ++col)
                if ((bits & (1u << (14 - (row * 3 + col)))) &&
                    (unsigned)(x + col) < HUD_W && (unsigned)(y + row) < HUD_H)
                    hud_tex[(y + row) * HUD_W + (x + col)] = color;
    }
}

static void hud_update(const char *l1, const char *l2)
{
    if (!strcmp(l1, hud_l1) && !strcmp(l2, hud_l2))
        return;
    for (int i = 0; i < HUD_W * HUD_H; ++i)
        hud_tex[i] = hud_plate;
    hud_text(2, 1, l1, hud_text_col);
    hud_text(2, 9, l2, hud_text_col);
    if (tex_fast) {
        /* The HUD lives in the fast tier; re-upload in place so the domain
         * never has to flip to SDRAM.  Drain first — this only runs when the
         * text actually changes (~once a second), so the cost is negligible,
         * and it stops us clobbering a frame still fetching the old HUD. */
        of_gpu_finish();
        _of_gpu_fast_tex_upload(of_texture_gpu_addr(&hud_th),
                                (const uint32_t *)hud_tex,
                                (HUD_W * HUD_H * FB_BPP + 3) >> 2);
    } else {
        of_cache_flush_range(hud_tex, HUD_W * HUD_H * FB_BPP);
    }
    snprintf(hud_l1, sizeof(hud_l1), "%s", l1);
    snprintf(hud_l2, sizeof(hud_l2), "%s", l2);
}

/* ================================================================
 * Math (single precision; rv32imafc has no hardware doubles)
 * ================================================================ */

typedef struct { float x, y, z; } vec3;

static inline vec3 vadd(vec3 a, vec3 b) { return (vec3){ a.x+b.x, a.y+b.y, a.z+b.z }; }
static inline vec3 vneg(vec3 a)         { return (vec3){ -a.x, -a.y, -a.z }; }
static inline float vdot(vec3 a, vec3 b){ return a.x*b.x + a.y*b.y + a.z*b.z; }

static void mat_mul(const float a[9], const float b[9], float o[9])
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            o[r * 3 + c] = a[r * 3 + 0] * b[0 * 3 + c]
                         + a[r * 3 + 1] * b[1 * 3 + c]
                         + a[r * 3 + 2] * b[2 * 3 + c];
}
static void rot_x(float a, float m[9])
{
    float c = cosf(a), s = sinf(a);
    m[0]=1; m[1]=0; m[2]=0;  m[3]=0; m[4]=c; m[5]=-s;  m[6]=0; m[7]=s; m[8]=c;
}
static void rot_y(float a, float m[9])
{
    float c = cosf(a), s = sinf(a);
    m[0]=c; m[1]=0; m[2]=s;  m[3]=0; m[4]=1; m[5]=0;  m[6]=-s; m[7]=0; m[8]=c;
}

/* Unit cube: 8 corners, 6 faces (CCW from outside) with model-space normals. */
static const float cube_v[8][3] = {
    {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
    {-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1},
};
static const int   cube_idx[6][4] = {
    {0,1,2,3},{5,4,7,6},{4,0,3,7},{1,5,6,2},{3,2,6,7},{4,5,1,0},
};
/* Object-space face normals (front -z, back +z, left -x, right +x, top +y,
 * bottom -y) — handed to the GPU per vertex; it does the N.L itself. */
static const float MN[6][3] = {
    {0,0,-1},{0,0,1},{-1,0,0},{1,0,0},{0,1,0},{0,-1,0},
};
/* Per-face texel UVs, precomputed Q16.16 (constant — never recomputed). */
static const int32_t UVF[4][2] = {
    {0, 0}, {TEX_W << 16, 0}, {TEX_W << 16, TEX_H << 16}, {0, TEX_H << 16},
};

static const vec3 LIGHT_DIR = { -0.40f, 0.55f, -0.73f };
/* Directional light colour and ambient floor (RGB565).  texel*(ambient+N.L*lit)
 * is the final fragment, so a bright light + dim ambient lights faces toward
 * LIGHT_DIR and darkens those facing away. */
#define LIGHT_RGB565   0xFFFF                 /* white directional */
#define AMBIENT_RGB565 0x4208                 /* ~rgb(64,64,64) floor */

/* Per-triangle perspective-scale targets (match SM64's gfx_gpu.c ZI_*).  The HW
 * vert-tri derive forms s*zi and zi in a Q16.16 window that starves on small
 * 1/w; scale zi so the largest of {1/w, u/w, v/w} lands near ZI_DERIVE_TARGET.
 * The scale CANCELS in (s*zi)/zi, so it is free to vary per triangle; depth is
 * decoupled (1/w * 2^30, GPU reads sp_depth_value not zi) so this never touches
 * the z-buffer.  ZI_SCALE is the degenerate (zero-magnitude) fallback. */
#define ZI_DERIVE_TARGET 8192.0f
#define ZI_SCALE         64.0f

/* Flat face shade -> RGB565 (the per-vertex colour the GPU modulates the texel
 * by: fragment = texel * shade).  Replaces the GPU light-state's N.L: per RGB565
 * channel, shade = ambient + clamp01(N.L) * light, exactly as the old hardware
 * GPU_CMD_SET_LIGHT_STATE computed it, so the lit cubes look identical. */
static inline uint16_t flat_shade565(float ndotl)
{
    if (ndotl < 0.0f)      ndotl = 0.0f;
    else if (ndotl > 1.0f) ndotl = 1.0f;
    int lr = (LIGHT_RGB565   >> 11) & 0x1F, lg = (LIGHT_RGB565   >> 5) & 0x3F, lb = LIGHT_RGB565   & 0x1F;
    int ar = (AMBIENT_RGB565 >> 11) & 0x1F, ag = (AMBIENT_RGB565 >> 5) & 0x3F, ab = AMBIENT_RGB565 & 0x1F;
    int r = ar + (int)lrintf(ndotl * (float)lr);
    int g = ag + (int)lrintf(ndotl * (float)lg);
    int b = ab + (int)lrintf(ndotl * (float)lb);
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* ================================================================
 * Per-frame shared geometry
 * ================================================================ */

/* The model->camera rotation is identical for every cube, so it (m) and the
 * object-space light direction (lobj) are computed ONCE per frame.  vn holds the
 * six camera-space face normals for back-face culling; rvc0/rvc2 are the
 * view-rotation columns used to place each cube's centre (grid on XZ, y = 0). */
static void build_geom(float spin_y, float spin_x, float orbit,
                       float m[9], vec3 vn[6], vec3 *lobj,
                       vec3 *rvc0, vec3 *rvc2)
{
    float rs[9], rv[9], ta[9], tb[9];
    rot_y(spin_y, ta); rot_x(spin_x, tb); mat_mul(ta, tb, rs);     /* R_spin */
    rot_x(SCENE_PITCH, ta); rot_y(orbit, tb); mat_mul(ta, tb, rv); /* R_view */
    mat_mul(rv, rs, m);                                            /* M = Rv*Rs */

    vec3 c0 = { m[0], m[3], m[6] };       /* columns of M (camera-space axes) */
    vec3 c1 = { m[1], m[4], m[7] };
    vec3 c2 = { m[2], m[5], m[8] };
    vn[0] = vneg(c2); vn[1] = c2;     /* front(-z) / back(+z)  */
    vn[2] = vneg(c0); vn[3] = c0;     /* left(-x)  / right(+x) */
    vn[4] = c1;       vn[5] = vneg(c1);/* top(+y)   / bottom(-y)*/

    /* Light into object space: lobj = R_spin^T * LIGHT_DIR (normals are
     * object-space, so the light must be too — the GPU does not rotate them). */
    float ln = sqrtf(vdot(LIGHT_DIR, LIGHT_DIR));
    vec3 L = { LIGHT_DIR.x / ln, LIGHT_DIR.y / ln, LIGHT_DIR.z / ln };
    lobj->x = rs[0]*L.x + rs[3]*L.y + rs[6]*L.z;
    lobj->y = rs[1]*L.x + rs[4]*L.y + rs[7]*L.z;
    lobj->z = rs[2]*L.x + rs[5]*L.y + rs[8]*L.z;

    *rvc0 = (vec3){ rv[0], rv[3], rv[6] };
    *rvc2 = (vec3){ rv[2], rv[5], rv[8] };
}

/* ================================================================
 * Scene
 * ================================================================ */

/* Sticky surface state (0x4A) shared by the lit-vertex draws: truecolour
 * RGB565 framebuffer + brick texture + z-buffer + clip rect. */
static void bind_cube_surface(uint32_t fb_addr)
{
    of_gpu_tri_state_t st;
    memset(&st, 0, sizeof(st));
    st.fb_base       = fb_addr;
    st.fb_major_step = fb_stride;          /* bytes per row */
    st.fb_minor_step = FB_BPP;             /* RGB565 -> 2 bytes per pixel */
    st.tex_addr      = tex_fast ? of_texture_gpu_addr(&cube_th)
                                : (uint32_t)(uintptr_t)cube_tex;
    st.tex_width     = TEX_W;
    st.tex_w_mask    = TEX_W - 1;
    st.tex_h_mask    = TEX_H - 1;
    st.flags         = OF_GPU_SPAN_TRUECOLOR;   /* RGB565 direct-colour */
    st.colormap_id   = 0;
    st.clip_x0 = 0;  st.clip_x1 = (int16_t)vw;
    st.clip_y0 = 0;  st.clip_y1 = (int16_t)vh;
    if (has_zbuffer) {
        st.z_mode       = OF_GPU_PARAM_Z_TEST_WRITE;
        st.z_base       = (uint32_t)(uintptr_t)zbuf;
        st.z_major_step = vw * ZBUF_ELEM;
        st.z_minor_step = ZBUF_ELEM;
    } else {
        st.z_mode = OF_GPU_PARAM_Z_NONE;
    }
    of_gpu_set_tri_state(&st);
}

/* Screen-space textured quad for the HUD (truecolour, no z, full-bright vertex
 * colour so the fragment is just the HUD texel). */
static void emit_hud_quad(uint32_t fb_addr)
{
    of_gpu_tri_state_t st;
    memset(&st, 0, sizeof(st));
    st.fb_base       = fb_addr;
    st.fb_major_step = fb_stride;
    st.fb_minor_step = FB_BPP;
    st.tex_addr      = tex_fast ? of_texture_gpu_addr(&hud_th)
                                : (uint32_t)(uintptr_t)hud_tex;
    st.tex_width     = HUD_W;
    st.tex_w_mask    = HUD_W - 1;
    st.tex_h_mask    = HUD_H - 1;
    st.flags         = OF_GPU_SPAN_TRUECOLOR;
    st.z_mode        = OF_GPU_PARAM_Z_NONE;
    st.clip_x0 = 0;  st.clip_x1 = (int16_t)vw;
    st.clip_y0 = 0;  st.clip_y1 = (int16_t)vh;
    of_gpu_set_tri_state(&st);

    const int x0 = HUD_X, y0 = HUD_Y, x1 = HUD_X + HUD_W, y1 = HUD_Y + HUD_H;
    const int16_t X0 = (int16_t)(x0 * 16), X1 = (int16_t)(x1 * 16);  /* Q12.4 */
    const int32_t zc = 1 << 16;
    const uint16_t white[3] = { 0xFFFF, 0xFFFF, 0xFFFF };
    const int32_t  zi[3]    = { zc, zc, zc };
    const int32_t  depth[3] = { zc, zc, zc };

    /* tri 0: (x0,y0)(x1,y0)(x1,y1) ; tri 1: (x0,y0)(x1,y1)(x0,y1) */
    int16_t xa[3] = { X0, X1, X1 }, ya[3] = { (int16_t)y0, (int16_t)y0, (int16_t)y1 };
    int32_t sa[3] = { 0, HUD_W << 16, HUD_W << 16 };
    int32_t ta[3] = { 0, 0, HUD_H << 16 };
    of_gpu_draw_vert_tri_rgb(xa, ya, sa, ta, zi, white, 0, depth, NULL);

    int16_t xb[3] = { X0, X1, X0 }, yb[3] = { (int16_t)y0, (int16_t)y1, (int16_t)y1 };
    int32_t sb[3] = { 0, HUD_W << 16, 0 };
    int32_t tb[3] = { 0, HUD_H << 16, HUD_H << 16 };
    of_gpu_draw_vert_tri_rgb(xb, yb, sb, tb, zi, white, 0, depth, NULL);
}

static int build_frame(uint32_t fb_addr, float orbit, float spin)
{
    of_gpu_clear_rect_strided(fb_addr, (uint16_t)(vw * FB_BPP), (uint16_t)vh,
                              (uint16_t)fb_stride, 0x00);   /* black */
    if (has_zbuffer)
        of_gpu_clear_rect_strided((uint32_t)(uintptr_t)zbuf,
                                  (uint16_t)(vw * ZBUF_ELEM), (uint16_t)vh,
                                  (uint16_t)(vw * ZBUF_ELEM), 0);
    bind_cube_surface(fb_addr);

    float m[9];
    vec3 vn[6], lobj, rvc0, rvc2;
    build_geom(spin * 0.9f, spin * 0.6f, orbit, m, vn, &lobj, &rvc0, &rvc2);

    float half     = (grid_side - 1) * 0.5f * SPACING;
    float cam_dist = CAM_NEAR + half * CAM_K;
    float xslope   = (float)vw / (2.0f * focal);   /* on-screen if |x| < z*slope */
    float yslope   = (float)vh / (2.0f * focal);
    float cube_r   = HALF * 1.74f;                  /* bounding radius (margin)  */
    float near_z   = cube_r + 0.4f;                 /* keeps every vert in front */

    /* The two-triangle split of a face quad (corners 0,1,2 and 0,2,3). */
    static const int split[2][3] = { {0, 1, 2}, {0, 2, 3} };

    int tris = 0, drawn = 0;
    for (int j = 0; j < grid_side; ++j) {
        float gz = j * SPACING - half;
        for (int i = 0; i < grid_side; ++i) {
            float gx = i * SPACING - half;
            /* cube centre in camera space = R_view * (gx,0,gz) + (0,0,cam_dist) */
            vec3 c = { gx * rvc0.x + gz * rvc2.x,
                       gx * rvc0.y + gz * rvc2.y,
                       gx * rvc0.z + gz * rvc2.z + cam_dist };

            if (c.z < near_z)                                  continue;
            if (c.x < -(c.z * xslope + cube_r) ||
                c.x >  (c.z * xslope + cube_r))                continue;
            if (c.y < -(c.z * yslope + cube_r) ||
                c.y >  (c.z * yslope + cube_r))                continue;

            for (int f = 0; f < 6; ++f) {
                if (vdot(vn[f], c) >= -HALF)                   continue; /* backface */

                /* Flat lit shade for this whole face: ambient + clamp01(N.L)*light.
                 * Normals are object-space (MN[f]); lobj is the light rotated into
                 * object space (build_geom) so the dot is the same N.L the old GPU
                 * light state computed.  All six face vertices share it. */
                float ndotl = MN[f][0] * lobj.x + MN[f][1] * lobj.y
                            + MN[f][2] * lobj.z;
                uint16_t shade = flat_shade565(ndotl);

                /* Project the four face corners on the CPU: model->camera
                 * (shared rotation scaled by HALF + this cube's centre), then
                 * perspective.  Cache 1/w and the raw texel coords; zi/depth are
                 * formed per triangle so each gets its own perspective scale. */
                int16_t px[4], py[4];
                int32_t ps[4], pt[4];
                float   pwi[4], puf[4], pvf[4];
                for (int k = 0; k < 4; ++k) {
                    const float *mv = cube_v[cube_idx[f][k]];
                    float camx = HALF * (m[0]*mv[0] + m[1]*mv[1] + m[2]*mv[2]) + c.x;
                    float camy = HALF * (m[3]*mv[0] + m[4]*mv[1] + m[5]*mv[2]) + c.y;
                    float camz = HALF * (m[6]*mv[0] + m[7]*mv[1] + m[8]*mv[2]) + c.z;
                    float wi   = (camz > 0.0001f) ? 1.0f / camz : 0.0f;   /* 1/w */
                    float sx   = cx + focal * camx * wi;
                    float sy   = cy + focal * camy * wi;
                    if (sx < -2047.0f) sx = -2047.0f; else if (sx > 2047.0f) sx = 2047.0f;
                    if (sy < -2047.0f) sy = -2047.0f; else if (sy > 2047.0f) sy = 2047.0f;
                    px[k]  = (int16_t)lrintf(sx * 16.0f);   /* Q12.4 subpixel X */
                    py[k]  = (int16_t)lrintf(sy);           /* integer scanline (4A subpix_y=0) */
                    ps[k]  = UVF[k][0];                     /* raw texel u, Q16.16 (GPU forms s*zi) */
                    pt[k]  = UVF[k][1];                     /* raw texel v, Q16.16 */
                    pwi[k] = wi;
                    puf[k] = (float)UVF[k][0] * (1.0f / 65536.0f);  /* texel u in units */
                    pvf[k] = (float)UVF[k][1] * (1.0f / 65536.0f);
                }

                /* Two triangles per face. */
                for (int q = 0; q < 2; ++q) {
                    int16_t  x[3], y[3];
                    int32_t  s[3], t[3], zi[3], depth[3];
                    uint16_t rgb[3];

                    /* Per-triangle perspective scale: peak of {|1/w|,|u/w|,|v/w|}
                     * over the 3 verts -> zscale lands it at ZI_DERIVE_TARGET. */
                    float peak = 0.0f;
                    for (int k = 0; k < 3; ++k) {
                        int vi = split[q][k];
                        float a3 = fabsf(pwi[vi]);
                        float a4 = fabsf(puf[vi] * pwi[vi]);
                        float a5 = fabsf(pvf[vi] * pwi[vi]);
                        if (a3 > peak) peak = a3;
                        if (a4 > peak) peak = a4;
                        if (a5 > peak) peak = a5;
                    }
                    float zscale = (peak > 0.0f) ? (ZI_DERIVE_TARGET / peak) : ZI_SCALE;
                    if (zscale < 1.0f) zscale = 1.0f;       /* never shrink below 1:1 */

                    for (int k = 0; k < 3; ++k) {
                        int vi = split[q][k];
                        x[k] = px[vi];  y[k] = py[vi];
                        s[k] = ps[vi];  t[k] = pt[vi];

                        int z = (int)lrintf(pwi[vi] * (65536.0f * zscale));  /* (1/w)*K, Q16.16 */
                        if (z < 1) z = 1;
                        zi[k] = z;

                        /* Decoupled high-precision depth: 1/w * 2^30. */
                        float df = pwi[vi] * 1073741824.0f;
                        if (df < 1.0f) df = 1.0f;
                        else if (df > 2147483520.0f) df = 2147483520.0f;
                        depth[k] = (int32_t)lrintf(df);

                        rgb[k] = shade;                     /* flat: all verts share it */
                    }
                    of_gpu_draw_vert_tri_rgb(x, y, s, t, zi, rgb, 0u, depth, NULL);
                }
                tris += 2;
            }
            if ((++drawn & 7) == 0)
                of_gpu_kick();      /* keep the GPU busy while the CPU continues */
        }
    }

    if (show_hud)
        emit_hud_quad(fb_addr);
    return tris;
}

/* ================================================================
 * Bring-up + present loop
 * ================================================================ */

static void no_tl(const char *why)
{
    of_video_set_display_mode(OF_DISPLAY_TERMINAL);
    printf("\033[2J\033[H");
    printf("  triangles: this core lacks the vert-tri raster path\n");
    printf("  (CPU does the transform; the GPU just rasterises)\n");
    printf("  (%s)\n", why);
    printf("  Needs OF_HW_GPU_VERT_TRI + VCOLOR + PERSP\n");
    printf("  -- the os30 GPU bitstream.\n");
    for (;;) { of_input_poll_p0(); usleep(16 * 1000); }
}

static int probe_gpu(int draw_idx)
{
    uint32_t fb = (uint32_t)(uintptr_t)of_video_buffer_addr(draw_idx);
    of_gpu_clear_rect_strided(fb, (uint16_t)(vw * FB_BPP), (uint16_t)vh,
                              (uint16_t)fb_stride, 0x00);
    uint32_t token = of_gpu_submit();
    uint32_t t0 = of_time_us();
    while (!of_gpu_fence_reached(token)) {
        if ((uint32_t)(of_time_us() - t0) > 500000u) {
            of_gpu_debug_snapshot_t s; of_gpu_debug_snapshot(&s, 0);
            printf("[triangles] GPU probe STALL fence=%u reached=%u status=%08x\n",
                   (unsigned)token, (unsigned)s.fence_reached, (unsigned)s.status);
            return 0;
        }
    }
    return 1;
}

static void adopt_mode(void)
{
    of_video_mode_t cur;
    of_video_get_mode(&cur);
    vw = cur.width; vh = cur.height;
    fb_stride = cur.stride ? cur.stride : (cur.width * FB_BPP);
    focal = (float)vh * 1.25f;
    cx = (float)vw * 0.5f;
    cy = (float)vh * 0.5f;
    hires = (vw >= 640);
}

/* Request an RGB565 mode of the given size.  Returns 0 on failure. */
static int set_rgb565_mode(int want_hires)
{
    of_video_mode_t want, norm;
    memset(&want, 0, sizeof want);
    want.width      = want_hires ? 640 : 320;
    want.height     = want_hires ? 480 : 240;
    want.color_mode = OF_VIDEO_MODE_RGB565;
    if (of_video_check_mode(&want, &norm) != 0 || of_video_set_mode(&norm) != 0)
        return 0;
    adopt_mode();
    return 1;
}

/* d-pad LEFT/RIGHT select a resolution directly.  Returns 1 if it changed
 * (so the caller re-acquires the buffer rotation). */
static int set_resolution(int want_hires)
{
    if (want_hires == hires)
        return 0;
    if (!set_rgb565_mode(want_hires)) {
        printf("[triangles] %dx%d RGB565 not available on this core\n",
               want_hires ? 640 : 320, want_hires ? 480 : 240);
        return 0;
    }
    of_video_set_display_mode(OF_DISPLAY_FRAMEBUFFER);
    printf("[triangles] resolution -> %dx%d\n", vw, vh);
    return 1;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    cube_tex = malloc((size_t)TEX_W * TEX_H * FB_BPP);
    hud_tex  = malloc((size_t)HUD_W * HUD_H * FB_BPP);
    zbuf     = malloc((size_t)MAX_W * MAX_H * ZBUF_ELEM);
    if (!cube_tex || !hud_tex || !zbuf) {
        printf("[triangles] allocation failed\n");
        return 1;
    }

    hud_plate    = rgb565_888(brick_pal[PAL_HUD_PLATE]);
    hud_text_col = rgb565_888(brick_pal[PAL_HUD_TEXT]);
    load_texture();
    of_cache_flush_range(zbuf, (uint32_t)((size_t)MAX_W * MAX_H * ZBUF_ELEM));

    const struct of_capabilities *caps = of_get_caps();
    if (!(caps->hw_features & OF_HW_GPU_SPAN) || caps->gpu_base == 0)
        no_tl("no GPU span unit / gpu_base");

    of_gpu_init();

    /* CPU geometry feeds the os30 vert-tri raster: needs the raster + truecolour
     * + perspective, but NOT the XFORM front-end (os30 omits INCLUDE_XFORM). */
    const uint32_t need = OF_HW_GPU_VERT_TRI | OF_HW_GPU_VCOLOR | OF_HW_GPU_PERSP;
    if ((caps->hw_features & need) != need) {
        char buf[96];
        snprintf(buf, sizeof(buf), "VERT_TRI=%d VCOLOR=%d PERSP=%d",
                 of_has_feature(OF_HW_GPU_VERT_TRI),
                 of_has_feature(OF_HW_GPU_VCOLOR), of_has_feature(OF_HW_GPU_PERSP));
        no_tl(buf);
    }
    has_zbuffer = of_has_feature(OF_HW_GPU_PARAM_SPAN_ZTEST);

    if (!set_rgb565_mode(0))
        no_tl("RGB565 truecolour mode not available");

    int draw_idx = of_video_acquire_next(-1, 0);
    if (!probe_gpu(draw_idx))
        no_tl("GPU fence never retired (OS/bitstream issue, not the demo)");

    /* Texture residence (after the GPU is verified — of_texture_bind drains).
     * os30 has a dedicated fast-texture tier.  Use it for both RGB565 textures
     * so the GPU's fetch domain never flips mid-frame (a flip calls
     * of_gpu_finish(), which would break the CPU/GPU overlap).  Only take the
     * fast path if it ALL fits in one domain; otherwise fall back to the SDRAM
     * path used on cores without it. */
    of_texture_init();
    {
        uint32_t fast_need = (uint32_t)(TEX_W * TEX_H * FB_BPP)
                           + (uint32_t)(HUD_W * HUD_H * FB_BPP) + 64u;
        tex_fast = of_texture_has_fast_mem() &&
                   of_texture_budget_free() >= fast_need;
    }
    if (tex_fast) {
        for (int i = 0; i < HUD_W * HUD_H; ++i) hud_tex[i] = hud_plate;
        of_texture_create(&cube_th, cube_tex, TEX_W, TEX_H, TEX_W * TEX_H * FB_BPP);
        of_texture_create(&hud_th,  hud_tex,  HUD_W, HUD_H, HUD_W * HUD_H * FB_BPP);
        of_texture_bind(&cube_th);    /* route the fetch domain → fast (it stays) */
    }

    of_video_set_display_mode(OF_DISPLAY_FRAMEBUFFER);

    printf("[triangles] ready (CPU geom + HW vert-tri, %dx%d, z-buffer=%s, fast-tex=%s)\n",
           vw, vh, has_zbuffer ? "on" : "off", tex_fast ? "on" : "off");

    float orbit = 0.5f, spin = 0.0f;
    uint32_t stat_tick = of_time_us();

    while (1) {
        of_input_poll_p0();
        if (of_btn_pressed(OF_BTN_UP)    && grid_side < MAX_SIDE) grid_side++;
        if (of_btn_pressed(OF_BTN_DOWN)  && grid_side > MIN_SIDE) grid_side--;
        if (of_btn_pressed(OF_BTN_LEFT)  && set_resolution(0))    draw_idx = of_video_acquire_next(-1, 0);
        if (of_btn_pressed(OF_BTN_RIGHT) && set_resolution(1))    draw_idx = of_video_acquire_next(-1, 0);
        if (of_btn_pressed(OF_BTN_A))    paused = !paused;
        if (of_btn_pressed(OF_BTN_B))    show_hud = !show_hud;

        if (!paused) {
            orbit += 0.006f;
            spin  += 0.03f;
        }

        if (show_hud) {
            char l1[40], l2[40];
            snprintf(l1, sizeof(l1), "%dX%d %d CUBES", vw, vh, grid_side * grid_side);
            snprintf(l2, sizeof(l2), "%u.%u FPS %u TRI",
                     last_fps_x10 / 10, last_fps_x10 % 10, stat_tris_last);
            hud_update(l1, l2);
        }

        uint32_t fb_addr = (uint32_t)(uintptr_t)of_video_buffer_addr(draw_idx);
        stat_tris_last = (unsigned)build_frame(fb_addr, orbit, spin);

        uint32_t token = of_gpu_flip_to(draw_idx);
        of_gpu_kick();
        draw_idx = of_video_acquire_next(draw_idx, token);

        ++stat_frames;
        uint32_t now = of_time_us(), elapsed = now - stat_tick;
        if (elapsed >= 1000000) {
            last_fps_x10 = (unsigned)((uint64_t)stat_frames * 10000000ull / elapsed);
            stat_frames = 0;
            stat_tick = now;
        }
    }
}
