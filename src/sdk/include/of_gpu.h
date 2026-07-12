//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_gpu.h -- Hardware GPU Accelerator API for openfpgaOS
 *
 * Asynchronous span rasteriser.  CPU submits commands to a
 * 16 KB ring buffer in GPU-internal M10K BRAM; the GPU processes them
 * in parallel, writing pixels to the framebuffer via AXI4.
 *
 * Ring buffer: 16 KB in GPU-internal M10K BRAM.  CPU builds command
 * streams in a cached SDRAM scratch buffer, flushes and drains those
 * cache lines, then the GPU doorbell-DMA pulls the words into the ring
 * and publishes the write pointer atomically.  There is no CPU MMIO
 * command-data path.
 *
 * IMPORTANT: This header contains static mutable state (_gpu_wrptr, etc).
 * Include it from exactly ONE translation unit per program.
 */

#ifndef OF_GPU_H
#define OF_GPU_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <string.h>

#ifndef OF_PC
#include "of_caps.h"
#include "of_cache.h"
#endif

/* ================================================================
 * Constants
 * ================================================================ */

#define OF_GPU_CLEAR_COLOR      (1 << 0)
/* bit 1 reserved */

#define OF_GPU_RING_SIZE        16384   /* 16 KB M10K BRAM ring */

/* Fixed-point helpers */
#define OF_GPU_FIXED_16_16(x)   ((int32_t)((x) * 65536))   /* float → 16.16 */
#define OF_GPU_SUBPIXEL(x)      ((int16_t)((x) * 16))       /* pixel → 12.4  */

/* ================================================================
 * Span Flags
 * ================================================================ */

#define OF_GPU_SPAN_COLORMAP     (1 << 0)
#define OF_GPU_SPAN_BLEND        (1 << 1)   /* src-over const-alpha blend (truecolor; 0x4A ctl[1]); src alpha = 0x4A w16 */
#define OF_GPU_SPAN_SKIP_ZERO    (1 << 2)
/* bits 3/4 reserved */
#define OF_GPU_SPAN_PERSP        (1 << 5)
#define OF_GPU_SPAN_TRANSLUC     (1 << 6)
#define OF_GPU_SPAN_TRUECOLOR    (1 << 7)   /* RGB565 direct-color select (0x4A ctl[7]) */
/* Control-word bit 30 (not in the 8-bit flags byte): enables the full N64
 * combiner emulation clamp(texel*C + D).  C rides the per-vertex RGB565 words
 * 12-14 (signed 5b/ch); D rides the 22-word 0x4E words 19-21.  Set via
 * of_gpu_tri_state_t.cd_combine; OFF = byte-exact legacy texel*C. */
#define OF_GPU_SPAN_CD_COMBINE   (1u << 30)

/* ================================================================
 * Data Structures
 * ================================================================ */

typedef struct {
    uint8_t  lane_count;     /* 1..8; SDK splits to 4-lane native chunks */
    uint8_t  flags;          /* OF_GPU_SPAN_* shared by all lanes */
    uint8_t  reserved[2];
    uint16_t tex_width;
    uint16_t tex_w_mask;
    uint16_t tex_h_mask;
    int32_t  fb_step;        /* byte step per pixel inside each span */
    uint32_t fb_addr[8];
    uint32_t tex_addr[8];
    uint16_t count[8];
    int32_t  s[8], t[8];
    int32_t  sstep[8], tstep[8];
    uint8_t  light[8];       /* low 6 bits select palookup shade row */
    uint8_t  colormap_id[8]; /* explicit slot per lane, including slot 0 */
} of_gpu_affine_span_group_t;

/* Vertical 1-wide textured columns (CMD_DRAW_COLUMN_LIST / 0x4C).  A column
 * samples one texture column straight down, so the s (u) coordinate and its
 * per-pixel step are always 0 and are NOT carried on the wire — only t (v) and
 * tstep.  This is the bandwidth-optimised twin of of_gpu_affine_span_group():
 * the produced framebuffer is BYTE-IDENTICAL to that group with s[i]=0 and
 * sstep[i]=0, just with 5-word lane records instead of 7.
 *
 * fb_step is the byte step per pixel WITHIN each column (= stride for a
 * top-to-bottom wall column).  Lanes are independent: there is no implicit
 * per-lane fb_addr delta — each column carries its own fb_addr. */
typedef struct {
    uint8_t  lane_count;     /* 1..8; SDK splits to 4-lane native chunks */
    uint8_t  flags;          /* OF_GPU_SPAN_* shared by all lanes */
    uint8_t  reserved[2];
    uint16_t tex_width;
    uint16_t tex_w_mask;
    uint16_t tex_h_mask;
    int32_t  fb_step;        /* byte step per pixel down each column */
    uint32_t fb_addr[8];
    uint32_t tex_addr[8];
    uint16_t count[8];
    int32_t  t[8];           /* per-lane v origin (Q16.16) */
    int32_t  tstep[8];       /* per-lane v step per pixel (Q16.16) */
    uint8_t  light[8];       /* low 6 bits select palookup shade row */
    uint8_t  colormap_id[8]; /* explicit slot per lane, including slot 0 */
} of_gpu_column_list_group_t;

typedef struct {
    uint32_t fb_addr;
    uint32_t tex_addr;

    uint8_t  lane_count;     /* 1..8; SDK splits to 4-lane native chunks */
    uint8_t  flags;          /* OF_GPU_SPAN_* shared by generated spans */
    uint8_t  reserved;
    uint8_t  colormap_id;

    int32_t  major_fb_step;  /* byte step between adjacent lanes */
    int32_t  minor_fb_step;  /* byte step per pixel inside each span */

    uint16_t tex_width;
    uint16_t tex_w_mask;
    uint16_t tex_h_mask;

    int16_t  start[8];       /* per-lane minor start */
    uint16_t count[8];       /* per-lane pixel count */

    int32_t  sdivz, tdivz, zi_persp;
    int32_t  sdivz_major_step, tdivz_major_step, zi_major_step;
    int32_t  sdivz_minor_step, tdivz_minor_step, zi_minor_step;

    int32_t  light;          /* signed Q6.16, low 24 bits used */
    int32_t  light_major_step;
    int32_t  light_minor_step;
} of_gpu_persp_span_group_t;

enum {
    OF_GPU_PARAM_ATTR_AFFINE = 0,
    OF_GPU_PARAM_ATTR_PERSP  = 1,
    OF_GPU_PARAM_ATTR_SOLID  = 2,
    /*
     * Quake-oriented perspective plane format. attr0/attr1 carry the usual
     * baked numerator scaled by 2^13, while attr2 carries 1/z in Q3.29.
     * This preserves small per-pixel z gradients that vanish in Q16.16.
     */
    OF_GPU_PARAM_ATTR_PERSP_Q29 = 3
};

enum {
    OF_GPU_PARAM_AXIS_X = 0,
    OF_GPU_PARAM_AXIS_Y = 1
};

enum {
    OF_GPU_PARAM_RECORD_U16V16_COUNT16 = 0
};

enum {
    OF_GPU_PARAM_Z_NONE       = 0,
    OF_GPU_PARAM_Z_WRITE_ZI   = 1,
    OF_GPU_PARAM_Z_TEST_ZI    = 2,
    OF_GPU_PARAM_Z_TEST_WRITE = 3
};

typedef struct {
    uint16_t u;
    uint16_t v;
    uint16_t count;
} of_gpu_param_span_record_t;

typedef struct {
    uint32_t fb_base;
    int32_t  fb_major_step;
    int32_t  fb_minor_step;

    uint32_t tex_addr;
    uint16_t tex_width;
    uint16_t tex_w_mask;
    uint16_t tex_h_mask;

    uint8_t  flags;
    uint8_t  colormap_id;
    uint8_t  attr_mode;
    uint8_t  span_axis;
    uint8_t  z_mode;
    uint8_t  q29_attr_shift;
    uint8_t  reserved[2];

    int32_t  attr_origin[3];
    int32_t  attr_du[3];
    int32_t  attr_dv[3];

    int32_t  light_origin;
    int32_t  light_du;
    int32_t  light_dv;

    /* Per-axis texture coordinate clamp window (signed Q16.16); 0/0 =
     * clamp disabled for that axis.  CONTRACT: clamp_min[i] <=
     * clamp_max[i] (signed) per axis — the HW clamps the coordinate's
     * integer part with 16-bit top-half compares that are equivalent to
     * the full 32-bit clamp ONLY under this ordering; behavior is
     * UNDEFINED for min > max. */
    int32_t  clamp_min[3];
    int32_t  clamp_max[3];

    uint32_t z_base;
    int32_t  z_major_step;
    int32_t  z_minor_step;
} of_gpu_param_span_list_t;

typedef struct {
    uint32_t addr;
    uint16_t width;
    uint16_t height;
} of_gpu_texture_t;

/* Vertex for GPU_CMD_DRAW_PARAM_TRI (HW edge walker). */
typedef struct {
    int16_t x;   /* signed Q12.4 subpixel screen x */
    int16_t y;   /* signed integer scanline */
} of_gpu_tri_vert_t;

#define OF_GPU_PARAM_TRI_WORDS 36u
#define OF_GPU_PARAM_TRI_RECS_WORDS 16u

/* ================================================================
 * MMIO Registers
 * ================================================================ */

#ifndef OF_PC

/* GPU MMIO base. The kernel reports the per-target address via the
 * of_capabilities descriptor; of_gpu_init() reads it once and caches
 * it in _gpu_base, so the GPU register macros below dereference a
 * file-static variable instead of a hardcoded immediate. This is what
 * lets the same SDK app .elf run on a target whose GPU window sits
 * at a different CPU address. */
static uint32_t _gpu_base;

#define OF_GPU_REG(off)         (*(volatile uint32_t *)(_gpu_base + (off)))

#define GPU_CTRL                OF_GPU_REG(0x00)  /* W: bit0=enable, bit1=soft_reset, bit2=ring_reset */
#define GPU_RING_WRPTR          OF_GPU_REG(0x04)  /* R: published write pointer */
#define GPU_DMA_SRC             OF_GPU_REG(0x0C)  /* W: SDRAM byte address of command buffer to pull */
#define GPU_RING_RDPTR          OF_GPU_REG(0x10)  /* R: GPU read pointer */
#define GPU_STATUS              OF_GPU_REG(0x14)  /* R: bit6=DMA desc full, bit3=transluc busy, bit2=DMA busy, bit1=ring empty, bit0=busy */
#define GPU_FENCE_REACHED       OF_GPU_REG(0x18)  /* R: last completed fence token */
#define GPU_DMA_LEN             OF_GPU_REG(0x1C)  /* W: word count to pull (≤4096) */
#define GPU_TRANSLUC_ADDR       OF_GPU_REG(0x20)  /* W: byte addr into transluc[] (auto-inc by 4) */
#define GPU_TRANSLUC_DATA       OF_GPU_REG(0x24)  /* W: 32-bit word into transluc[] */
#define GPU_TEX_FLUSH           OF_GPU_REG(0x28)  /* W: flush texture cache */
#define GPU_DMA_KICK            OF_GPU_REG(0x2C)  /* W: write 1 to fire DMA pull from (SRC, LEN) */
#define GPU_PALOOKUP_BASE       OF_GPU_REG(0x30)  /* W/R: SDRAM byte base for palookup slots */
/* SDK-INTERNAL fast-texture-memory control.  Apps use of_texture.h and never
 * touch these — which memory backs "fast textures" (a dedicated sync-burst chip
 * on Pocket, nothing on MiSTer) is an implementation detail. */
#define _GPU_FAST_TEX_ENABLE    OF_GPU_REG(0x38)  /* bit0 = route tex-cache fills to fast mem */
#define _GPU_FAST_TEX_UP_ADDR   OF_GPU_REG(0x08)  /* fast-tex upload word pointer (auto-inc) */
#define _GPU_FAST_TEX_UP_DATA   OF_GPU_REG(0x3C)  /* W: upload data word; R bit0 = upload busy */

/* PASSIVE SDRAM channel-utilization counters (measurement only).  Free-running
 * 32-bit counters in the GPU read out through a single window: write the
 * counter index to GPU_CHANUTIL_SEL (reg13/byte0x34, an otherwise unused write
 * slot), then read the value from GPU_CHANUTIL_VAL (reg2/byte0x08 read; that
 * byte's WRITE side is the unrelated fast-tex upload pointer, untouched here).
 * Counters reset only on GPU reset, so the caller diffs against the previous
 * frame's snapshot; cnt_clk is the cycle denominator since this VexiiRiscv
 * build lacks rdcycle. */
#define GPU_CHANUTIL_SEL        OF_GPU_REG(0x34)  /* W: select which counter VAL returns (0..12) */
#define GPU_CHANUTIL_VAL        OF_GPU_REG(0x08)  /* R: selected free-running counter */

#define GPU_CHANUTIL_CLK        0u   /* cycle denominator */
#define GPU_CHANUTIL_BUSY_ANY   1u   /* read|write channel occupied (incl wait) */
#define GPU_CHANUTIL_XFER       2u   /* a real data beat moved */
#define GPU_CHANUTIL_WAIT       3u   /* valid && !ready: starved at the slave */
#define GPU_CHANUTIL_RD_Z       4u   /* read occupancy, z/FBSS source */
#define GPU_CHANUTIL_RD_TEX     5u   /* read occupancy, SDRAM tex source */
#define GPU_CHANUTIL_WR_Z       6u   /* write occupancy, z-write source */
#define GPU_CHANUTIL_WR_COLOR   7u   /* write occupancy, color-write source */

/* GPU_STATUS bit definitions */
#define GPU_STATUS_BUSY        0x1u
#define GPU_STATUS_RING_EMPTY  0x2u
#define GPU_STATUS_DMA_BUSY    0x4u  /* SDRAM command/payload DMA busy */
#define GPU_STATUS_TRANSLUC_BUSY 0x8u /* SRAM translucency LUT upload/lookup busy */
#define GPU_STATUS_DMA_DESC_FULL 0x40u /* command DMA descriptor FIFO full */

/* ================================================================
 * Command IDs
 * ================================================================ */

#define GPU_CMD_FENCE           0x02
#define GPU_CMD_CLEAR_RECT      0x11  /* 3-word payload: start byte addr,
                                       * {w,h}, {pad,color}. Color's low
                                       * byte is replicated 4× per FB word. */
#define GPU_CMD_SET_TEXTURE     0x20
#define GPU_CMD_SET_FB          0x23
/* GPU-triggered display flip (cr-gpu-triggered-flip.md).  2-word payload:
 *   word 0: bits[1:0] = back-buffer index (0/1/2 → FB_ADDR_{0,1,2})
 *   word 1: fence token (published to GPU_FENCE_REACHED after the swap)
 * Drains all outstanding m_wr_* writes (same primitive as the upgraded
 * CMD_FENCE), pulses the swap side-port to axi_periph_slave for one
 * cycle, then publishes the fence token. */
#define GPU_CMD_FLIP             0x42
#define GPU_CMD_DRAW_PARAM_SPAN_LIST 0x48   /* unified affine/persp span command */
#define GPU_CMD_SET_TRI_STATE        0x4A   /* sticky vert-tri surface state */
#define GPU_CMD_DRAW_VERT_TRI        0x4B   /* raw-vertex triangle, HW plane derive */
#define GPU_CMD_DRAW_PARAM_TRI       0x49   /* param-span header + 3 vertices;
                                             * HW edge walker generates records */
/* Command 0x49 is reserved; unsupported cores reject it as a no-op. */
#define GPU_CMD_DRAW_COLUMN_LIST     0x4C   /* vertical 1-wide textured columns:
                                             * same 4-word header as the 0x48
                                             * direct-affine variant, but a
                                             * 5-word lane record (drops the
                                             * always-0 s and sstep words).
                                             * Byte-identical pixels to a 0x48
                                             * column with s=0/sstep=0; ~28% less
                                             * command traffic.  Gate on
                                             * of_has_feature(OF_HW_GPU_COLUMN_LIST). */
#define GPU_CMD_DRAW_PARAM_TRI_RECS  0x4D   /* records-only 0x49 variant: 16-word
                                             * payload (12 plane words + q29 +
                                             * 3 vertices) on top of the 0x4A
                                             * sticky surface/control/clamp/z/
                                             * clip state.  Saves ~21 words per
                                             * triangle vs full 0x49.  Gate on
                                             * of_has_feature(OF_HW_GPU_PARAM_TRI_RECS). */
#define GPU_CMD_SET_OBJECT_STATE     0x50   /* sticky transform matrix (up to 5x4,
                                             * rows 0-2 cam, 3-4 s/t) + projection
                                             * consts + row-count/Q29/shift.  Used
                                             * on top of a 0x4A surface state and
                                             * feeds 0x51.  26-word payload.  Gate
                                             * (coupled) on OF_HW_GPU_VERT_TRI. */
#define GPU_CMD_DRAW_XFORM_TRI       0x51   /* raw model/world verts {x,y,z} (+ s/t
                                             * passthrough for N=3, + light) -> GPU
                                             * does M*v transform, perspective
                                             * project, plane derive, raster.
                                             * 16-word payload. */
#define GPU_CMD_DRAW_VERT_TRI_RGB    0x4E   /* truecolor sibling of 0x4B: per-vertex
                                             * RGB565 + decoupled depth. 19-word. */
#define GPU_CMD_DRAW_XFORM_TRI_RGB   0x52   /* truecolor sibling of 0x51: raw verts +
                                             * per-vertex RGB565. 18-word. */
#define GPU_CMD_LOAD_VERTS           0x53   /* transform one raw vert into a 5-bit GPU
                                             * vertex-cache slot. 7-word. */
#define GPU_CMD_DRAW_INDEXED_TRI     0x54   /* triangle from 3 cached slots. 1-word. */
#define GPU_CMD_SET_LIGHT_STATE      0x55   /* sticky single dir light + ambient for
                                             * 0x57 lit loads. 6-word. */
#define GPU_CMD_LOAD_VERT_LIT        0x57   /* transform+light one raw vert (object-
                                             * space normal) into a cache slot. 9-word. */
#define GPU_CMD_DRAW_CLIP_TRI        0x4F   /* clip-space feed: CPU sends M*v clip
                                             * {x,y,w} (Q16.16); GPU does ONLY
                                             * recip+project. 18-word, truecolor. */

#define OF_GPU_COLUMN_LIST_LANE_WORDS 5u
#define OF_GPU_COLUMN_LIST_MAX_NATIVE_LANES 4u
#define OF_GPU_COLUMN_LIST_MAX_LANES 8u
#define OF_GPU_COLUMN_LIST_WORDS(lanes) \
    (4u + ((uint32_t)(lanes) * OF_GPU_COLUMN_LIST_LANE_WORDS))

#define OF_GPU_PERSP_SPAN_GROUP_MAX_LANES 8u
#define OF_GPU_AFFINE_SPAN_GROUP_LANE_WORDS 7u
#define OF_GPU_AFFINE_SPAN_GROUP_MAX_NATIVE_LANES 4u
#define OF_GPU_AFFINE_SPAN_GROUP_MAX_LANES 8u

#define OF_GPU_PARAM_SPAN_LIST_HEADER_WORDS 31u
#define OF_GPU_PARAM_SPAN_LIST_RECORDS_PER_PAIR 2u
#define OF_GPU_PARAM_SPAN_LIST_RECORD_PAIR_WORDS 3u
#define OF_GPU_PARAM_SPAN_MAX_RECORDS 512u
#define OF_GPU_PARAM_SPAN_LIST_MAX_NATIVE_RECORDS OF_GPU_PARAM_SPAN_MAX_RECORDS
#define OF_GPU_PARAM_SPAN_LIST_WORDS(records) \
    (OF_GPU_PARAM_SPAN_LIST_HEADER_WORDS + \
     ((((uint32_t)(records) + 1u) >> 1) * \
      OF_GPU_PARAM_SPAN_LIST_RECORD_PAIR_WORDS))
#define OF_GPU_PARAM_DIRECT_AFFINE_WORDS(lanes) \
    (4u + \
     ((uint32_t)(lanes) * OF_GPU_AFFINE_SPAN_GROUP_LANE_WORDS))

/* ================================================================
 * Palookup (colormap) layout in SDRAM — must match gpu_core.v's
 * PALOOKUP_BASE / PALOOKUP_STRIDE constants.
 *
 * Each slot holds a Quake/BUILD-shape shade × texel table.  Scalar and
 * perspective span paths carry an explicit colormap_id in each command;
 * direct-affine records carry an explicit colormap_id per lane. Up to
 * 16 slots; the GPU reads
 * palookup[slot][shade & 63][texel] from
 *   GPU_AXI_BASE + OF_GPU_PALOOKUP_AXI_OFFSET
 *              + slot*0x4000 + shade*256 + texel
 * via gpu_tex_cache port B.
 *
 * These constants encode the GPU-side SDRAM offset and per-slot
 * stride.  The SDK adds caps->sdram_base or caps->sdram_uncached_base
 * at runtime to form the CPU-visible upload address.
 * ================================================================ */
/* Legacy GPU AXI M0 byte addr of palookup slot 0.  Current SDKs program
 * GPU_PALOOKUP_BASE to app-owned storage in of_gpu_init(); this fixed
 * offset remains only as a fallback for old cores / unusual init order. */
#define OF_GPU_PALOOKUP_AXI_OFFSET 0x03FC0000u
#define OF_GPU_PALOOKUP_STRIDE     0x00004000u  /* 16 KB per slot */
#define OF_GPU_PALOOKUP_SLOTS      16
#define OF_GPU_PALOOKUP_BYTES      (OF_GPU_PALOOKUP_STRIDE * OF_GPU_PALOOKUP_SLOTS)

/* Doorbell-DMA scratch region — must live in SDRAM because gpu_core's
 * m_rd_* AXI master only reaches the SDRAM arbiter (see core_top.v's
 * sdram_arb instantiation: GPU is m0, no other targets are wired).
 * The buffer is real app-owned storage, so the linker accounts for it
 * instead of relying on a hidden fixed SDRAM reservation.  CPU writes it
 * through the cached alias for speed.  The RTL DMA puller has a two-entry
 * descriptor FIFO, so the SDK alternates between two scratch buffers and
 * can build one command stream while the prior stream is still being copied
 * into ring BRAM.  Before each GPU DMA kick, of_gpu drains the flushed cache
 * lines with same-master readbacks so the GPU cannot read stale command
 * words. */
#define OF_GPU_BATCH_BUFFER_COUNT    2u
#define OF_GPU_BATCH_BUFFER_BYTES    0x00004000u  /* 16 KB per buffer */
#define OF_GPU_BATCH_BUF_BYTES       (OF_GPU_BATCH_BUFFER_COUNT * OF_GPU_BATCH_BUFFER_BYTES)
#define OF_GPU_CACHE_LINE_BYTES      64u

/* ================================================================
 * Ring Buffer State (app-side)
 *
 * Static mutable — include this header from one .c file only.
 * ================================================================ */

static uint32_t _gpu_wrptr;
static uint32_t _gpu_known_rdptr;
static uint32_t _gpu_fence_next;
static uint32_t _gpu_cmd_words;
static uint32_t _gpu_batch_dma_base;
static uint32_t _gpu_batch_dma_addr;
static uint32_t _gpu_batch_index;
static uint32_t _gpu_batch_inflight_mask;
/* Set when a fence/flip token has been staged but not yet flushed —
 * forces the next of_gpu_kick() to publish even below the lazy-kick
 * threshold, so every "stage token, kick, poll token" pattern stays
 * deadlock-free by construction. */
static uint32_t _gpu_unflushed_sync;
/* -1 = unresolved; 1 = publish commands via the uncached alias (MiSTer),
 * 0 = cached flush + drain (Pocket).  See _gpu_flush_cmd_stream. */
static int _gpu_publish_uncached = -1;

static const uint32_t _gpu_ring_mask = OF_GPU_RING_SIZE - 1;

/* Doorbell-DMA scratch buffer.  Kept cached so command construction does
 * not stall on every store.  NULL on targets that don't expose SDRAM;
 * command submission traps on those targets. */
static uint32_t _gpu_batch_storage[OF_GPU_BATCH_BUFFER_COUNT]
    [OF_GPU_BATCH_BUFFER_BYTES / sizeof(uint32_t)]
    __attribute__((aligned(OF_GPU_CACHE_LINE_BYTES)));
static uint32_t *_gpu_batch_buf_base;
static uint32_t *_gpu_batch_buf;
static uint8_t _gpu_palookup_storage[OF_GPU_PALOOKUP_BYTES]
    __attribute__((aligned(OF_GPU_PALOOKUP_STRIDE)));
static uint32_t _gpu_palookup_dma_base;
static uint32_t  _gpu_dbg_dma_waits;
static uint32_t  _gpu_dbg_dma_spin_iters;
static uint32_t  _gpu_dbg_ring_waits;
static uint32_t  _gpu_dbg_ring_spin_iters;
static uint32_t  _gpu_dbg_min_ring_free;

static uint32_t _gpu_state_valid;
static uint32_t _gpu_state_fb_addr;
static uint32_t _gpu_state_fb_stride;
static uint32_t _gpu_state_tex_addr;
static uint32_t _gpu_state_tex_dims;

#define OF_GPU_STATE_FB       (1u << 0)
#define OF_GPU_STATE_TEXTURE  (1u << 1)

#define OF_GPU_COMMAND_STREAM_BATCH_WORDS ((OF_GPU_RING_SIZE / 4u) - 1u)

#if (OF_GPU_COMMAND_STREAM_BATCH_WORDS * 4u) > OF_GPU_BATCH_BUFFER_BYTES
#error "GPU command stream buffer must fit in one reserved SDRAM scratch buffer"
#endif

#if OF_GPU_BATCH_BUFFER_COUNT != 2u
#error "GPU command staging assumes two scratch buffers"
#endif

/* ---- Internal helpers ---- */

static inline void _gpu_select_batch_buffer(uint32_t index) {
    _gpu_batch_index = index & 1u;
    _gpu_batch_dma_addr = _gpu_batch_dma_base +
        (_gpu_batch_index * OF_GPU_BATCH_BUFFER_BYTES);
    _gpu_batch_buf = _gpu_batch_buf_base +
        (_gpu_batch_index * (OF_GPU_BATCH_BUFFER_BYTES / sizeof(uint32_t)));
}

static inline void _gpu_wait_dma_idle_debug(void) {
    /* Bounded like of_gpu_wait: a wedged doorbell DMA otherwise spins
     * here forever with no diagnostic. */
    uint32_t dma_spins = 0;
    while (GPU_STATUS & GPU_STATUS_DMA_BUSY) {
        if (++dma_spins == 50000000u)
            __builtin_trap();  /* → fatal_trap dumps GPU state */
    }
    _gpu_batch_inflight_mask = 0;
    if (dma_spins) {
        _gpu_dbg_dma_waits++;
        _gpu_dbg_dma_spin_iters += dma_spins;
    }
}

static inline void _gpu_wait_dma_desc_slot_debug(void) {
    /* The descriptor FIFO is 2 deep and only our two staging buffers
     * ever occupy it: with at most one batch inflight a slot is free by
     * construction — skip the ~20-cycle uncached GPU_STATUS read. */
    if ((_gpu_batch_inflight_mask & (_gpu_batch_inflight_mask - 1u)) == 0u)
        return;

    uint32_t dma_spins = 0;
    while (GPU_STATUS & GPU_STATUS_DMA_DESC_FULL) {
        if (++dma_spins == 50000000u)
            __builtin_trap();  /* wedged DMA queue — see of_gpu_wait */
    }
    if (dma_spins) {
        _gpu_dbg_dma_waits++;
        _gpu_dbg_dma_spin_iters += dma_spins;
    }
}

static inline void _gpu_wait_transluc_idle(void) {
    while (GPU_STATUS & GPU_STATUS_TRANSLUC_BUSY) {
    }
}

static inline uint32_t _gpu_ring_free_now(void) {
    uint32_t rdptr = GPU_RING_RDPTR;
    _gpu_known_rdptr = rdptr;
    return (rdptr - _gpu_wrptr - 4) & _gpu_ring_mask;
}

static inline uint32_t _gpu_ring_free_known(void) {
    return (_gpu_known_rdptr - _gpu_wrptr - 4) & _gpu_ring_mask;
}

static inline void _gpu_note_ring_free(uint32_t ring_free) {
    if (ring_free < _gpu_dbg_min_ring_free)
        _gpu_dbg_min_ring_free = ring_free;
}

static inline void _gpu_cbo_flush_line(void *addr) {
    __asm__ volatile(".insn i 0x0F, 2, x0, %0, 2" :: "r"(addr) : "memory");
}

static inline void _gpu_flush_cmd_cache_range(void *addr, uint32_t bytes) {
    __asm__ volatile("fence" ::: "memory");
    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)(OF_GPU_CACHE_LINE_BYTES - 1u);
    uintptr_t end = (uintptr_t)addr + bytes;
    for (; a < end; a += OF_GPU_CACHE_LINE_BYTES)
        _gpu_cbo_flush_line((void *)a);
    __asm__ volatile("fence" ::: "memory");
}

static inline void _gpu_drain_cmd_writeback(uint32_t bytes) {
    if (bytes == 0)
        return;

    volatile const uint32_t *p = (volatile const uint32_t *)_gpu_batch_buf;
    uint32_t words = (bytes + 3u) >> 2;
    uint32_t sink = 0;

    /* of_cache_flush_range() invalidates each line after scheduling its
     * writeback.  These cached reads use the same d_axi master as those
     * writebacks, unlike a p_axi uncached read, so they force the CPU-side
     * memory stream to observe the flushed command data before GPU DMA
     * gets priority on the SDRAM arbiter. */
    for (uint32_t off = 0; off < bytes; off += OF_GPU_CACHE_LINE_BYTES)
        sink ^= p[off >> 2];
    sink ^= p[words - 1u];

    __asm__ volatile("" :: "r"(sink) : "memory");
    __asm__ volatile("fence" ::: "memory");
}

static inline void _gpu_flush_cmd_stream(void) {
    if (_gpu_cmd_words == 0)
        return;
    if (_gpu_batch_buf == NULL)
        __builtin_trap();

    uint32_t submit_words = _gpu_cmd_words;
    uint32_t submit_index = _gpu_batch_index;

    /* Publish the staged commands to DRAM — path is per-platform.
     *
     * MiSTer: the doorbell DMA has GPU-over-CPU priority at the SDRAM
     * arbiter, so a dirty line whose cbo.flush writeback hasn't physically
     * landed is read back by the GPU as the line's PREVIOUS content —
     * valid-looking command words from two submissions ago in the same
     * staging slot (screen-visible as stale replayed draws + contiguous
     * missing column runs; same failure class as the HW-confirmed
     * white-texel bug).  Uncached stores are ordered by construction:
     * each completes on its AXI B-response through the same
     * single-outstanding fabric the GPU's pull reads, so when the kick
     * lands every word is in SDRAM.  The cached copy stays valid for the
     * CPU (no invalidate), so emission reads/writes stay fast.
     *
     * Pocket: the arbiter doesn't starve the writeback, so the cached
     * cbo.flush + same-master readback drain is already correct there —
     * and far faster than re-writing every command word through the
     * single-outstanding uncached alias (thousands of serialized stores
     * per heavy frame = visible frame-rate loss on the handheld). */
    if (_gpu_publish_uncached < 0)
        _gpu_publish_uncached =
            (of_get_caps()->platform_id == OF_PLATFORM_MISTER);
    if (_gpu_publish_uncached) {
        volatile uint32_t *dst =
            (volatile uint32_t *)of_uncached(_gpu_batch_buf);
        for (uint32_t w = 0; w < submit_words; w++)
            dst[w] = _gpu_batch_buf[w];
        __asm__ volatile("fence" ::: "memory");
    } else {
        _gpu_flush_cmd_cache_range(_gpu_batch_buf, submit_words * 4u);
        _gpu_drain_cmd_writeback(submit_words * 4u);
    }
    _gpu_wait_dma_desc_slot_debug();

    /* The publish above is the data-visibility barrier.  The GPU MMIO
     * doorbell registers sit on a single in-order peripheral path, so
     * extra CPU fences between these volatile writes only add latency. */
    GPU_DMA_SRC  = _gpu_batch_dma_addr;
    GPU_DMA_LEN  = submit_words;
    GPU_DMA_KICK = 1;
    __asm__ volatile("" ::: "memory");

    _gpu_batch_inflight_mask |= (1u << submit_index);
    _gpu_cmd_words = 0;
    _gpu_unflushed_sync = 0;

    uint32_t next_index = submit_index ^ 1u;
    if (_gpu_batch_inflight_mask & (1u << next_index))
        _gpu_wait_dma_idle_debug();
    _gpu_select_batch_buffer(next_index);
}

static inline void _gpu_ring_ensure(uint32_t bytes) {
    if (bytes > (OF_GPU_RING_SIZE - 4u))
        __builtin_trap();

    uint32_t ring_free = _gpu_ring_free_known();
    _gpu_note_ring_free(ring_free);
    if (ring_free >= bytes)
        return;

    /* Slow path: if commands are only staged in SDRAM, first publish them
     * through the doorbell DMA so the GPU has something to drain. */
    _gpu_flush_cmd_stream();
    _gpu_wait_dma_idle_debug();

    {
        /* Bounded like of_gpu_wait: if the GPU stops draining the ring
         * (starved bus, wedged pipeline), the old unbounded spin froze
         * the machine here silently — the ring fills within one frame,
         * so a hang lands in this wait long before any fence wait. */
        uint32_t ring_spins = 0;
        do {
            ring_free = _gpu_ring_free_now();
            _gpu_note_ring_free(ring_free);
            if (ring_free >= bytes)
                break;
            if (++ring_spins == 50000000u)
                __builtin_trap();  /* → fatal_trap dumps GPU state */
        } while (1);
        _gpu_dbg_ring_waits++;
        _gpu_dbg_ring_spin_iters += ring_spins;
    }
}

static inline void _gpu_stream_reserve_words(uint32_t words) {
    if (words == 0)
        return;
    if (_gpu_batch_buf == NULL || words > OF_GPU_COMMAND_STREAM_BATCH_WORDS)
        __builtin_trap();
    if (_gpu_cmd_words + words > OF_GPU_COMMAND_STREAM_BATCH_WORDS)
        _gpu_flush_cmd_stream();
    _gpu_ring_ensure(words * 4u);
}

/* Append one word to the staged SDRAM command stream.  Callers reserve
 * a whole command first so this helper never flushes in the middle of a
 * command payload. */
static inline void _gpu_ring_write(uint32_t w) {
    if (_gpu_batch_buf == NULL || _gpu_cmd_words >= OF_GPU_COMMAND_STREAM_BATCH_WORDS)
        __builtin_trap();
    _gpu_batch_buf[_gpu_cmd_words++] = w;
    _gpu_wrptr = (_gpu_wrptr + 4) & _gpu_ring_mask;
}

/* Bulk emission fast path.  _gpu_cmd_header/_gpu_stream_reserve_words
 * already proved the buffer exists and the whole command fits, so the
 * per-word guards and bookkeeping in _gpu_ring_write are pure overhead
 * (~8 instructions/word) in the hot emitters.  claim() hands the caller
 * the raw staging cursor; the caller writes exactly the words it
 * reserved as plain sequential stores (~1 instruction/word) and
 * commit() settles the bookkeeping once per command.
 *
 * RULES: only legal inside a reserved region; the committed count must
 * equal the words actually written; never interleave with flushes. */
static inline uint32_t *_gpu_ring_claim(void) {
    return _gpu_batch_buf + _gpu_cmd_words;
}

static inline void _gpu_ring_commit(uint32_t words) {
    _gpu_cmd_words += words;
    _gpu_wrptr = (_gpu_wrptr + (words << 2)) & _gpu_ring_mask;
}

static inline void _gpu_cmd_header(uint8_t cmd, uint32_t payload_words) {
    _gpu_stream_reserve_words(1u + payload_words);
    _gpu_ring_write(((uint32_t)cmd << 24) | (payload_words & 0x00FFFFFF));
}

/* ================================================================
 * API Functions
 * ================================================================ */

static inline void of_gpu_init(void) {
    /* Resolve the GPU MMIO base from the runtime caps descriptor.
     * Must be called after main() (or after the SDK constructors run)
     * so _of_caps_ptr is populated. Apps that try to drive the GPU
     * before of_gpu_init() may write address 0, which is valid BRAM on
     * Pocket, so always initialize before touching GPU helpers or MMIO. */
    _gpu_base = of_get_caps()->gpu_base;

    _gpu_wrptr = 0;
    _gpu_known_rdptr = 0;
    _gpu_fence_next = 1;
    _gpu_cmd_words = 0;
    _gpu_batch_dma_base = 0;
    _gpu_batch_dma_addr = 0;
    _gpu_batch_index = 0;
    _gpu_batch_inflight_mask = 0;
    _gpu_batch_buf_base = NULL;
    _gpu_batch_buf = NULL;
    _gpu_palookup_dma_base = 0;
    _gpu_dbg_dma_waits = 0;
    _gpu_dbg_dma_spin_iters = 0;
    _gpu_dbg_ring_waits = 0;
    _gpu_dbg_ring_spin_iters = 0;
    _gpu_dbg_min_ring_free = OF_GPU_RING_SIZE;
    _gpu_state_valid = 0;
    GPU_CTRL = 6;               /* soft_reset | ring_reset */
    for (volatile int i = 0; i < 100; i++) {}
    GPU_CTRL = 4;               /* ring_reset: clear wr_addr + wrptr + rdptr */
    GPU_CTRL = 1;               /* enable */

    /* Command words are written through cached SDRAM for normal CPU store
     * speed.  _gpu_flush_cmd_stream() handles the external-master handoff
     * by flushing and then reading back the invalidated lines on the same
     * d_axi path before GPU_DMA_KICK. */
    {
        const struct of_capabilities *caps = of_get_caps();
        if (caps && caps->sdram_base != 0) {
            uint32_t base = (uint32_t)(uintptr_t)&_gpu_batch_storage[0][0];
            uint64_t lo = base;
            uint64_t hi = lo + OF_GPU_BATCH_BUF_BYTES;
            uint64_t sdram_lo = caps->sdram_base;
            uint64_t sdram_hi = sdram_lo + caps->sdram_size;

            if (lo < sdram_lo || hi > sdram_hi)
                __builtin_trap();

            _gpu_batch_dma_base = base;
            _gpu_batch_buf_base = &_gpu_batch_storage[0][0];
            _gpu_select_batch_buffer(0);

            uint32_t pal_base = (uint32_t)(uintptr_t)&_gpu_palookup_storage[0];
            uint64_t pal_lo = pal_base;
            uint64_t pal_hi = pal_lo + OF_GPU_PALOOKUP_BYTES;

            if ((pal_base & (OF_GPU_PALOOKUP_STRIDE - 1u)) != 0u ||
                pal_lo < sdram_lo || pal_hi > sdram_hi)
                __builtin_trap();

            _gpu_palookup_dma_base = pal_base;
            GPU_PALOOKUP_BASE = pal_base;
        }
    }

    GPU_TEX_FLUSH = 1;
}

/* Upload a palookup table to slot N in SDRAM.  The GPU reads palookup
 * bytes through gpu_tex_cache port B; this helper writes the table
 * directly through the uncached SDRAM alias so the GPU sees committed
 * data.  16 KB per slot, up to 16 slots (cf. OF_GPU_PALOOKUP_*).
 *
 * Span commands select slots explicitly with their colormap_id fields. */
static inline void of_gpu_palookup_upload(uint8_t slot, const uint8_t *data,
                                           uint32_t size) {
    if (slot >= OF_GPU_PALOOKUP_SLOTS || size > OF_GPU_PALOOKUP_STRIDE) return;
    const struct of_capabilities *caps = of_get_caps();
    if (caps->sdram_base == 0) return;  /* target without exposed SDRAM */
    /* Write through the uncached SDRAM alias so the bytes hit DRAM
     * directly — no D-cache pollution, no flush dependency.  The GPU's
     * tex_cache port B reads the same physical bytes via AXI; on the
     * first fill it sees the committed data unconditionally.
     *
     * Use 32-bit word writes, not single-byte writes.  Pocket's SDRAM
     * controller passes wstrb through, but the 16-bit PHY behind it
     * resolves byte-strobed writes via a read-modify-write path that
     * has produced "palookup mostly zeros" symptoms in practice — the
     * GPU then sees a near-uniform table and every pixel resolves to
     * the same palette index regardless of (texel, shade), which reads
     * on screen as a uniform colour with no fade.  Full-word writes
     * sidestep the RMW path; the SDRAM slave's [25:2] address decode
     * means consecutive uint32_t writes hit consecutive SDRAM words.
     *
     * The prior cached + cache_clean version had the same class of
     * stale-data bug; uncached alias is the right destination, just
     * in 32-bit chunks rather than bytes. */
    uint32_t cached_base = (_gpu_palookup_dma_base != 0)
                         ? _gpu_palookup_dma_base
                         : (caps->sdram_base + OF_GPU_PALOOKUP_AXI_OFFSET);
    cached_base += (uint32_t)slot * OF_GPU_PALOOKUP_STRIDE;
    volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)
        ((cached_base - caps->sdram_base) + caps->sdram_uncached_base);
    const uint8_t *src = data;
    uint32_t words = size >> 2;
    for (uint32_t i = 0; i < words; i++) {
        uint32_t off = i << 2;
        dst[i] = (uint32_t)src[off + 0] |
                 ((uint32_t)src[off + 1] << 8) |
                 ((uint32_t)src[off + 2] << 16) |
                 ((uint32_t)src[off + 3] << 24);
    }
    /* Tail bytes (size not a multiple of 4) — fold into a final word
     * so we still write every byte the caller passed.  Pad the unused
     * lanes with zero rather than skipping, so the SDRAM word is
     * fully defined regardless of whatever was there before. */
    uint32_t tail = size & 3u;
    if (tail) {
        uint32_t w = 0;
        const uint8_t *tb = src + (words << 2);
        for (uint32_t i = 0; i < tail; i++)
            w |= ((uint32_t)tb[i]) << (i * 8);
        dst[words] = w;
    }
}

/* Decimates BUILD's 64 KB transluc[256][256] to the fabric's 32 KB
 * / 128×256 quantised LUT (low bit of source axis dropped) during the
 * upload. */
static inline void of_gpu_translucency_upload(const uint8_t *table, uint32_t size) {
    if (size != 65536) return;
    GPU_TRANSLUC_ADDR = 0;
    for (int s7 = 0; s7 < 128; s7++) {
        const uint8_t *row = &table[(s7 << 1) << 8];
        const uint32_t *row32 = (const uint32_t *)row;
        for (int w = 0; w < 64; w++) {
            _gpu_wait_transluc_idle();
            GPU_TRANSLUC_DATA = row32[w];
        }
    }
    _gpu_wait_transluc_idle();
}

/* Lazy-kick threshold: below this much staged data an advisory kick is
 * a net loss — the flush pays a full cache clean + writeback drain +
 * fences + an uncached doorbell sequence regardless of size, and
 * engines that kick per-surface (hundreds of times a frame) were
 * spending more CPU on flush overhead than on the commands themselves.
 * A kick always publishes when a fence/flip token is staged, so any
 * caller that waits after kicking is unaffected. */
#ifndef OF_GPU_KICK_MIN_WORDS
#define OF_GPU_KICK_MIN_WORDS 512u
#endif

static inline void of_gpu_kick(void) {
    if (_gpu_cmd_words >= OF_GPU_KICK_MIN_WORDS || _gpu_unflushed_sync)
        _gpu_flush_cmd_stream();
}

/* Unconditional publish — for callers that need the staged stream on
 * its way NOW regardless of size (frame boundaries, before CPU FB
 * access, debug). */
static inline void of_gpu_kick_now(void) {
    _gpu_flush_cmd_stream();
}

static inline uint32_t of_gpu_fence(void) {
    uint32_t token = _gpu_fence_next++;
    _gpu_cmd_header(GPU_CMD_FENCE, 1);
    _gpu_ring_write(token);
    _gpu_unflushed_sync = 1;
    return token;
}

/* GPU-triggered page flip — emits CMD_FLIP into the ring with the
 * given back-buffer index and a fresh fence token.  The GPU's command
 * processor drains all outstanding m_wr_* writes, pulses the swap
 * side-port to the display controller (queued for next vsync), and
 * publishes the fence token to GPU_FENCE_REACHED.  Non-blocking — the
 * returned token proves the swap request reached the display
 * controller, not that the next vsync has presented it.
 *
 * Pair with the kernel's of_video_acquire_next(idx) to get the next
 * free draw buffer.  See docs/cr-gpu-triggered-flip.md for the
 * standard call pattern. */
/* CMD_FLIP re-enabled with diagnostic counters in place (2026-04-30).
 * The kernel side of_video_acquire_next() retains a bounded fence-wait
 * and only uses a CPU FB_SWAP_CTRL write as a timeout fallback, so a
 * healthy CMD_FLIP path stays non-blocking and does not double-kick
 * the same swap. */
static inline uint32_t of_gpu_flip_to(int idx) {
    uint32_t token = _gpu_fence_next++;
    _gpu_cmd_header(GPU_CMD_FLIP, 2);
    _gpu_ring_write((uint32_t)idx & 0x3u);
    _gpu_ring_write(token);
    _gpu_unflushed_sync = 1;
    return token;
}

static inline uint32_t of_gpu_submit(void) {
    uint32_t token = of_gpu_fence();
    of_gpu_kick();
    return token;
}

static inline int of_gpu_fence_reached(uint32_t token) {
    return (int32_t)(GPU_FENCE_REACHED - token) >= 0;
}

static inline void of_gpu_wait(uint32_t token) {
    /* Bounded spin — if the GPU hangs (fb_acc never flushes, tex cache
     * stuck in fill, pipeline deadlocked), the old unbounded spin
     * silently froze the machine with no diagnostic.  Timeout triggers
     * an illegal-instruction trap so fatal_trap dumps the GPU state;
     * the registers to inspect on the trap side are:
     *   GPU_STATUS    (0x14) — busy, ring_empty, DMA state/queue
     *   GPU_RING_RDPTR (0x10) — where the GPU last stopped fetching
     *
     * Uses a plain iteration counter rather than a cycle CSR: this
     * VexiiRiscv build is compiled without --performance-counters so
     * rdcycle / mcycle both trap illegal-instruction (mtval=0xc8002873
     * / 0xb8002873 observed).  Iteration count of 50M with a ~10-cycle
     * body gives roughly ~5 s of wait at 100 MHz — generous enough
     * that normal fence completions always beat it, tight enough that
     * a genuine hang surfaces quickly. */
    uint32_t spins = 50000000u;
    while (!of_gpu_fence_reached(token)) {
        if (--spins == 0) {
            __builtin_trap();  /* → illegal-instruction trap, mcause=2 */
        }
    }
}

static inline void of_gpu_finish(void) {
    of_gpu_wait(of_gpu_submit());
}

/* Engines that mix GPU rendering with direct CPU framebuffer access should
 * call this before reading from the framebuffer, or before CPU overlays that
 * must land after GPU-rendered pixels. */
static inline void of_gpu_prepare_framebuffer_for_cpu(void) {
    of_gpu_finish();
}

static inline void of_gpu_shutdown(void) {
    of_gpu_finish();
    GPU_CTRL = 0;
    _gpu_state_valid = 0;
}

typedef struct {
    uint32_t status;
    uint32_t rdptr;
    uint32_t wrptr;
    uint32_t fence_reached;
    uint32_t dma_waits;
    uint32_t dma_spin_iters;
    uint32_t ring_waits;
    uint32_t ring_spin_iters;
    uint32_t min_ring_free;
    uint32_t ring_free;
    /* PASSIVE SDRAM channel-utilization counters (free-running 32-bit, GPU
     * side; raw cumulative values — the caller diffs against the previous
     * frame's snapshot, with chan_clk as the per-frame cycle denominator). */
    uint32_t chan_clk;
    uint32_t chan_busy_any;
    uint32_t chan_xfer;
    uint32_t chan_wait;
    uint32_t chan_rd_z;
    uint32_t chan_rd_tex;
    uint32_t chan_wr_z;
    uint32_t chan_wr_color;
} of_gpu_debug_snapshot_t;

/* Read one free-running channel-utilization counter: select then read.  The
 * GPU MMIO path is in-order, so the read returns the just-selected counter. */
static inline uint32_t _of_gpu_chanutil_read(uint32_t which) {
    GPU_CHANUTIL_SEL = which;
    return GPU_CHANUTIL_VAL;
}

static inline void of_gpu_debug_snapshot(of_gpu_debug_snapshot_t *snap,
                                         int reset_wait_counters) {
    if (snap == NULL)
        return;

    memset(snap, 0, sizeof(*snap));
    snap->status = GPU_STATUS;
    snap->rdptr = GPU_RING_RDPTR;
    snap->wrptr = _gpu_wrptr;
    snap->fence_reached = GPU_FENCE_REACHED;

    snap->ring_free = _gpu_ring_free_now();
    snap->min_ring_free = _gpu_dbg_min_ring_free < snap->ring_free ?
        _gpu_dbg_min_ring_free : snap->ring_free;
    snap->dma_waits = _gpu_dbg_dma_waits;
    snap->dma_spin_iters = _gpu_dbg_dma_spin_iters;
    snap->ring_waits = _gpu_dbg_ring_waits;
    snap->ring_spin_iters = _gpu_dbg_ring_spin_iters;

    snap->chan_clk      = _of_gpu_chanutil_read(GPU_CHANUTIL_CLK);
    snap->chan_busy_any = _of_gpu_chanutil_read(GPU_CHANUTIL_BUSY_ANY);
    snap->chan_xfer     = _of_gpu_chanutil_read(GPU_CHANUTIL_XFER);
    snap->chan_wait     = _of_gpu_chanutil_read(GPU_CHANUTIL_WAIT);
    snap->chan_rd_z     = _of_gpu_chanutil_read(GPU_CHANUTIL_RD_Z);
    snap->chan_rd_tex   = _of_gpu_chanutil_read(GPU_CHANUTIL_RD_TEX);
    snap->chan_wr_z     = _of_gpu_chanutil_read(GPU_CHANUTIL_WR_Z);
    snap->chan_wr_color = _of_gpu_chanutil_read(GPU_CHANUTIL_WR_COLOR);

    if (reset_wait_counters) {
        _gpu_dbg_dma_waits = 0;
        _gpu_dbg_dma_spin_iters = 0;
        _gpu_dbg_ring_waits = 0;
        _gpu_dbg_ring_spin_iters = 0;
        _gpu_dbg_min_ring_free = snap->ring_free;
    }
}

/* ---- State commands ---- */

static inline void of_gpu_set_framebuffer(uint32_t addr, uint16_t stride) {
    if ((_gpu_state_valid & OF_GPU_STATE_FB) &&
        _gpu_state_fb_addr == addr &&
        _gpu_state_fb_stride == (uint32_t)stride)
        return;

    _gpu_cmd_header(GPU_CMD_SET_FB, 2);
    _gpu_ring_write(addr);
    _gpu_ring_write((uint32_t)stride);
    _gpu_state_fb_addr = addr;
    _gpu_state_fb_stride = (uint32_t)stride;
    _gpu_state_valid |= OF_GPU_STATE_FB;
}

/* ================================================================
 * SDK-INTERNAL fast-texture-memory primitives.  These are the only place that
 * knows the dedicated texture chip exists; of_texture.h drives them and apps
 * use of_texture.h.  (Names carry no hardware token on purpose.)
 * ================================================================ */

/* Route the GPU texture cache (and its colormap port) to fast memory
 * (enable=1) or SDRAM (enable=0).  GPU must be idle — flushes the tex cache.
 * No-op where there is no fast texture memory. */
static inline void _of_gpu_route_fast_tex(int enable) {
    _GPU_FAST_TEX_ENABLE = enable ? 1u : 0u;
    GPU_TEX_FLUSH = 1u;
}

/* Stream `nwords` 32-bit words into fast texture memory at byte offset
 * `byte_off` (hardware auto-increment; polls the busy bit so it can't overrun).
 * Load-time / GPU-idle. */
static inline void _of_gpu_fast_tex_upload(uint32_t byte_off,
                                           const uint32_t *data, uint32_t nwords) {
    _GPU_FAST_TEX_UP_ADDR = byte_off >> 2;        /* byte -> word address */
    for (uint32_t i = 0; i < nwords; i++) {
        while (_GPU_FAST_TEX_UP_DATA & 1u) { }    /* wait prior word drained */
        _GPU_FAST_TEX_UP_DATA = data[i];          /* kick word_wr + auto-inc */
    }
    while (_GPU_FAST_TEX_UP_DATA & 1u) { }        /* drain the last word */
}

static inline void of_gpu_bind_texture(const of_gpu_texture_t *tex) {
    uint32_t dims = ((uint32_t)tex->width << 16) | tex->height;
    if ((_gpu_state_valid & OF_GPU_STATE_TEXTURE) &&
        _gpu_state_tex_addr == tex->addr &&
        _gpu_state_tex_dims == dims)
        return;

    _gpu_cmd_header(GPU_CMD_SET_TEXTURE, 2);
    _gpu_ring_write(tex->addr);
    _gpu_ring_write(dims);
    _gpu_state_tex_addr = tex->addr;
    _gpu_state_tex_dims = dims;
    _gpu_state_valid |= OF_GPU_STATE_TEXTURE;
}

/* ---- Draw commands ---- */

/* Whole-FB clear.  flags bit 0 = clear color. */
static inline void of_gpu_clear(uint32_t flags, uint16_t color) {
    if ((flags & OF_GPU_CLEAR_COLOR) == 0)
        return;

    /* The old whole-FB clear was a fixed 320x200 contiguous write from
     * the current framebuffer base.  Keep that public behavior but encode
     * it as a normal rectangle clear so the RTL only has one clear path. */
    _gpu_cmd_header(GPU_CMD_CLEAR_RECT, 3);
    _gpu_ring_write(_gpu_state_fb_addr);
    _gpu_ring_write((320u << 16) | 200u);
    _gpu_ring_write((320u << 16) | ((uint32_t)color & 0xFFu));
}

/* Clear a rectangular region of the framebuffer to a constant color.
 * Caller computes the start byte address (fb_base + y*stride + x); the
 * GPU walks `h` rows × `w` bytes from there, advancing each row by the
 * active st_fb_stride.  Color's low byte is replicated 4× per FB word.
 * Word-aligned full-width strips
 * (letterbox / status bar) hit the 4-byte fast path; arbitrary x/w
 * paths byte-strobe the partial-word edges.  Used to retire the last
 * per-frame CPU memset(frameplace, …) categories — see
 * project_gpu_owns_framebuffer.md. */
static inline void of_gpu_clear_rect(uint32_t start_byte_addr,
                                      uint16_t w, uint16_t h,
                                      uint8_t color) {
    _gpu_cmd_header(GPU_CMD_CLEAR_RECT, 3);
    _gpu_ring_write(start_byte_addr);
    _gpu_ring_write(((uint32_t)w << 16) | (uint32_t)h);
    _gpu_ring_write((uint32_t)color);
}

/* Strided clear_rect — word 2 of the payload carries the row stride at
 * bits [31:16].  When stride==0 the GPU falls back to the SET_FB-
 * resident global stride (matches plain of_gpu_clear_rect).  See
 * docs/cr-gpu-clear-rect-stride.md for the rationale. */
static inline void of_gpu_clear_rect_strided(uint32_t start_byte_addr,
                                              uint16_t w, uint16_t h,
                                              uint16_t stride,
                                              uint8_t color) {
    _gpu_cmd_header(GPU_CMD_CLEAR_RECT, 3);
    _gpu_ring_write(start_byte_addr);
    _gpu_ring_write(((uint32_t)w << 16) | (uint32_t)h);
    _gpu_ring_write(((uint32_t)stride << 16) | (uint32_t)color);
}

static inline void
_gpu_emit_param_span_list(const of_gpu_param_span_list_t *p,
                          const of_gpu_param_span_record_t *records,
                          uint32_t record_count);

static inline uint32_t _gpu_affine_group_lane_count(uint32_t lane_count) {
    if (lane_count > OF_GPU_AFFINE_SPAN_GROUP_MAX_LANES)
        return OF_GPU_AFFINE_SPAN_GROUP_MAX_LANES;
    return lane_count;
}

static inline void
of_gpu_draw_affine_span_group(const of_gpu_affine_span_group_t *group) {
    uint32_t lane_count;

    if (group == NULL)
        return;
#ifndef OF_PC
    /* This emitter lowers to the 0x48 compact-direct lane form (4-word
     * header + 7-word lane records).  Bitstreams without it (Pocket OS30
     * lean Quake2 GPU) drain those payloads silently as no-ops, so the
     * SDK refuses here instead.  Callers on such cores lower to the
     * long-form of_gpu_draw_param_span_list(), which decodes on every
     * variant. */
    if (!of_has_feature(OF_HW_GPU_SPAN_GROUP))
        return;
#endif

    lane_count = _gpu_affine_group_lane_count(group->lane_count);
    if (lane_count == 0)
        return;

    for (uint32_t first = 0; first < lane_count;) {
        uint32_t chunk = lane_count - first;
        uint32_t any_pixels = 0;
        if (chunk > OF_GPU_AFFINE_SPAN_GROUP_MAX_NATIVE_LANES)
            chunk = OF_GPU_AFFINE_SPAN_GROUP_MAX_NATIVE_LANES;

        for (uint32_t i = 0; i < chunk; i++)
            any_pixels |= group->count[first + i];

        if (any_pixels != 0) {
            _gpu_cmd_header(GPU_CMD_DRAW_PARAM_SPAN_LIST,
                            OF_GPU_PARAM_DIRECT_AFFINE_WORDS(chunk));
            uint32_t *w = _gpu_ring_claim();
            *w++ = (chunk << 28) |
                   ((((uint32_t)group->flags & ~OF_GPU_SPAN_PERSP) & 0xFFu) << 20);
            *w++ = (uint32_t)group->tex_width;
            *w++ = ((uint32_t)group->tex_h_mask << 16) |
                   (uint32_t)group->tex_w_mask;
            *w++ = (uint32_t)group->fb_step;

            for (uint32_t lane = 0; lane < chunk; lane++) {
                uint32_t src = first + lane;
                *w++ = group->fb_addr[src];
                *w++ = group->tex_addr[src];
                *w++ = (((uint32_t)group->colormap_id[src] & 0x0Fu) << 28) |
                       (((uint32_t)group->light[src] & 0x3Fu) << 16) |
                       (uint32_t)group->count[src];
                *w++ = (uint32_t)group->s[src];
                *w++ = (uint32_t)group->t[src];
                *w++ = (uint32_t)group->sstep[src];
                *w++ = (uint32_t)group->tstep[src];
            }
            _gpu_ring_commit(4u + 7u * chunk);
        }

        first += chunk;
    }
}

static inline uint32_t _gpu_column_group_lane_count(uint32_t lane_count) {
    if (lane_count > OF_GPU_COLUMN_LIST_MAX_LANES)
        return OF_GPU_COLUMN_LIST_MAX_LANES;
    return lane_count;
}

/* Emit a CMD_DRAW_COLUMN_LIST (0x4C) group: vertical 1-wide textured columns
 * with 5-word lane records (no s/sstep — always 0 for columns).  Mirrors
 * of_gpu_draw_affine_span_group() exactly (same 4-word header, same chunking,
 * same colormap/light/count packing), just dropping the two constant-zero
 * words per lane.  The result is byte-identical to that group with s=0/sstep=0.
 *
 * Callers should gate availability on of_has_feature(OF_HW_GPU_COLUMN_LIST).
 * On cores without the bit, fall back to of_gpu_draw_affine_span_group()
 * (s/sstep=0) when OF_HW_GPU_SPAN_GROUP is set; on lean cores with NEITHER
 * bit (Pocket OS30) lower to the long-form param span list —
 * of_gpu_draw_param_span_list() with one {u,v,count} record per column and
 * s=0/sstep=0 plane terms — which decodes on every variant. */
static inline void
of_gpu_draw_column_list(const of_gpu_column_list_group_t *group) {
    uint32_t lane_count;

    if (group == NULL)
        return;
#ifndef OF_PC
    /* 0x4C shares the compact-span decode hardware; cores without
     * OF_HW_GPU_COLUMN_LIST (Pocket OS30) drain it silently as a no-op,
     * so the SDK refuses here instead.  See the fallback note above. */
    if (!of_has_feature(OF_HW_GPU_COLUMN_LIST))
        return;
#endif

    lane_count = _gpu_column_group_lane_count(group->lane_count);
    if (lane_count == 0)
        return;

    for (uint32_t first = 0; first < lane_count;) {
        uint32_t chunk = lane_count - first;
        uint32_t any_pixels = 0;
        if (chunk > OF_GPU_COLUMN_LIST_MAX_NATIVE_LANES)
            chunk = OF_GPU_COLUMN_LIST_MAX_NATIVE_LANES;

        for (uint32_t i = 0; i < chunk; i++)
            any_pixels |= group->count[first + i];

        if (any_pixels != 0) {
            _gpu_cmd_header(GPU_CMD_DRAW_COLUMN_LIST,
                            OF_GPU_COLUMN_LIST_WORDS(chunk));
            uint32_t *w = _gpu_ring_claim();
            /* Same 4-word header as the 0x48 direct-affine variant. */
            *w++ = (chunk << 28) |
                   ((((uint32_t)group->flags & ~OF_GPU_SPAN_PERSP) & 0xFFu) << 20);
            *w++ = (uint32_t)group->tex_width;
            *w++ = ((uint32_t)group->tex_h_mask << 16) |
                   (uint32_t)group->tex_w_mask;
            *w++ = (uint32_t)group->fb_step;

            for (uint32_t lane = 0; lane < chunk; lane++) {
                uint32_t src = first + lane;
                /* 5-word lane record: drop s (+3) and sstep (+5) vs 0x48. */
                *w++ = group->fb_addr[src];
                *w++ = group->tex_addr[src];
                *w++ = (((uint32_t)group->colormap_id[src] & 0x0Fu) << 28) |
                       (((uint32_t)group->light[src] & 0x3Fu) << 16) |
                       (uint32_t)group->count[src];
                *w++ = (uint32_t)group->t[src];
                *w++ = (uint32_t)group->tstep[src];
            }
            _gpu_ring_commit(4u + 5u * chunk);
        }

        first += chunk;
    }
}

/* Perspective variable-count adjacent span group.  The hardware native
 * command expands up to four clipped lanes into scalar perspective spans.
 * The public helper accepts up to eight lanes and splits wide submissions.
 *
 * For lane i:
 *   fb   = fb_addr + i*major_fb_step + start[i]*minor_fb_step
 *   s/z  = sdivz + i*sdivz_major_step + start[i]*sdivz_minor_step
 *   t/z  = tdivz + i*tdivz_major_step + start[i]*tdivz_minor_step
 *   1/z  = zi_persp + i*zi_major_step + start[i]*zi_minor_step
 * Then count[i] pixels are generated along minor_fb_step. */
static inline void
of_gpu_draw_persp_span_group(const of_gpu_persp_span_group_t *span) {
    if (span == NULL) return;

    uint32_t lanes_left = span->lane_count;
    if (lanes_left == 0)
        return;
    if (lanes_left > 8u)
        lanes_left = 8u;

    for (uint32_t first = 0; lanes_left != 0;) {
        uint32_t n = (lanes_left >= 4u) ? 4u : lanes_left;
        uint32_t live = 0;
        of_gpu_param_span_list_t p;
        of_gpu_param_span_record_t records[4];
        uint32_t fb_major = (uint32_t)span->major_fb_step * first;
        uint32_t sZ_major = (uint32_t)span->sdivz_major_step * first;
        uint32_t tZ_major = (uint32_t)span->tdivz_major_step * first;
        uint32_t zi_major = (uint32_t)span->zi_major_step * first;
        uint32_t light_major = (uint32_t)span->light_major_step * first;

        memset(&p, 0, sizeof(p));
        p.fb_base = span->fb_addr + fb_major;
        p.fb_major_step = span->major_fb_step;
        p.fb_minor_step = span->minor_fb_step;
        p.tex_addr = span->tex_addr;
        p.tex_width = span->tex_width;
        p.tex_w_mask = span->tex_w_mask;
        p.tex_h_mask = span->tex_h_mask;
        p.flags = span->flags | OF_GPU_SPAN_PERSP;
        p.colormap_id = span->colormap_id;
        p.attr_mode = OF_GPU_PARAM_ATTR_PERSP;
        p.span_axis = OF_GPU_PARAM_AXIS_X;
        p.attr_origin[0] = span->sdivz + sZ_major;
        p.attr_origin[1] = span->tdivz + tZ_major;
        p.attr_origin[2] = span->zi_persp + zi_major;
        p.attr_du[0] = span->sdivz_minor_step;
        p.attr_du[1] = span->tdivz_minor_step;
        p.attr_du[2] = span->zi_minor_step;
        p.attr_dv[0] = span->sdivz_major_step;
        p.attr_dv[1] = span->tdivz_major_step;
        p.attr_dv[2] = span->zi_major_step;
        p.light_origin = span->light + light_major;
        p.light_du = span->light_minor_step;
        p.light_dv = span->light_major_step;

        for (uint32_t i = 0; i < n; i++) {
            uint32_t src = first + i;
            records[i].u = (uint16_t)span->start[src];
            records[i].v = (uint16_t)i;
            records[i].count = span->count[src];
            live |= records[i].count;
        }
        if (live != 0)
            _gpu_emit_param_span_list(&p, records, n);
        first += n;
        lanes_left -= n;
    }
}

static inline void
of_gpu_draw_persp_span_group_batch(const of_gpu_persp_span_group_t *spans,
                                   int count) {
    if (count <= 0 || spans == NULL) return;

    for (int i = 0; i < count; i++)
        of_gpu_draw_persp_span_group(&spans[i]);
}

/* Words 0-30 shared by GPU_CMD_DRAW_PARAM_SPAN_LIST and
 * GPU_CMD_DRAW_PARAM_TRI: planes, control, clamps, z, record count and
 * the optional Q29 dynamic scale. */
static inline void
_gpu_emit_param_span_header_words(const of_gpu_param_span_list_t *p,
                                  uint32_t control,
                                  uint32_t record_count,
                                  uint32_t q29_attr_shift) {
    /* 31 words inside the caller's reservation — raw stores. */
    uint32_t *w = _gpu_ring_claim();
    *w++ = p->fb_base;
    *w++ = (uint32_t)p->fb_major_step;
    *w++ = (uint32_t)p->fb_minor_step;
    *w++ = p->tex_addr;
    *w++ = (uint32_t)p->tex_width;
    *w++ = (uint32_t)p->tex_w_mask;
    *w++ = (uint32_t)p->tex_h_mask;
    *w++ = control;

    for (uint32_t i = 0; i < 3; i++) {
        *w++ = (uint32_t)p->attr_origin[i];
        *w++ = (uint32_t)p->attr_du[i];
        *w++ = (uint32_t)p->attr_dv[i];
    }

    *w++ = (uint32_t)p->light_origin;
    *w++ = (uint32_t)p->light_du;
    *w++ = (uint32_t)p->light_dv;

    for (uint32_t i = 0; i < 3; i++) {
        *w++ = (uint32_t)p->clamp_min[i];
        *w++ = (uint32_t)p->clamp_max[i];
    }

    *w++ = p->z_base;
    *w++ = (uint32_t)p->z_major_step;
    *w++ = (uint32_t)p->z_minor_step;
    *w++ = record_count;
    *w++ = q29_attr_shift;
    _gpu_ring_commit(31u);
}

static inline void
_gpu_emit_param_span_list(const of_gpu_param_span_list_t *p,
                          const of_gpu_param_span_record_t *records,
                          uint32_t record_count) {
    uint32_t control;
    uint32_t q29_attr_shift = 0;

    if (record_count == 0)
        return;
    if (record_count > OF_GPU_PARAM_SPAN_MAX_RECORDS)
        record_count = OF_GPU_PARAM_SPAN_MAX_RECORDS;
    if (p->z_mode != OF_GPU_PARAM_Z_NONE
        && p->z_mode != OF_GPU_PARAM_Z_WRITE_ZI
        && p->z_mode != OF_GPU_PARAM_Z_TEST_ZI
        && p->z_mode != OF_GPU_PARAM_Z_TEST_WRITE)
        return;
    if (p->attr_mode == OF_GPU_PARAM_ATTR_PERSP_Q29) {
        q29_attr_shift = (uint32_t)p->q29_attr_shift & 31u;
#ifndef OF_PC
        if (q29_attr_shift != 0u
            && !of_has_feature(OF_HW_GPU_PARAM_SPAN_Q29_SCALE))
            return;
#endif
    }

    control = ((uint32_t)p->flags & 0xFFu)
            | (((uint32_t)p->colormap_id & 0x0Fu) << 8)
            | (((uint32_t)p->attr_mode & 0x0Fu) << 12)
            | (((uint32_t)p->span_axis & 0x0Fu) << 16)
            | ((uint32_t)OF_GPU_PARAM_RECORD_U16V16_COUNT16 << 20)
            | (((uint32_t)p->z_mode & 0x0Fu) << 24);

    _gpu_cmd_header(GPU_CMD_DRAW_PARAM_SPAN_LIST,
                    OF_GPU_PARAM_SPAN_LIST_WORDS(record_count));
    _gpu_emit_param_span_header_words(p, control, record_count,
                                      q29_attr_shift);

    {
        /* Record pairs as raw sequential stores; the odd tail pairs with
         * an implicit zero record (same wire bytes as before). */
        uint32_t *w = _gpu_ring_claim();
        uint32_t pairs = record_count >> 1;
        for (uint32_t i = 0; i < pairs; i++) {
            const of_gpu_param_span_record_t *a = &records[2u * i];
            const of_gpu_param_span_record_t *b = a + 1;
            *w++ = ((uint32_t)a->v << 16) | (uint32_t)a->u;
            *w++ = ((uint32_t)b->u << 16) | (uint32_t)a->count;
            *w++ = ((uint32_t)b->count << 16) | (uint32_t)b->v;
        }
        if (record_count & 1u) {
            const of_gpu_param_span_record_t *a = &records[record_count - 1u];
            *w++ = ((uint32_t)a->v << 16) | (uint32_t)a->u;
            *w++ = (uint32_t)a->count;
            *w++ = 0u;
        }
        _gpu_ring_commit(3u * ((record_count + 1u) >> 1));
    }
}

static inline void
of_gpu_draw_param_span_list(const of_gpu_param_span_list_t *params,
                            const of_gpu_param_span_record_t *records,
                            uint32_t record_count) {
    uint32_t any_pixels = 0;

    if (params == NULL || records == NULL || record_count == 0)
        return;
    if (record_count > OF_GPU_PARAM_SPAN_MAX_RECORDS)
        record_count = OF_GPU_PARAM_SPAN_MAX_RECORDS;

    for (uint32_t i = 0; i < record_count; i++)
        any_pixels |= records[i].count;

    if (any_pixels != 0)
        _gpu_emit_param_span_list(params, records, record_count);
}

/* GPU_CMD_DRAW_PARAM_TRI — hardware edge walker.
 * Same plane/control/z header as the span list, but the per-scanline
 * {u,v,count} records are generated by the GPU from three vertices.
 * Vertices: x is signed Q12.4 subpixel, y is a signed integer scanline.
 * The walker clips to [clip_x0, clip_x1) x [clip_y0, clip_y1) with ceil
 * fill on both edges (left-closed, right-open).  Gate on
 * OF_HW_GPU_PARAM_TRI before relying on this path. */
static inline void
of_gpu_draw_param_tri(const of_gpu_param_span_list_t *p,
                      const of_gpu_tri_vert_t v[3],
                      int16_t clip_x0, int16_t clip_x1,
                      int16_t clip_y0, int16_t clip_y1) {
    uint32_t control;
    uint32_t q29_attr_shift = 0;

    if (p == NULL || v == NULL)
        return;
    if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1)
        return;
    if (p->z_mode != OF_GPU_PARAM_Z_NONE
        && p->z_mode != OF_GPU_PARAM_Z_WRITE_ZI
        && p->z_mode != OF_GPU_PARAM_Z_TEST_ZI
        && p->z_mode != OF_GPU_PARAM_Z_TEST_WRITE)
        return;
#ifndef OF_PC
    if (!of_has_feature(OF_HW_GPU_PARAM_TRI))
        return;
#endif
    if (p->attr_mode == OF_GPU_PARAM_ATTR_PERSP_Q29) {
        q29_attr_shift = (uint32_t)p->q29_attr_shift & 31u;
#ifndef OF_PC
        if (q29_attr_shift != 0u
            && !of_has_feature(OF_HW_GPU_PARAM_SPAN_Q29_SCALE))
            return;
#endif
    }

    control = ((uint32_t)p->flags & 0xFFu)
            | (((uint32_t)p->colormap_id & 0x0Fu) << 8)
            | (((uint32_t)p->attr_mode & 0x0Fu) << 12)
            | (((uint32_t)p->span_axis & 0x0Fu) << 16)
            | ((uint32_t)OF_GPU_PARAM_RECORD_U16V16_COUNT16 << 20)
            | (((uint32_t)p->z_mode & 0x0Fu) << 24);

    _gpu_cmd_header(GPU_CMD_DRAW_PARAM_TRI, OF_GPU_PARAM_TRI_WORDS);
    _gpu_emit_param_span_header_words(p, control, 0u, q29_attr_shift);
    _gpu_ring_write(((uint32_t)(uint16_t)clip_x1 << 16)
                  | (uint32_t)(uint16_t)clip_x0);
    _gpu_ring_write(((uint32_t)(uint16_t)clip_y1 << 16)
                  | (uint32_t)(uint16_t)clip_y0);
    _gpu_ring_write(((uint32_t)(uint16_t)v[0].y << 16)
                  | (uint32_t)(uint16_t)v[0].x);
    _gpu_ring_write(((uint32_t)(uint16_t)v[1].y << 16)
                  | (uint32_t)(uint16_t)v[1].x);
    _gpu_ring_write(((uint32_t)(uint16_t)v[2].y << 16)
                  | (uint32_t)(uint16_t)v[2].x);
}

/* GPU_CMD_DRAW_PARAM_TRI_RECS — records-only param-tri (Quake2 world-pass
 * header dedup).  Same edge walker and fill convention as
 * of_gpu_draw_param_tri(), but only the per-triangle words travel on the
 * wire: the surface/control/clamp/z state AND the clip rect come from the
 * 0x4A sticky state (of_gpu_set_tri_state()), so 16 payload words replace
 * 36 — ~21 fewer words per triangle.
 *
 * Payload (matches gpu_core.v's 0x4D arm; w0..w11 land on the identical
 * 0x49 header arms idx 8..19, w12 on idx 30):
 *   w0..w8   attr planes: {origin, du, dv} x {attr0, attr1, attr2}
 *   w9..w11  light plane: origin, du, dv (low 24 bits used)
 *   w12      q29_attr_shift (same validation as 0x49 word 30)
 *   w13..w15 vertices, {y[31:16], x_Q12.4[15:0]} (same packing as 0x49)
 * Only p->attr_origin/attr_du/attr_dv, p->light_*, and (in Q29 mode)
 * p->q29_attr_shift are consumed; fb/tex/flags/colormap/z/clamp fields of
 * `p` are IGNORED — they were armed by the 0x4A.
 *
 * STICKY CONTRACT: REQUIRES a prior of_gpu_set_tri_state() (0x4A) that has
 * not been invalidated since — ANY 0x48/0x49/0x4C header overwrites the
 * shared staging and clears the sticky valid (even on lean variants that
 * drain the draw itself), so re-issue the 0x4A after such commands.  0x4D
 * itself does NOT invalidate: back-to-back 0x4D draws are the intended
 * fast path.  Without a valid sticky state the GPU retires the command as
 * a guarded no-op.
 *
 * Q29 note: w12 is validated against the sticky control word's Q29 arm.
 * Arm of_gpu_set_tri_state() with attr_q29 = 1 to use attr_mode ==
 * OF_GPU_PARAM_ATTR_PERSP_Q29 here (the shift is per-triangle in w12,
 * only the MODE bit is sticky); with the default attr_q29 = 0 sticky
 * state pass attr_mode == OF_GPU_PARAM_ATTR_PERSP (shift 0).  A nonzero
 * shift without a Q29-armed sticky control makes the RTL clear
 * header-supported (no-op, persisting until the next 0x4A). */
static inline void
of_gpu_draw_param_tri_recs(const of_gpu_param_span_list_t *p,
                           const of_gpu_tri_vert_t v[3]) {
    uint32_t q29_attr_shift = 0;

    if (p == NULL || v == NULL)
        return;
#ifndef OF_PC
    if (!of_has_feature(OF_HW_GPU_PARAM_TRI_RECS))
        return;
#endif
    if (p->attr_mode == OF_GPU_PARAM_ATTR_PERSP_Q29) {
        q29_attr_shift = (uint32_t)p->q29_attr_shift & 31u;
#ifndef OF_PC
        if (q29_attr_shift != 0u
            && !of_has_feature(OF_HW_GPU_PARAM_SPAN_Q29_SCALE))
            return;
#endif
    }

    _gpu_cmd_header(GPU_CMD_DRAW_PARAM_TRI_RECS, OF_GPU_PARAM_TRI_RECS_WORDS);
    {
        uint32_t *w = _gpu_ring_claim();
        for (uint32_t i = 0; i < 3; i++) {
            *w++ = (uint32_t)p->attr_origin[i];
            *w++ = (uint32_t)p->attr_du[i];
            *w++ = (uint32_t)p->attr_dv[i];
        }
        *w++ = (uint32_t)p->light_origin;
        *w++ = (uint32_t)p->light_du;
        *w++ = (uint32_t)p->light_dv;
        *w++ = q29_attr_shift;
        *w++ = ((uint32_t)(uint16_t)v[0].y << 16) | (uint32_t)(uint16_t)v[0].x;
        *w++ = ((uint32_t)(uint16_t)v[1].y << 16) | (uint32_t)(uint16_t)v[1].x;
        *w++ = ((uint32_t)(uint16_t)v[2].y << 16) | (uint32_t)(uint16_t)v[2].x;
        _gpu_ring_commit(OF_GPU_PARAM_TRI_RECS_WORDS);
    }
}

/* ================================================================
 * Hardware plane derivation (CMD_SET_TRI_STATE / CMD_DRAW_VERT_TRI)
 *
 * The GPU derives the four perspective planes (s*zi, t*zi, zi, light)
 * from raw per-vertex values — no client-side 2x2 solve, no bbox
 * rebase, no Q29 sliver escalation.  Set the sticky surface state once
 * per texture/skin, then stream 14-word triangles.  Gate on
 * of_has_feature(OF_HW_GPU_VERT_TRI); CMD_DRAW_PARAM_TRI (0x49) stays
 * as the fallback for bitstreams without it.
 * ================================================================ */

typedef struct {
    uint32_t fb_base;
    int32_t  fb_major_step;
    int32_t  fb_minor_step;

    uint32_t tex_addr;
    uint16_t tex_width;
    uint16_t tex_w_mask;
    uint16_t tex_h_mask;

    uint8_t  flags;          /* OF_GPU_SPAN_* (PERSP implied) */
    uint8_t  colormap_id;
    uint8_t  z_mode;         /* OF_GPU_PARAM_Z_* */

    int32_t  clamp_min[2];   /* s, t (0/0 = disabled, as 0x49).
                              * CONTRACT: min <= max per axis (signed
                              * Q16.16); the HW clamp compares only the
                              * top 16 bits, which matches the 32-bit
                              * clamp ONLY under this ordering — behavior
                              * is UNDEFINED for min > max. */
    int32_t  clamp_max[2];

    uint32_t z_base;
    int32_t  z_major_step;
    int32_t  z_minor_step;

    int16_t  clip_x0, clip_x1;   /* [x0, x1) x [y0, y1) */
    int16_t  clip_y0, clip_y1;

    uint8_t  attr_q29;       /* sticky attr mode: 0 = PERSP (default — the
                              * 0x4B derivation emits plain persp planes,
                              * so do NOT set this for the vert-tri path),
                              * 1 = PERSP_Q29 (for the 0x4D records path
                              * with CPU-solved Q29 planes; the per-
                              * triangle shift rides in 0x4D w12, only the
                              * MODE bit is sticky).  Zero-init keeps the
                              * pre-existing PERSP behavior. */

    /* Truecolor extras (0x4A control word bits 28/29 + word 16).  mirror_s/_t
     * drive G_TX_MIRROR addressing; const_alpha is the per-surface src alpha
     * (0..255) consumed when OF_GPU_SPAN_BLEND is set. */
    uint8_t  mirror_s, mirror_t;
    uint8_t  const_alpha;
    /* Full combiner emulation (control bit 30): when 1, the GPU computes
     * clamp(texel*C + D) — C = per-vertex RGB565 (signed 5b/ch) in the normal
     * rgb[] words, D = the rgb_d[] triple on the 22-word 0x4E.  0 = legacy. */
    uint8_t  cd_combine;
    /* Vert-tri vertex Y is Q12.4 subpixel instead of an integer scanline
     * (control bit 31, INCLUDE_DIRECT_COLOR-gated; decoded as data[31] in
     * gpu_core.v -> spanprod_subpix_y, consumed by gpu_edge_walker.v).
     * 0 = legacy integer Y. */
    uint8_t  subpix_y;
} of_gpu_tri_state_t;

/* 0x4A serves BOTH sticky-state consumers — 0x4B (HW plane derivation)
 * and 0x4D (records-only param tri) — and the RTL decodes it on every
 * variant, so accept it when either feature bit is set. */
static inline void of_gpu_set_tri_state(const of_gpu_tri_state_t *st) {
    if (st == NULL)
        return;
#ifndef OF_PC
    if (!of_has_feature(OF_HW_GPU_VERT_TRI)
        && !of_has_feature(OF_HW_GPU_PARAM_TRI_RECS))
        return;
#endif
    if (st->z_mode != OF_GPU_PARAM_Z_NONE
        && st->z_mode != OF_GPU_PARAM_Z_WRITE_ZI
        && st->z_mode != OF_GPU_PARAM_Z_TEST_ZI
        && st->z_mode != OF_GPU_PARAM_Z_TEST_WRITE)
        return;

    /* w6 control mirrors 0x49 header word 7 (bit 12 = persp, bit 13 =
     * q29); attr_mode is PERSP (or PERSP_Q29 when attr_q29 is set, for
     * the 0x4D records path) and the span axis X by construction. */
    uint32_t control = ((uint32_t)st->flags & 0xFFu)
                     | (((uint32_t)st->colormap_id & 0x0Fu) << 8)
                     | ((uint32_t)(st->attr_q29 ? OF_GPU_PARAM_ATTR_PERSP_Q29
                                                : OF_GPU_PARAM_ATTR_PERSP) << 12)
                     | ((uint32_t)OF_GPU_PARAM_AXIS_X << 16)
                     | ((uint32_t)OF_GPU_PARAM_RECORD_U16V16_COUNT16 << 20)
                     | (((uint32_t)st->z_mode & 0x0Fu) << 24)
                     | ((uint32_t)(st->mirror_s & 1u) << 28)   /* G_TX_MIRROR S (ctl[28]) */
                     | ((uint32_t)(st->mirror_t & 1u) << 29)   /* G_TX_MIRROR T (ctl[29]) */
                     | ((uint32_t)(st->cd_combine & 1u) << 30) /* texel*C+D combine (ctl[30]) */
                     | ((uint32_t)(st->subpix_y & 1u) << 31);  /* Q12.4 subpixel Y (ctl[31]) */

    /* 17-word 0x4A: word 16 carries the OF_GPU_SPAN_BLEND src alpha (RTL accepts
     * 16- or 17-word; const_alpha is ignored unless OF_GPU_SPAN_BLEND is set). */
    _gpu_cmd_header(GPU_CMD_SET_TRI_STATE, 17);
    uint32_t *w = _gpu_ring_claim();
    *w++ = st->fb_base;
    *w++ = (uint32_t)st->fb_major_step;
    *w++ = (uint32_t)st->fb_minor_step;
    *w++ = st->tex_addr;
    *w++ = (uint32_t)st->tex_width;
    *w++ = ((uint32_t)st->tex_h_mask << 16) | (uint32_t)st->tex_w_mask;
    *w++ = control;
    *w++ = (uint32_t)st->clamp_min[0];
    *w++ = (uint32_t)st->clamp_max[0];
    *w++ = (uint32_t)st->clamp_min[1];
    *w++ = (uint32_t)st->clamp_max[1];
    *w++ = st->z_base;
    *w++ = (uint32_t)st->z_major_step;
    *w++ = (uint32_t)st->z_minor_step;
    *w++ = ((uint32_t)(uint16_t)st->clip_x1 << 16) | (uint16_t)st->clip_x0;
    *w++ = ((uint32_t)(uint16_t)st->clip_y1 << 16) | (uint16_t)st->clip_y0;
    *w++ = (uint32_t)st->const_alpha;   /* w16: src alpha for OF_GPU_SPAN_BLEND */
    _gpu_ring_commit(17u);
}

/* One triangle: x[] in signed Q12.4 subpixels, y[] integer scanlines,
 * s/t/zi[] signed Q16.16 RAW per-vertex values (the GPU forms s*zi and
 * t*zi itself), light[] Q6 integer rows (0..63). */
static inline void of_gpu_draw_vert_tri(const int16_t x[3], const int16_t y[3],
                                        const int32_t s[3], const int32_t t[3],
                                        const int32_t zi[3],
                                        const uint8_t light[3]) {
#ifndef OF_PC
    if (!of_has_feature(OF_HW_GPU_VERT_TRI))
        return;
#endif
    _gpu_cmd_header(GPU_CMD_DRAW_VERT_TRI, 14);
    uint32_t *w = _gpu_ring_claim();
    *w++ = ((uint32_t)(uint16_t)y[0] << 16) | (uint16_t)x[0];
    *w++ = ((uint32_t)(uint16_t)y[1] << 16) | (uint16_t)x[1];
    *w++ = ((uint32_t)(uint16_t)y[2] << 16) | (uint16_t)x[2];
    *w++ = (uint32_t)s[0]; *w++ = (uint32_t)s[1]; *w++ = (uint32_t)s[2];
    *w++ = (uint32_t)t[0]; *w++ = (uint32_t)t[1]; *w++ = (uint32_t)t[2];
    *w++ = (uint32_t)zi[0]; *w++ = (uint32_t)zi[1]; *w++ = (uint32_t)zi[2];
    *w++ = (((uint32_t)light[2] & 0x3Fu) << 12)
         | (((uint32_t)light[1] & 0x3Fu) << 6)
         |  ((uint32_t)light[0] & 0x3Fu);
    *w++ = 0u;
    _gpu_ring_commit(14u);
}

/* ---- CMD_SET_OBJECT_STATE (0x50) + CMD_DRAW_XFORM_TRI (0x51) --------------
 * The GPU transform front-end: the CPU hands RAW model/world verts + a sticky
 * matrix instead of screen-space verts, and the GPU does M*v -> perspective
 * project -> plane derive -> raster.  Requires a preceding of_gpu_set_tri_state()
 * (0x4A) for the surface/clip/z state (same as 0x4B).  rows==3 is the alias path
 * (3x4 model matrix, s/t passthrough, Q16.16); rows==5 is the world path (rows
 * 3-4 = texvecs computing s/t, q29_en + q29_shift for the world-magnitude derive). */
typedef struct {
    int32_t  matrix[5][4];   /* row-major Q16.16; rows 0-2 cam, 3-4 s/t (N=5) */
    int32_t  xcenter, ycenter;   /* screen center, pixels */
    int32_t  xscale, yscale;     /* pixel scale */
    int32_t  near_clip;          /* Q16.16 min cam.z */
    uint8_t  rows;               /* 3 (alias) or 5 (world) */
    uint8_t  q29_en;             /* world: Q29-scale the derived planes */
    uint8_t  q29_shift;          /* CPU conservative magnitude shift (0..31) */
} of_gpu_object_state_t;

static inline void of_gpu_set_object_state(const of_gpu_object_state_t *st) {
    if (st == NULL)
        return;
#ifndef OF_PC
    if (!of_has_feature(OF_HW_GPU_VERT_TRI))   /* transform is coupled to vert-tri */
        return;
#endif
    _gpu_cmd_header(GPU_CMD_SET_OBJECT_STATE, 26);
    uint32_t *w = _gpu_ring_claim();
    int i, j;
    for (i = 0; i < 5; i++)
        for (j = 0; j < 4; j++)
            *w++ = (uint32_t)st->matrix[i][j];
    *w++ = (uint32_t)st->xcenter;
    *w++ = (uint32_t)st->ycenter;
    *w++ = (uint32_t)st->xscale;
    *w++ = (uint32_t)st->yscale;
    *w++ = (uint32_t)st->near_clip;
    *w++ = ((uint32_t)st->rows & 7u)
         | ((st->q29_en ? 1u : 0u) << 3)
         | (((uint32_t)st->q29_shift & 31u) << 4);
    _gpu_ring_commit(26u);
}

/* One transform triangle: vx/vy/vz[] signed Q16.16 model/world coords; s/t[]
 * Q16.16 raw texels (used only when rows==3; ignored for rows==5 which computes
 * them from the matrix); light[] Q6 rows (0..63). */
static inline void of_gpu_draw_xform_tri(const int32_t vx[3], const int32_t vy[3],
                                         const int32_t vz[3], const int32_t s[3],
                                         const int32_t t[3], const uint8_t light[3]) {
#ifndef OF_PC
    if (!of_has_feature(OF_HW_GPU_VERT_TRI))
        return;
#endif
    _gpu_cmd_header(GPU_CMD_DRAW_XFORM_TRI, 16);
    uint32_t *w = _gpu_ring_claim();
    *w++ = (uint32_t)vx[0]; *w++ = (uint32_t)vy[0]; *w++ = (uint32_t)vz[0];
    *w++ = (uint32_t)vx[1]; *w++ = (uint32_t)vy[1]; *w++ = (uint32_t)vz[1];
    *w++ = (uint32_t)vx[2]; *w++ = (uint32_t)vy[2]; *w++ = (uint32_t)vz[2];
    *w++ = (uint32_t)s[0]; *w++ = (uint32_t)s[1]; *w++ = (uint32_t)s[2];
    *w++ = (uint32_t)t[0]; *w++ = (uint32_t)t[1]; *w++ = (uint32_t)t[2];
    *w++ = (((uint32_t)light[2] & 0x3Fu) << 12)
         | (((uint32_t)light[1] & 0x3Fu) << 6)
         |  ((uint32_t)light[0] & 0x3Fu);
    _gpu_ring_commit(16u);
}

/* RGB565 per-vertex truecolour sibling of of_gpu_draw_vert_tri (0x4B):
 * w0-11 identical (verts/s/t/zi); w12-14 carry per-vertex RGB565 instead of
 * the packed light word; w15 is the per-triangle Q29 override; w16-18 carry
 * the decoupled high-range depth (1/w * 2^30).  Gate on the truecolour pair
 * OF_HW_GPU_VERT_TRI && OF_HW_GPU_VCOLOR (the GPU only honours w12-14 when the
 * RGB565 direct-colour path is built). */
static inline void of_gpu_draw_vert_tri_rgb(const int16_t x[3], const int16_t y[3],
                                            const int32_t s[3], const int32_t t[3],
                                            const int32_t zi[3], const uint16_t rgb[3],
                                            uint32_t q29, const int32_t depth[3],
                                            const uint16_t *rgb_d /* NULL=19-word legacy */) {
#ifndef OF_PC
    if (!of_has_feature(OF_HW_GPU_VERT_TRI) || !of_has_feature(OF_HW_GPU_VCOLOR))
        return;
#endif
    uint32_t n = rgb_d ? 19u : 17u;  /* shrunk: dead q29 word dropped, 3 RGB565 packed -> 2 words */
    (void)q29;                        /* q29 kept in the signature for ABI, no longer emitted */
    _gpu_cmd_header(GPU_CMD_DRAW_VERT_TRI_RGB, n);
    uint32_t *w = _gpu_ring_claim();
    *w++ = ((uint32_t)(uint16_t)y[0] << 16) | (uint16_t)x[0];
    *w++ = ((uint32_t)(uint16_t)y[1] << 16) | (uint16_t)x[1];
    *w++ = ((uint32_t)(uint16_t)y[2] << 16) | (uint16_t)x[2];
    *w++ = (uint32_t)s[0]; *w++ = (uint32_t)s[1]; *w++ = (uint32_t)s[2];
    *w++ = (uint32_t)t[0]; *w++ = (uint32_t)t[1]; *w++ = (uint32_t)t[2];
    *w++ = (uint32_t)zi[0]; *w++ = (uint32_t)zi[1]; *w++ = (uint32_t)zi[2];
    /* w12-13: three RGB565 colours packed into 2 words (rgb1<<16|rgb0, then rgb2). */
    *w++ = ((uint32_t)rgb[1] << 16) | (uint32_t)rgb[0]; *w++ = (uint32_t)rgb[2];
    *w++ = (uint32_t)depth[0]; *w++ = (uint32_t)depth[1]; *w++ = (uint32_t)depth[2];
    /* w17-18: per-vertex additive D (RGB565) packed 3->2 — only on the 19-word combine path. */
    if (rgb_d) { *w++ = ((uint32_t)rgb_d[1] << 16) | (uint32_t)rgb_d[0]; *w++ = (uint32_t)rgb_d[2]; }
    _gpu_ring_commit(n);
}

/* Truecolour sibling of of_gpu_draw_xform_tri (0x51): w0-14 identical (raw
 * model/world verts + s/t), then three RGB565 colours w15-17 replacing the
 * single packed-light word.  The GPU does M*v + project + plane derive on the
 * raw verts.  Gate on the xform-rgb cluster (OF_HW_GPU_XFORM_RGB) plus the
 * truecolour pair. */
static inline void of_gpu_draw_xform_tri_rgb(const int32_t vx[3], const int32_t vy[3],
                                             const int32_t vz[3], const int32_t s[3],
                                             const int32_t t[3], const uint16_t rgb[3]) {
#ifndef OF_PC
    if (!of_has_feature(OF_HW_GPU_VERT_TRI) || !of_has_feature(OF_HW_GPU_VCOLOR) ||
        !of_has_feature(OF_HW_GPU_XFORM_RGB))
        return;
#endif
    _gpu_cmd_header(GPU_CMD_DRAW_XFORM_TRI_RGB, 18);
    uint32_t *w = _gpu_ring_claim();
    *w++ = (uint32_t)vx[0]; *w++ = (uint32_t)vy[0]; *w++ = (uint32_t)vz[0];
    *w++ = (uint32_t)vx[1]; *w++ = (uint32_t)vy[1]; *w++ = (uint32_t)vz[1];
    *w++ = (uint32_t)vx[2]; *w++ = (uint32_t)vy[2]; *w++ = (uint32_t)vz[2];
    *w++ = (uint32_t)s[0]; *w++ = (uint32_t)s[1]; *w++ = (uint32_t)s[2];
    *w++ = (uint32_t)t[0]; *w++ = (uint32_t)t[1]; *w++ = (uint32_t)t[2];
    *w++ = (uint32_t)rgb[0]; *w++ = (uint32_t)rgb[1]; *w++ = (uint32_t)rgb[2];
    _gpu_ring_commit(18u);
}

/* Clip-space feed (0x4F): the CPU already did M*v, so it sends the clip {x,y,w}
 * (Q16.16) directly; the GPU does ONLY the perspective divide + viewport project
 * (no matrix MAC).  Wire-identical to 0x52.  Per-vertex RGB565.  Needs a preceding
 * sticky viewport (xc/yc/xscale/yscale/nearclip via of_gpu_set_object_state) and
 * a 0x4A surface state. */
static inline void of_gpu_draw_clip_tri(const int32_t cx[3], const int32_t cy[3],
                                        const int32_t cw[3], const int32_t s[3],
                                        const int32_t t[3], const uint16_t rgb[3]) {
#ifndef OF_PC
    if (!of_has_feature(OF_HW_GPU_VERT_TRI) || !of_has_feature(OF_HW_GPU_VCOLOR))
        return;
#endif
    _gpu_cmd_header(GPU_CMD_DRAW_CLIP_TRI, 18);
    uint32_t *w = _gpu_ring_claim();
    *w++ = (uint32_t)cx[0]; *w++ = (uint32_t)cy[0]; *w++ = (uint32_t)cw[0];
    *w++ = (uint32_t)cx[1]; *w++ = (uint32_t)cy[1]; *w++ = (uint32_t)cw[1];
    *w++ = (uint32_t)cx[2]; *w++ = (uint32_t)cy[2]; *w++ = (uint32_t)cw[2];
    *w++ = (uint32_t)s[0]; *w++ = (uint32_t)s[1]; *w++ = (uint32_t)s[2];
    *w++ = (uint32_t)t[0]; *w++ = (uint32_t)t[1]; *w++ = (uint32_t)t[2];
    *w++ = (uint32_t)rgb[0]; *w++ = (uint32_t)rgb[1]; *w++ = (uint32_t)rgb[2];
    _gpu_ring_commit(18u);
}

/* Transform-once/draw-many: load ONE raw vert {x,y,z}+{s,t}+RGB565 into GPU
 * vertex-cache slot (5-bit).  The GPU transforms it via the sticky 0x50 matrix
 * and parks the projected result; later 0x54 indexed-tris reference the slot.
 * Cluster-gated (OF_HW_GPU_XFORM_RGB). */
static inline void of_gpu_load_vert(uint8_t slot, int32_t vx, int32_t vy, int32_t vz,
                                    int32_t s, int32_t t, uint16_t rgb) {
#ifndef OF_PC
    if (!of_has_feature(OF_HW_GPU_VERT_TRI) || !of_has_feature(OF_HW_GPU_XFORM_RGB))
        return;
#endif
    _gpu_cmd_header(GPU_CMD_LOAD_VERTS, 7);
    uint32_t *w = _gpu_ring_claim();
    *w++ = (uint32_t)(slot & 0x1Fu);
    *w++ = (uint32_t)vx;
    *w++ = (uint32_t)vy;
    *w++ = (uint32_t)vz;
    *w++ = (uint32_t)s;
    *w++ = (uint32_t)t;
    *w++ = (uint32_t)rgb;
    _gpu_ring_commit(7u);
}

/* Draw a triangle from three previously-loaded vertex-cache slots (0x53/0x57).
 * One word packs three 5-bit indices.  Cluster-gated (OF_HW_GPU_XFORM_RGB). */
static inline void of_gpu_draw_indexed_tri(uint8_t i0, uint8_t i1, uint8_t i2) {
#ifndef OF_PC
    if (!of_has_feature(OF_HW_GPU_VERT_TRI) || !of_has_feature(OF_HW_GPU_XFORM_RGB))
        return;
#endif
    _gpu_cmd_header(GPU_CMD_DRAW_INDEXED_TRI, 1);
    uint32_t *w = _gpu_ring_claim();
    *w++ = ((uint32_t)(i2 & 0x1Fu) << 10)
         | ((uint32_t)(i1 & 0x1Fu) << 5)
         |  (uint32_t)(i0 & 0x1Fu);
    _gpu_ring_commit(1u);
}

/* Sticky directional-light state consumed by 0x57 lit-vertex loads.  dir is
 * normalized Q16.16 in OBJECT space (the GPU does NOT transform the normal or
 * the dir — the CPU pre-rotates the light into object space, mirroring SM64's
 * calculate_normal_dir).  Single directional light + ambient only.  Cluster-
 * gated (OF_HW_GPU_XFORM_RGB). */
static inline void of_gpu_set_light_state(int32_t dx, int32_t dy, int32_t dz,
                                          uint16_t light_rgb, uint16_t ambient_rgb,
                                          uint8_t enable) {
#ifndef OF_PC
    if (!of_has_feature(OF_HW_GPU_VERT_TRI) || !of_has_feature(OF_HW_GPU_XFORM_RGB))
        return;
#endif
    _gpu_cmd_header(GPU_CMD_SET_LIGHT_STATE, 6);
    uint32_t *w = _gpu_ring_claim();
    *w++ = (uint32_t)dx;
    *w++ = (uint32_t)dy;
    *w++ = (uint32_t)dz;
    *w++ = (uint32_t)light_rgb;
    *w++ = (uint32_t)ambient_rgb;
    *w++ = (uint32_t)(enable & 1u);
    _gpu_ring_commit(6u);
}

/* Load ONE raw vert {x,y,z} + object-space normal {nx,ny,nz} + {s,t} into a
 * GPU vertex-cache slot, with GPU-side lighting.  The GPU transforms the
 * position (0x50 matrix), computes N.L against the sticky 0x55 light, derives
 * RGB565, and writes the slot.  Normals are NOT transformed by the GPU; pass
 * them in object space (consistent with the object-space light dir).  Cluster-
 * gated (OF_HW_GPU_XFORM_RGB). */
static inline void of_gpu_load_vert_lit(uint8_t slot, int32_t vx, int32_t vy, int32_t vz,
                                        int32_t nx, int32_t ny, int32_t nz,
                                        int32_t s, int32_t t) {
#ifndef OF_PC
    if (!of_has_feature(OF_HW_GPU_VERT_TRI) || !of_has_feature(OF_HW_GPU_XFORM_RGB))
        return;
#endif
    _gpu_cmd_header(GPU_CMD_LOAD_VERT_LIT, 9);
    uint32_t *w = _gpu_ring_claim();
    *w++ = (uint32_t)(slot & 0x1Fu);
    *w++ = (uint32_t)vx;
    *w++ = (uint32_t)vy;
    *w++ = (uint32_t)vz;
    *w++ = (uint32_t)nx;
    *w++ = (uint32_t)ny;
    *w++ = (uint32_t)nz;
    *w++ = (uint32_t)s;
    *w++ = (uint32_t)t;
    _gpu_ring_commit(9u);
}

/* Submit an already-encoded command stream through the doorbell-DMA path.
 *
 * `words` must contain complete GPU commands, including each command
 * header.  This can batch order-sensitive mixtures of commands without
 * flushing whenever descriptor type changes.
 *
 * The helper does not split the stream because splitting inside a
 * command would publish an incomplete command to the decoder.  Callers
 * must cap each stream to OF_GPU_COMMAND_STREAM_BATCH_WORDS and flush
 * only at command boundaries. */
static inline void of_gpu_submit_command_stream_batch(const uint32_t *words,
                                                       int word_count) {
    if (word_count <= 0 || words == NULL) return;
    if ((uint32_t)word_count > OF_GPU_COMMAND_STREAM_BATCH_WORDS)
        __builtin_trap();

    uint32_t stream_words = (uint32_t)word_count;
    _gpu_stream_reserve_words(stream_words);

    for (int i = 0; i < word_count; i++)
        _gpu_batch_buf[_gpu_cmd_words + (uint32_t)i] = words[i];

    _gpu_cmd_words += stream_words;
    _gpu_wrptr = (_gpu_wrptr + stream_words * 4u) & _gpu_ring_mask;
    _gpu_state_valid = 0;
    _gpu_flush_cmd_stream();
}

#else /* OF_PC — desktop has no HW GPU; provide no-op stubs so apps that
       *         exercise the GPU API still link in the SDL2 test build. */

/* Raw GPU registers + ring/DMA debug counters that some ports' emit layers
 * (e.g. of_emit_q2.c) poke directly, bypassing the function API.  On the
 * desktop they sink to harmless dummies so those TUs compile and the writes
 * are no-ops — the soft renderer does the real work here. */
static volatile uint32_t _of_pc_gpu_reg_sink;
#define GPU_TEX_FLUSH       _of_pc_gpu_reg_sink
#define GPU_PALOOKUP_BASE   _of_pc_gpu_reg_sink
static uint32_t _gpu_dbg_ring_waits, _gpu_dbg_ring_spin_iters;
static uint32_t _gpu_dbg_dma_waits,  _gpu_dbg_dma_spin_iters;

typedef struct {
    uint32_t status, rdptr, wrptr, fence_reached;
    uint32_t dma_waits, dma_spin_iters;
    uint32_t ring_waits, ring_spin_iters;
    uint32_t min_ring_free, ring_free;
    /* PASSIVE SDRAM channel-utilization counters (HW-only; zeroed on PC). */
    uint32_t chan_clk, chan_busy_any, chan_xfer, chan_wait;
    uint32_t chan_rd_z, chan_rd_tex, chan_wr_z, chan_wr_color;
} of_gpu_debug_snapshot_t;

/* Mirror of the non-PC definition above so the of_gpu_set_tri_state stub
 * below has its parameter type in the desktop build (same pattern as
 * of_gpu_debug_snapshot_t). */
typedef struct {
    uint32_t fb_base;
    int32_t  fb_major_step;
    int32_t  fb_minor_step;

    uint32_t tex_addr;
    uint16_t tex_width;
    uint16_t tex_w_mask;
    uint16_t tex_h_mask;

    uint8_t  flags;
    uint8_t  colormap_id;
    uint8_t  z_mode;

    int32_t  clamp_min[2];   /* CONTRACT: min <= max per axis — see the
                              * non-PC definition above */
    int32_t  clamp_max[2];

    uint32_t z_base;
    int32_t  z_major_step;
    int32_t  z_minor_step;

    int16_t  clip_x0, clip_x1;
    int16_t  clip_y0, clip_y1;

    uint8_t  attr_q29;       /* 0 = PERSP (default), 1 = PERSP_Q29 — see
                              * the non-PC definition above */
    uint8_t  mirror_s, mirror_t;
    uint8_t  const_alpha;
    uint8_t  cd_combine;
    uint8_t  subpix_y;       /* Q12.4 subpixel vert-tri Y — see the non-PC definition above */
} of_gpu_tri_state_t;

static inline void     of_gpu_init(void)                                  {}
static inline void     of_gpu_shutdown(void)                              {}
static inline void     of_gpu_kick(void)                                  {}
static inline void     of_gpu_kick_now(void)                              {}
static inline void     of_gpu_set_tri_state(const of_gpu_tri_state_t *s)  { (void)s; }
static inline void     of_gpu_draw_vert_tri(const int16_t x[3], const int16_t y[3],
                                            const int32_t s[3], const int32_t t[3],
                                            const int32_t zi[3], const uint8_t l[3])
                                            { (void)x;(void)y;(void)s;(void)t;(void)zi;(void)l; }
static inline void     of_gpu_draw_vert_tri_rgb(const int16_t x[3], const int16_t y[3],
                                                const int32_t s[3], const int32_t t[3],
                                                const int32_t zi[3], const uint16_t rgb[3],
                                                uint32_t q29, const int32_t depth[3],
                                                const uint16_t *rgb_d)
                                                { (void)x;(void)y;(void)s;(void)t;(void)zi;(void)rgb;(void)q29;(void)depth;(void)rgb_d; }
static inline void     of_gpu_draw_xform_tri_rgb(const int32_t vx[3], const int32_t vy[3],
                                                 const int32_t vz[3], const int32_t s[3],
                                                 const int32_t t[3], const uint16_t rgb[3])
                                                 { (void)vx;(void)vy;(void)vz;(void)s;(void)t;(void)rgb; }
static inline void     of_gpu_draw_clip_tri(const int32_t cx[3], const int32_t cy[3],
                                            const int32_t cw[3], const int32_t s[3],
                                            const int32_t t[3], const uint16_t rgb[3])
                                            { (void)cx;(void)cy;(void)cw;(void)s;(void)t;(void)rgb; }
static inline void     of_gpu_load_vert(uint8_t slot, int32_t vx, int32_t vy, int32_t vz,
                                        int32_t s, int32_t t, uint16_t rgb)
                                        { (void)slot;(void)vx;(void)vy;(void)vz;(void)s;(void)t;(void)rgb; }
static inline void     of_gpu_draw_indexed_tri(uint8_t i0, uint8_t i1, uint8_t i2)
                                               { (void)i0;(void)i1;(void)i2; }
static inline void     of_gpu_set_light_state(int32_t dx, int32_t dy, int32_t dz,
                                              uint16_t light_rgb, uint16_t ambient_rgb,
                                              uint8_t enable)
                                              { (void)dx;(void)dy;(void)dz;(void)light_rgb;(void)ambient_rgb;(void)enable; }
static inline void     of_gpu_load_vert_lit(uint8_t slot, int32_t vx, int32_t vy, int32_t vz,
                                            int32_t nx, int32_t ny, int32_t nz,
                                            int32_t s, int32_t t)
                                            { (void)slot;(void)vx;(void)vy;(void)vz;(void)nx;(void)ny;(void)nz;(void)s;(void)t; }
typedef struct {
    int32_t  matrix[5][4];
    int32_t  xcenter, ycenter;
    int32_t  xscale, yscale;
    int32_t  near_clip;
    uint8_t  rows;
    uint8_t  q29_en;
    uint8_t  q29_shift;
} of_gpu_object_state_t;
static inline void     of_gpu_set_object_state(const of_gpu_object_state_t *st) { (void)st; }
static inline void     of_gpu_draw_xform_tri(const int32_t vx[3], const int32_t vy[3],
                                             const int32_t vz[3], const int32_t s[3],
                                             const int32_t t[3], const uint8_t light[3])
                                             { (void)vx;(void)vy;(void)vz;(void)s;(void)t;(void)light; }
static inline uint32_t of_gpu_fence(void)                                 { return 0; }
static inline uint32_t of_gpu_submit(void)                                { return 0; }
static inline int      of_gpu_fence_reached(uint32_t t)                   { (void)t; return 1; }
static inline void     of_gpu_wait(uint32_t t)                            { (void)t; }
static inline void     of_gpu_finish(void)                                {}
static inline void     of_gpu_prepare_framebuffer_for_cpu(void)           {}
static inline uint32_t of_gpu_flip_to(int idx)                            { (void)idx; return 0; }

static inline void of_gpu_palookup_upload(uint8_t slot, const uint8_t *data,
                                          uint32_t size) {
    (void)slot; (void)data; (void)size;
}
static inline void of_gpu_translucency_upload(const uint8_t *table, uint32_t size) {
    (void)table; (void)size;
}
static inline void of_gpu_set_framebuffer(uint32_t addr, uint16_t stride) {
    (void)addr; (void)stride;
}
static inline void of_gpu_bind_texture(const of_gpu_texture_t *tex)       { (void)tex; }
static inline void _of_gpu_route_fast_tex(int enable)                     { (void)enable; }
static inline void _of_gpu_fast_tex_upload(uint32_t off, const uint32_t *data, uint32_t n) { (void)off; (void)data; (void)n; }
static inline void of_gpu_clear(uint32_t flags, uint16_t color)           { (void)flags; (void)color; }
static inline void of_gpu_clear_rect(uint32_t addr, uint16_t w, uint16_t h,
                                     uint8_t color) {
    (void)addr; (void)w; (void)h; (void)color;
}
static inline void of_gpu_clear_rect_strided(uint32_t addr, uint16_t w,
                                             uint16_t h, uint16_t stride,
                                             uint8_t color) {
    (void)addr; (void)w; (void)h; (void)stride; (void)color;
}
static inline void of_gpu_draw_affine_span_group(const of_gpu_affine_span_group_t *g) {
    (void)g;
}
static inline void of_gpu_draw_column_list(const of_gpu_column_list_group_t *g) {
    (void)g;
}
static inline void of_gpu_draw_persp_span_group(const of_gpu_persp_span_group_t *g) {
    (void)g;
}
static inline void of_gpu_draw_persp_span_group_batch(const of_gpu_persp_span_group_t *spans,
                                                      int count) {
    (void)spans; (void)count;
}
static inline void of_gpu_draw_param_tri(const of_gpu_param_span_list_t *p,
                                         const of_gpu_tri_vert_t v[3],
                                         int16_t clip_x0, int16_t clip_x1,
                                         int16_t clip_y0, int16_t clip_y1) {
    (void)p; (void)v;
    (void)clip_x0; (void)clip_x1; (void)clip_y0; (void)clip_y1;
}
static inline void of_gpu_draw_param_tri_recs(const of_gpu_param_span_list_t *p,
                                              const of_gpu_tri_vert_t v[3]) {
    (void)p; (void)v;
}
static inline void of_gpu_draw_param_span_list(const of_gpu_param_span_list_t *params,
                                               const of_gpu_param_span_record_t *records,
                                               uint32_t record_count) {
    (void)params; (void)records; (void)record_count;
}
static inline void of_gpu_submit_command_stream_batch(const uint32_t *words, int count) {
    (void)words; (void)count;
}
static inline void of_gpu_debug_snapshot(of_gpu_debug_snapshot_t *snap, int reset) {
    (void)reset;
    if (snap) {
        snap->status = snap->rdptr = snap->wrptr = snap->fence_reached = 0;
        snap->dma_waits = snap->dma_spin_iters = 0;
        snap->ring_waits = snap->ring_spin_iters = 0;
        snap->min_ring_free = snap->ring_free = 0;
        snap->chan_clk = snap->chan_busy_any = snap->chan_xfer = snap->chan_wait = 0;
        snap->chan_rd_z = snap->chan_rd_tex = snap->chan_wr_z = snap->chan_wr_color = 0;
    }
}

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_GPU_H */
