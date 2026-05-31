/*
 * of_services.h -- openfpgaOS OS Services Table
 *
 * Direct function pointer table for OS services. Apps call through
 * this table instead of ecall, saving ~50 cycles per call.
 *
 * The table address is delivered to apps via the AT_OF_SVC auxv tag
 * set up by the kernel ELF loader (see of_app_abi.h). of_init.c's
 * constructor stashes the pointer in _of_svc_ptr before main() runs;
 * apps just use the OF_SVC macro:
 *
 *   uint8_t *fb = OF_SVC->video_get_surface();
 */

#ifndef OF_SERVICES_H
#define OF_SERVICES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

#define OF_SVC_MAGIC    0x4F535643  /* 'OSVC' */
#define OF_SVC_VERSION  2   /* v2: retired AWE coprocessor slots removed */

/* Forward declare input state struct */
struct of_input_state;

/* Display timing snapshot shared by the SDK API and OS services table.
 * Times are sampled from the CPU cycle counter and converted to
 * microseconds by the OS when the snapshot is copied. */
#ifndef OF_VIDEO_TIMING_T_DEFINED
#define OF_VIDEO_TIMING_T_DEFINED
typedef struct of_video_timing {
    uint32_t vblank_count;
    uint32_t present_count;
    uint32_t last_presented_idx;
    uint32_t reserved;
    uint64_t last_vblank_us;
    uint64_t last_flip_presented_us;
} of_video_timing_t;
#endif

#ifndef OF_VIDEO_MODE_T_DEFINED
#define OF_VIDEO_MODE_T_DEFINED
typedef struct of_video_mode {
    uint16_t width;
    uint16_t height;
    uint16_t stride;
    uint8_t color_mode;
    uint8_t reserved;
} of_video_mode_t;
#endif

#ifndef OF_VIDEO_CAPS_T_DEFINED
#define OF_VIDEO_CAPS_T_DEFINED
typedef struct of_video_caps {
    uint16_t max_width;
    uint16_t max_height;
    uint16_t max_stride;
    uint16_t physical_width;
    uint16_t physical_height;
    uint16_t default_width;
    uint16_t default_height;
    uint16_t default_stride;
    uint32_t max_frame_bytes;
    uint32_t color_mode_mask;
} of_video_caps_t;
#endif

struct of_services_table {
    uint32_t magic;
    uint32_t version;
    uint32_t count;         /* Number of function pointers */

    /* -- Video (14) -- */
    void      (*video_init)(void);
    uint8_t * (*video_get_surface)(void);
    uint8_t * (*video_flip)(void);
    void      (*video_wait_flip)(void);
    void      (*video_vsync)(void);
    void      (*video_set_palette)(uint8_t index, uint32_t rgb);
    void      (*video_set_palette_bulk)(const uint32_t *pal, int count);
    void      (*video_set_palette_vga4)(const uint8_t *vga_pal, int count);
    void      (*video_clear)(uint8_t color);
    void      (*video_flush_cache)(void);
    void      (*video_set_display_mode)(int mode);
    void      (*video_set_color_mode)(int mode);
    /* GPU-triggered flip (cr-gpu-triggered-flip.md):
     * acquire_next(just_flipped_idx, fence_token) — caller passes idx
     * of the buffer they just emitted CMD_FLIP for and the fence
     * token the SDK helper returned (or -1, 0 on first call).  Kernel
     * waits for fence_reached >= token (proves CMD_FLIP retired and
     * fb_swap_pending=1 was latched), then returns the third buffer:
     * not current scanout and not queued for next vsync.  It does not
     * wait for vsync.  buffer_addr(idx) — returns FB address of the
     * given idx. */
    int       (*video_acquire_next)(int just_flipped_idx, uint32_t fence_token);
    uint8_t * (*video_buffer_addr)(int idx);

    /* -- Input (4) -- */
    void      (*input_poll)(void);
    void      (*input_get_state)(int player, void *out);
    void      (*input_poll_p0)(void *out);
    void      (*input_set_deadzone)(int16_t deadzone);

    /* -- Mixer core (22) -- (extensions appended below) */
    void      (*mixer_init)(int max_voices, int output_rate);
    int       (*mixer_play)(const uint8_t *pcm_s16, uint32_t sample_count,
                            uint32_t sample_rate, int priority, int volume);
    void      (*mixer_stop)(int voice);
    void      (*mixer_stop_all)(void);
    void      (*mixer_set_volume)(int voice, int volume);
    void      (*mixer_set_pan)(int voice, int pan);
    int       (*mixer_voice_active)(int voice);
    void      (*mixer_pump)(void);
    void      (*mixer_set_loop)(int voice, int loop_start, int loop_end);
    void      (*mixer_set_rate)(int voice, int sample_rate_hz);
    void      (*mixer_set_rate_raw)(int voice, uint32_t rate_fp16);
    void      (*mixer_set_vol_lr)(int voice, int vol_l, int vol_r);
    void      (*mixer_set_bidi)(int voice, int enable);
    int       (*mixer_get_position)(int voice);
    void      (*mixer_set_position)(int voice, int sample_offset);
    void      (*mixer_set_voice)(int voice, int sample_rate_hz, int vol_l, int vol_r);
    void      (*mixer_set_voice_raw)(int voice, uint32_t rate_fp16, int vol_l, int vol_r);
    void      (*mixer_set_vol_rate)(int voice, int rate);
    uint32_t  (*mixer_poll_ended)(void);
    void *    (*mixer_alloc_samples)(uint32_t size);
    void      (*mixer_free_samples)(void);
    void      (*mixer_set_end_callback)(void (*cb)(uint32_t ended_mask));

    /* -- Audio (3) -- */
    void      (*audio_init)(void);
    int       (*audio_write)(const int16_t *samples, int count);
    int       (*audio_get_free)(void);

    /* -- Timer (5) -- */
    void      (*timer_set_callback)(void (*cb)(void), uint32_t hz);
    void      (*timer_stop)(void);
    uint32_t  (*timer_get_us)(void);
    uint32_t  (*timer_get_ms)(void);
    void      (*timer_delay_us)(uint32_t us);

    /* -- Cache (3) -- */
    void      (*cache_flush)(void);
    void      (*cache_clean_range)(void *addr, uint32_t size);
    void      (*cache_inval_range)(void *addr, uint32_t size);

    /* -- Vsync callback (1) -- */
    void      (*video_set_vsync_callback)(void (*cb)(void));

    /* -- File (2) -- */
    long      (*file_size)(const char *path);
    long      (*file_size_fd)(int fd);

    /* -- Mixer + audio extensions (5+4, append-only to preserve ABI) --
     *    The audio_stream_* slots logically belong to the audio module
     *    but live here for ABI append-only ordering. */
    void      (*mixer_retrigger)(int voice, const uint8_t *pcm_s16,
                                 uint32_t sample_count, uint32_t sample_rate,
                                 int volume);
    int       (*mixer_play_8bit)(const uint8_t *pcm_s8, uint32_t sample_count,
                                 uint32_t sample_rate, int priority, int volume);
    void      (*mixer_set_group)(int voice, int group);
    void      (*mixer_set_group_volume)(int group, int volume);
    void      (*mixer_set_master_volume)(int volume);
    void      (*mixer_set_filter)(int voice, int cutoff_q016, int q, int enable);
    int       (*audio_stream_open)(int sample_rate);
    int       (*audio_stream_write)(const int16_t *samples, int count);
    int       (*audio_stream_ready)(void);
    void      (*audio_stream_close)(void);

    /* -- Filesystem (1) -- append-only, ABI-stable --
     *    Register a filename→slot mapping for fopen() by name.
     *    The openFPGA manifest identifies data slots by numeric id;
     *    this service lets apps tell the kernel which id holds which
     *    filename so fopen() by name resolves correctly. Overwrites
     *    any prior mapping for the same filename. Max 32 entries. */
    void      (*file_slot_register)(uint32_t slot_id, const char *filename);

    /* -- SoundFont preload (append-only, ABI-stable) --
     *    The kernel auto-loads the first .ofsf file it finds in a data
     *    slot during boot. Apps should check smp_bank_preload_base and,
     *    when non-NULL, skip of_smp_bank_load() and reuse the preloaded
     *    SDRAM buffer directly. Older firmware leaves these as NULL/0. */
    const void *smp_bank_preload_base;
    uint32_t    smp_bank_preload_size;

    /* -- Mixer group-aware allocation (append-only, ABI-stable) --
     *    Atomic alloc-and-tag entry; lets callers that know which
     *    group a new voice belongs to bias the slot search and steal
     *    paths so MUSIC and SFX don't collide in the same slot range.
     *    `mixer_voice_group` reads back the current tag for a slot
     *    (cheap shadow read) so callers can validate ownership before
     *    writing — used by the SW MIDI ISR to drop stale references
     *    when a slot has been reassigned to another group.  Older
     *    firmware leaves these as NULL; callers should fall back to
     *    of_mixer_play + of_mixer_set_group when these are absent. */
    int       (*mixer_alloc_for_group)(int group, const uint8_t *pcm_s16,
                                       uint32_t sample_count,
                                       uint32_t sample_rate,
                                       int priority, int volume);
    int       (*mixer_voice_group)(int voice);

    /* -- Cache (append-only) -- */
    /* Range-granular writeback + invalidate (cbo.flush per line).
     * On this VexiiRiscv config, cbo.clean alone has been observed
     * to leave dirty lines in L1 (the bank-preload + audio-mixer
     * paths both regressed when using clean-only).  Use this when
     * preparing a buffer to be read by an external AXI master like
     * the GPU's m_rd_* or the audio mixer's per-voice fetch — they
     * read DRAM directly, not through the CPU's cache.
     * Older firmware leaves this NULL; callers should fall back to
     * cache_flush() (full sweep) when this is absent. */
    void      (*cache_flush_range)(void *addr, uint32_t size);

    /* -- Input HID extensions (append-only) --
     * Dock keyboard and mouse are APF Player 3/4 special controller
     * reports, exposed separately from the two gamepad player snapshots. */
    void      (*input_get_keyboard_state)(void *out);
    void      (*input_read_mouse_state)(void *out);

    /* -- Mixer stable handles (append-only) --
     * These entries name logical sounds instead of physical mixer slots.
     * A stale handle must never be able to mutate a slot after reuse. */
    uint64_t  (*mixer_play_h)(const uint8_t *pcm_s16,
                              uint32_t sample_count,
                              uint32_t sample_rate,
                              int priority,
                              int volume);
    uint64_t  (*mixer_play_8bit_h)(const uint8_t *pcm_s8,
                                   uint32_t sample_count,
                                   uint32_t sample_rate,
                                   int priority,
                                   int volume);
    uint64_t  (*mixer_alloc_for_group_h)(int group,
                                         const uint8_t *pcm_s16,
                                         uint32_t sample_count,
                                         uint32_t sample_rate,
                                         int priority,
                                         int volume);
    uint64_t  (*mixer_retrigger_h)(uint64_t handle,
                                   const uint8_t *pcm_s16,
                                   uint32_t sample_count,
                                   uint32_t sample_rate,
                                   int volume);
    void      (*mixer_stop_h)(uint64_t handle);
    int       (*mixer_handle_active)(uint64_t handle);
    int       (*mixer_handle_group)(uint64_t handle);
    int       (*mixer_handle_voice)(uint64_t handle);
    void      (*mixer_set_volume_h)(uint64_t handle, int volume);
    void      (*mixer_set_pan_h)(uint64_t handle, int pan);
    void      (*mixer_set_loop_h)(uint64_t handle, int loop_start, int loop_end);
    void      (*mixer_set_rate_h)(uint64_t handle, int sample_rate_hz);
    void      (*mixer_set_rate_raw_h)(uint64_t handle, uint32_t rate_fp16);
    void      (*mixer_set_vol_lr_h)(uint64_t handle, int vol_l, int vol_r);
    void      (*mixer_set_bidi_h)(uint64_t handle, int enable);
    int       (*mixer_get_position_h)(uint64_t handle);
    void      (*mixer_set_position_h)(uint64_t handle, int sample_offset);
    void      (*mixer_set_voice_h)(uint64_t handle,
                                   int sample_rate_hz,
                                   int vol_l,
                                   int vol_r);
    void      (*mixer_set_voice_raw_h)(uint64_t handle,
                                       uint32_t rate_fp16,
                                       int vol_l,
                                       int vol_r);
    void      (*mixer_set_vol_rate_h)(uint64_t handle, int rate);
    void      (*mixer_set_filter_h)(uint64_t handle,
                                    int cutoff_q016,
                                    int q,
                                    int enable);
    uint32_t  (*mixer_poll_ended_h)(uint64_t *out_handles,
                                    uint32_t max_handles);

    /* -- Video timing (append-only) --
     * Snapshot of the most recent vblank and presented flip.  This is
     * intentionally additive so existing flip APIs keep their behavior
     * while apps that need smooth interpolation can pace against the
     * actual scanout clock. */
    void      (*video_get_timing)(of_video_timing_t *out);

    /* -- Video refresh policy (append-only) --
     * v_total=0 restores the OS automatic render-period policy.
     * Nonzero values request a fixed scanout line count; hardware clamps
     * again and fixed Analogizer/SNAC modes override this request. */
    void      (*video_set_refresh_vtotal)(uint32_t v_total);

    /* -- Dynamic framebuffer modes (append-only) --
     * The active source framebuffer can be resized at runtime.  The
     * scanout scaler maps it into the target's physical output timing. */
    int       (*video_set_mode)(const of_video_mode_t *mode);
    void      (*video_get_mode)(of_video_mode_t *out);
    int       (*video_get_mode_count)(void);
    int       (*video_get_mode_info)(int index, of_video_mode_t *out);
    void      (*video_get_caps)(of_video_caps_t *out);
    int       (*video_check_mode)(const of_video_mode_t *mode,
                                  of_video_mode_t *normalized);

    /* -- OS/app configuration (append-only) --
     * Parsed from os.ini at boot. Section/key lookup is
     * case-insensitive; values preserve case and internal spaces. */
    int       (*config_get)(const char *section, const char *key,
                            char *out, uint32_t out_len);
    int       (*config_get_int)(const char *section, const char *key,
                                int default_value);
    int       (*config_get_bool)(const char *section, const char *key,
                                 int default_value);
    int       (*config_next)(const char *section, uint32_t *cursor,
                             char *key_out, uint32_t key_len,
                             char *value_out, uint32_t value_len);
};

#ifndef OF_PC

/* Populated by of_init.c's constructor from the AT_OF_SVC auxv tag.
 * Apps must not read this directly -- use the OF_SVC macro so the
 * indirection can change without breaking the API. */
extern const struct of_services_table *_of_svc_ptr;

#define OF_SVC (_of_svc_ptr)

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_SERVICES_H */
