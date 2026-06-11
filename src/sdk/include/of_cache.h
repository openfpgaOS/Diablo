//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_cache.h -- Cache management API for openfpgaOS
 *
 * Exposes D-cache flush and I-cache invalidate for advanced users.
 * Most apps do not need to call these directly.
 */

#ifndef OF_CACHE_H
#define OF_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef OF_PC

#include "of_caps.h"
#include "of_services.h"

/* Flush D-cache for the draw buffer (same as of_video_flush). */
static inline void of_cache_flush_video(void) {
    OF_SVC->video_flush_cache();
}

/* Full D-cache write-back + I-cache invalidate.  Use before handing
 * data off to a DMA peer that reads from RAM, or after writing code
 * the CPU is about to fetch. */
static inline void of_cache_flush(void) {
    OF_SVC->cache_flush();
}

/* Range variants — cheaper than the full flush when the affected
 * window is small. */
static inline void of_cache_clean_range(void *addr, uint32_t size) {
    OF_SVC->cache_clean_range(addr, size);
}

static inline void of_cache_inval_range(void *addr, uint32_t size) {
    OF_SVC->cache_inval_range(addr, size);
}

/* Range writeback + invalidate (cbo.flush per line).  On this
 * VexiiRiscv config, cbo.clean alone leaves some dirty lines in L1 —
 * external AXI masters (GPU m_rd_*, audio mixer voice fetch) read
 * DRAM directly and see stale bytes.  Use this whenever you've just
 * written a buffer that an external master will read.  Falls back to
 * the full cache_flush() on older firmware that doesn't expose this
 * service entry. */
static inline void of_cache_flush_range(void *addr, uint32_t size) {
    if (OF_SVC->cache_flush_range)
        OF_SVC->cache_flush_range(addr, size);
    else
        OF_SVC->cache_flush();
}

/* Invalidate I-cache (fence.i). */
static inline void of_cache_invalidate_icache(void) {
    __asm__ volatile("fence");
    __asm__ volatile(".word 0x0000100f"); /* fence.i */
}

/* ── Uncached memory access ────────────────────────────────────────
 * Bypass the D-cache for random byte reads/writes (e.g. reading
 * sample data, framebuffer peek, DMA buffers).
 *
 * The SDRAM base + uncached-alias base come from the caps descriptor
 * the kernel hands the app at startup, so this header has no compiled-
 * in target addresses. The two loads are cheap (cached struct, single
 * line) and well below DMA setup overhead. */

/* Convert a cached SDRAM pointer to its uncached alias */
static inline volatile void *of_uncached(const void *ptr) {
    const struct of_capabilities *c = of_get_caps();
    return (volatile void *)((uint32_t)(uintptr_t)ptr
                             - c->sdram_base + c->sdram_uncached_base);
}

#else /* OF_PC */

static inline void of_cache_flush_video(void) { /* no-op on PC */ }
static inline void of_cache_flush(void) { /* no-op on PC */ }
static inline void of_cache_clean_range(void *addr, uint32_t size) { (void)addr; (void)size; }
static inline void of_cache_inval_range(void *addr, uint32_t size) { (void)addr; (void)size; }
static inline void of_cache_flush_range(void *addr, uint32_t size) { (void)addr; (void)size; }
static inline void of_cache_invalidate_icache(void) { /* no-op on PC */ }

/* PC: no cache — just access directly */
static inline volatile void *of_uncached(const void *ptr) { return (volatile void *)ptr; }

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_CACHE_H */
