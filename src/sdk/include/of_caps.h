//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_caps.h -- openfpgaOS Capability Descriptors
 *
 * The OS populates a capability struct at a fixed BRAM address before
 * launching the application. Apps read it to discover the platform,
 * available hardware, memory layout, and OS services.
 *
 * This replaces hardcoded addresses (framebuffer base, audio reserve,
 * GPU MMIO window, ...) and enables the same app binary to run on
 * different targets (Pocket, MiSTer) and core variants (full, lite, 3d).
 *
 *   const struct of_capabilities *caps = of_get_caps();
 *   if (of_has_feature(OF_HW_MIXER))
 *       init_pcm_audio();
 */

#ifndef OF_CAPS_H
#define OF_CAPS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define OF_CAPS_MAGIC   0x43415053  /* 'CAPS' */
#define OF_CAPS_VERSION 2
/* The of_capabilities pointer is delivered to apps via the AT_OF_CAPS
 * auxv tag set up by the kernel ELF loader (see of_app_abi.h). Apps
 * never need to know where the struct lives -- they just call
 * of_get_caps() below. */

/* Platform IDs */
#define OF_PLATFORM_POCKET    1
#define OF_PLATFORM_MISTER    2
#define OF_PLATFORM_SIM       255

/* Hardware feature flags — must match RTL HW_FEATURES bit layout in
 * src/fpga/common/axi_periph_slave.v and src/firmware/os/hal/regs.h. */
#define OF_HW_MIXER         (1 << 0)    /* Audio FIFO + CPU-side software mixer */
#define OF_HW_NET           (1 << 2)    /* Networking (link cable / serial / wifi) */
#define OF_HW_ANALOGIZER    (1 << 3)    /* Analog video output */
#define OF_HW_GPU_SPAN      (1 << 4)    /* GPU span renderer (always set) */
#define OF_HW_MIDI          (1 << 6)    /* MIDI playback (sample-based synth) */
#define OF_HW_WIFI          (1 << 7)    /* Wireless networking */
#define OF_HW_FPU           (1 << 8)    /* Hardware FPU (RISC-V F extension) */
#define OF_HW_SAVE_SLOTS    (1 << 9)    /* Persistent save storage */
#define OF_HW_GPU_VCOLOR    (1 << 10)   /* GPU vertex color interpolation */
#define OF_HW_GPU_BILINEAR  (1 << 11)   /* GPU bilinear texture filter */
#define OF_HW_GPU_ALPHA     (1 << 12)   /* GPU alpha / additive blending */
#define OF_HW_GPU_PERSP     (1 << 13)   /* GPU perspective-correct spans */
#define OF_HW_GPU_FRAGPIPE  (1 << 14)   /* GPU 1-px/cycle fragment pipeline */
#define OF_HW_GPU_PARAM_SPAN_LIST (1 << 15) /* GPU parametric span-list command */
#define OF_HW_GPU_PARAM_SPAN_Z    (1 << 16) /* Param-span Quake-compatible z writes */
#define OF_HW_GPU_PARAM_SPAN_ZTEST (1 << 17) /* Param-span Quake-compatible z test/write */
#define OF_HW_GPU_PARAM_SPAN_Q29_SCALE (1 << 18) /* Param-span Q29 dynamic scale */

/* Convenience: all the GPU bits an app might care about for renderer choice. */
#define OF_HW_GPU_LITE_MASK  (OF_HW_GPU_SPAN | OF_HW_GPU_FRAGPIPE)
#define OF_HW_GPU_FULL_MASK  (OF_HW_GPU_LITE_MASK | OF_HW_GPU_VCOLOR | \
                              OF_HW_GPU_BILINEAR | OF_HW_GPU_ALPHA)

struct of_capabilities {
    uint32_t magic;             /* OF_CAPS_MAGIC */
    uint32_t version;           /* OF_CAPS_VERSION */

    /* Memory regions (base + size, 0 = not available) */
    uint32_t heap_base;         /* App heap start (SDRAM) */
    uint32_t heap_size;         /* App heap size in bytes */
    uint32_t fb_base;           /* Framebuffer 0 base address */
    uint32_t fb_size;           /* Single framebuffer size in bytes */
    uint32_t fb_width;          /* Framebuffer width in pixels */
    uint32_t fb_height;         /* Framebuffer height in pixels */
    uint32_t fb_stride;         /* Bytes per row */
    uint32_t sample_base;       /* Persistent audio reservation base */
    uint32_t sample_size;       /* Persistent audio reservation bytes */

    /* Hardware features */
    uint32_t hw_features;       /* OF_HW_* bitmask */
    uint32_t mixer_voices;      /* Max voices in CPU software mixer (32) */
    uint32_t mixer_rate;        /* Output sample rate (48000) */

    /* Platform identity */
    uint32_t platform_id;       /* OF_PLATFORM_* */
    uint32_t core_variant;      /* Bitstream variant (0 = default) */
    uint32_t sdram_size;        /* Total SDRAM in bytes */
    uint32_t cram_size;         /* Per-bank CRAM size in bytes */

    /* OS info */
    uint32_t os_version;        /* Packed: major.minor.patch */
    uint32_t cpu_freq_hz;       /* CPU clock frequency */

    /* Memory bases for inline accessors that need to translate
     * pointers without hardcoding target addresses. Added in v2. */
    uint32_t sdram_base;            /* CPU base of cached SDRAM */
    uint32_t sdram_uncached_base;   /* CPU base of D-cache-bypass alias */
    uint32_t gpu_base;              /* GPU MMIO window base (0 = no GPU) */
};

#ifndef OF_PC

/* Populated by of_init.c's constructor from the AT_OF_CAPS auxv tag.
 * Apps must not read this directly -- use of_get_caps() so the
 * indirection can change without breaking the API. */
extern const struct of_capabilities *_of_caps_ptr;

static inline const struct of_capabilities *of_get_caps(void) {
    return _of_caps_ptr;
}

static inline int of_has_feature(uint32_t feature) {
    return (of_get_caps()->hw_features & feature) != 0;
}

#else /* OF_PC */

const struct of_capabilities *of_get_caps(void);
int of_has_feature(uint32_t feature);

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_CAPS_H */
