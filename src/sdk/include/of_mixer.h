//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_mixer.h -- PCM Mixer API for openfpgaOS
 *
 * 32-voice hardware PCM mixer.  Samples live in normal SDRAM buffers;
 * the OS flushes cached buffers before handing their absolute SDRAM
 * address to the mixer DMA path.
 *
 * Usage:
 *   1. Call of_mixer_init()
 *   2. Allocate sample memory: buf = malloc(size)
 *   3. Load 16-bit signed mono PCM data into buf
 *   4. Play: of_mixer_play(buf, sample_count, sample_rate, 0, volume)
 *   5. Optionally set loop, rate, stereo volume, bidi, position
 *
 * Volume: 0-255 per channel.
 * Resampling: 16.16 fixed-point rate, linear interpolation.
 * Volume ramp: per-sample step, configurable rate.
 */

#ifndef OF_MIXER_H
#define OF_MIXER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

#define OF_MIXER_MAX_VOICES  32
#define OF_MIXER_OUTPUT_RATE 48000

typedef uint64_t of_mixer_handle_t;

#define OF_MIXER_HANDLE_INVALID ((of_mixer_handle_t)0)

/* Convert sample rate in Hz to 16.16 fixed-point for raw API.
 * Use at init time to build a note-rate lookup table. */
#define OF_MIXER_RATE_FP16(hz) \
    ((uint32_t)(((uint64_t)(hz) << 16) / OF_MIXER_OUTPUT_RATE))

/* Volume groups (constants, shared by both branches — the PC stub backend
 * uses them too so apps can compile against the same group symbols).
 * Groups: OF_MIXER_GROUP_SFX (0), OF_MIXER_GROUP_MUSIC (1),
 *         OF_MIXER_GROUP_VOICE (2), OF_MIXER_GROUP_AUX (3). */
#define OF_MIXER_GROUP_SFX   0
#define OF_MIXER_GROUP_MUSIC 1
#define OF_MIXER_GROUP_VOICE 2
#define OF_MIXER_GROUP_AUX   3

#ifndef OF_PC

#include "of_services.h"

#define OF_SVC_FIELD_INDEX(field) \
    ((uint32_t)((offsetof(struct of_services_table, field) - \
                 offsetof(struct of_services_table, video_init)) / sizeof(void *)))

#define OF_SVC_HAS_FIELD(field) \
    (OF_SVC && (OF_SVC->count > OF_SVC_FIELD_INDEX(field)))

static inline void of_mixer_init(int max_voices, int output_rate) {
    OF_SVC->mixer_init(max_voices, output_rate);
}

static inline int of_mixer_play(const uint8_t *pcm_s16, uint32_t sample_count,
                                uint32_t sample_rate, int priority, int volume) {
    return OF_SVC->mixer_play(pcm_s16, sample_count, sample_rate, priority, volume);
}

static inline int of_mixer_play_8bit(const uint8_t *pcm_s8, uint32_t sample_count,
                                     uint32_t sample_rate, int priority, int volume) {
    return OF_SVC->mixer_play_8bit(pcm_s8, sample_count, sample_rate, priority, volume);
}

static inline of_mixer_handle_t
of_mixer_play_h(const uint8_t *pcm_s16, uint32_t sample_count,
                uint32_t sample_rate, int priority, int volume) {
    if (OF_SVC_HAS_FIELD(mixer_play_h) && OF_SVC->mixer_play_h)
        return OF_SVC->mixer_play_h(pcm_s16, sample_count, sample_rate,
                                    priority, volume);
    return OF_MIXER_HANDLE_INVALID;
}

static inline of_mixer_handle_t
of_mixer_play_8bit_h(const uint8_t *pcm_s8, uint32_t sample_count,
                     uint32_t sample_rate, int priority, int volume) {
    if (OF_SVC_HAS_FIELD(mixer_play_8bit_h) && OF_SVC->mixer_play_8bit_h)
        return OF_SVC->mixer_play_8bit_h(pcm_s8, sample_count, sample_rate,
                                         priority, volume);
    return OF_MIXER_HANDLE_INVALID;
}

static inline void of_mixer_retrigger(int voice, const uint8_t *pcm_s16,
                                      uint32_t sample_count, uint32_t sample_rate,
                                      int volume) {
    OF_SVC->mixer_retrigger(voice, pcm_s16, sample_count, sample_rate, volume);
}

static inline of_mixer_handle_t
of_mixer_retrigger_h(of_mixer_handle_t handle, const uint8_t *pcm_s16,
                     uint32_t sample_count, uint32_t sample_rate, int volume) {
    if (OF_SVC_HAS_FIELD(mixer_retrigger_h) && OF_SVC->mixer_retrigger_h)
        return OF_SVC->mixer_retrigger_h(handle, pcm_s16, sample_count,
                                         sample_rate, volume);
    return OF_MIXER_HANDLE_INVALID;
}

static inline void of_mixer_stop(int voice) { OF_SVC->mixer_stop(voice); }
static inline void of_mixer_stop_all(void) { OF_SVC->mixer_stop_all(); }

static inline void of_mixer_stop_h(of_mixer_handle_t handle) {
    if (OF_SVC_HAS_FIELD(mixer_stop_h) && OF_SVC->mixer_stop_h)
        OF_SVC->mixer_stop_h(handle);
}

static inline void of_mixer_set_volume(int voice, int volume) {
    OF_SVC->mixer_set_volume(voice, volume);
}

static inline void of_mixer_set_volume_h(of_mixer_handle_t handle, int volume) {
    if (OF_SVC_HAS_FIELD(mixer_set_volume_h) && OF_SVC->mixer_set_volume_h)
        OF_SVC->mixer_set_volume_h(handle, volume);
}

static inline void of_mixer_pump(void) { OF_SVC->mixer_pump(); }

static inline int of_mixer_voice_active(int voice) {
    return OF_SVC->mixer_voice_active(voice);
}

static inline int of_mixer_handle_active(of_mixer_handle_t handle) {
    if (OF_SVC_HAS_FIELD(mixer_handle_active) && OF_SVC->mixer_handle_active)
        return OF_SVC->mixer_handle_active(handle);
    return 0;
}

static inline int of_mixer_handle_group(of_mixer_handle_t handle) {
    if (OF_SVC_HAS_FIELD(mixer_handle_group) && OF_SVC->mixer_handle_group)
        return OF_SVC->mixer_handle_group(handle);
    return -1;
}

static inline int of_mixer_handle_voice(of_mixer_handle_t handle) {
    if (OF_SVC_HAS_FIELD(mixer_handle_voice) && OF_SVC->mixer_handle_voice)
        return OF_SVC->mixer_handle_voice(handle);
    return -1;
}

static inline void of_mixer_set_pan(int voice, int pan) {
    OF_SVC->mixer_set_pan(voice, pan);
}

static inline void of_mixer_set_pan_h(of_mixer_handle_t handle, int pan) {
    if (OF_SVC_HAS_FIELD(mixer_set_pan_h) && OF_SVC->mixer_set_pan_h)
        OF_SVC->mixer_set_pan_h(handle, pan);
}

static inline void of_mixer_set_loop(int voice, int loop_start, int loop_end) {
    OF_SVC->mixer_set_loop(voice, loop_start, loop_end);
}

static inline void of_mixer_set_loop_h(of_mixer_handle_t handle,
                                       int loop_start, int loop_end) {
    if (OF_SVC_HAS_FIELD(mixer_set_loop_h) && OF_SVC->mixer_set_loop_h)
        OF_SVC->mixer_set_loop_h(handle, loop_start, loop_end);
}

static inline void of_mixer_set_rate(int voice, int sample_rate_hz) {
    OF_SVC->mixer_set_rate(voice, sample_rate_hz);
}

static inline void of_mixer_set_rate_h(of_mixer_handle_t handle,
                                       int sample_rate_hz) {
    if (OF_SVC_HAS_FIELD(mixer_set_rate_h) && OF_SVC->mixer_set_rate_h)
        OF_SVC->mixer_set_rate_h(handle, sample_rate_hz);
}

static inline void of_mixer_set_vol_lr(int voice, int vol_l, int vol_r) {
    OF_SVC->mixer_set_vol_lr(voice, vol_l, vol_r);
}

static inline void of_mixer_set_vol_lr_h(of_mixer_handle_t handle,
                                         int vol_l, int vol_r) {
    if (OF_SVC_HAS_FIELD(mixer_set_vol_lr_h) && OF_SVC->mixer_set_vol_lr_h)
        OF_SVC->mixer_set_vol_lr_h(handle, vol_l, vol_r);
}

static inline void of_mixer_set_bidi(int voice, int enable) {
    OF_SVC->mixer_set_bidi(voice, enable);
}

static inline void of_mixer_set_bidi_h(of_mixer_handle_t handle, int enable) {
    if (OF_SVC_HAS_FIELD(mixer_set_bidi_h) && OF_SVC->mixer_set_bidi_h)
        OF_SVC->mixer_set_bidi_h(handle, enable);
}

static inline int of_mixer_get_position(int voice) {
    return OF_SVC->mixer_get_position(voice);
}

static inline int of_mixer_get_position_h(of_mixer_handle_t handle) {
    if (OF_SVC_HAS_FIELD(mixer_get_position_h) && OF_SVC->mixer_get_position_h)
        return OF_SVC->mixer_get_position_h(handle);
    return -1;
}

static inline void of_mixer_set_position(int voice, int sample_offset) {
    OF_SVC->mixer_set_position(voice, sample_offset);
}

static inline void of_mixer_set_position_h(of_mixer_handle_t handle,
                                           int sample_offset) {
    if (OF_SVC_HAS_FIELD(mixer_set_position_h) && OF_SVC->mixer_set_position_h)
        OF_SVC->mixer_set_position_h(handle, sample_offset);
}

/* Set rate + stereo volume in one call — tracker hot path. */
static inline void of_mixer_set_voice(int voice, int sample_rate_hz,
                                      int vol_l, int vol_r) {
    OF_SVC->mixer_set_voice(voice, sample_rate_hz, vol_l, vol_r);
}

static inline void of_mixer_set_voice_h(of_mixer_handle_t handle,
                                        int sample_rate_hz,
                                        int vol_l, int vol_r) {
    if (OF_SVC_HAS_FIELD(mixer_set_voice_h) && OF_SVC->mixer_set_voice_h)
        OF_SVC->mixer_set_voice_h(handle, sample_rate_hz, vol_l, vol_r);
}

static inline void of_mixer_set_rate_raw(int voice, uint32_t rate_fp16) {
    OF_SVC->mixer_set_rate_raw(voice, rate_fp16);
}

static inline void of_mixer_set_rate_raw_h(of_mixer_handle_t handle,
                                           uint32_t rate_fp16) {
    if (OF_SVC_HAS_FIELD(mixer_set_rate_raw_h) && OF_SVC->mixer_set_rate_raw_h)
        OF_SVC->mixer_set_rate_raw_h(handle, rate_fp16);
}

/* Set rate (16.16 raw) + stereo volume — zero-division tracker hot path. */
static inline void of_mixer_set_voice_raw(int voice, uint32_t rate_fp16,
                                          int vol_l, int vol_r) {
    OF_SVC->mixer_set_voice_raw(voice, rate_fp16, vol_l, vol_r);
}

static inline void of_mixer_set_voice_raw_h(of_mixer_handle_t handle,
                                            uint32_t rate_fp16,
                                            int vol_l, int vol_r) {
    if (OF_SVC_HAS_FIELD(mixer_set_voice_raw_h) && OF_SVC->mixer_set_voice_raw_h)
        OF_SVC->mixer_set_voice_raw_h(handle, rate_fp16, vol_l, vol_r);
}

/* Set the volume-ramp rate for a voice -- how fast the hardware smooths
 * volume transitions when of_mixer_set_volume()/set_vol_lr() change.
 * `rate` is a hardware-specific step value (higher = faster). */
static inline void of_mixer_set_volume_ramp(int voice, int rate) {
    OF_SVC->mixer_set_vol_rate(voice, rate);
}

static inline void of_mixer_set_volume_ramp_h(of_mixer_handle_t handle, int rate) {
    if (OF_SVC_HAS_FIELD(mixer_set_vol_rate_h) && OF_SVC->mixer_set_vol_rate_h)
        OF_SVC->mixer_set_vol_rate_h(handle, rate);
}

static inline uint32_t of_mixer_poll_ended(void) {
    return OF_SVC->mixer_poll_ended();
}

static inline uint32_t of_mixer_poll_ended_h(of_mixer_handle_t *out_handles,
                                             uint32_t max_handles) {
    if (OF_SVC_HAS_FIELD(mixer_poll_ended_h) && OF_SVC->mixer_poll_ended_h)
        return OF_SVC->mixer_poll_ended_h(out_handles, max_handles);
    return 0;
}

/* DEPRECATED legacy allocator.  New code MUST use malloc/free — the mixer
 * can DMA from any SDRAM buffer.  These remain only for older apps that use
 * of_mixer_free_samples() as a bulk reset; do not use in new code. */
static inline void *of_mixer_alloc_samples(size_t size) {
    return OF_SVC->mixer_alloc_samples((uint32_t)size);
}

static inline void of_mixer_free_samples(void) {
    OF_SVC->mixer_free_samples();
}

/* Register a callback invoked when any mixer voice finishes playing.
 * Callback receives bitmask of ended voices. Pass NULL to disable.
 * Callback runs in kernel context — keep it short. */
static inline void of_mixer_set_end_callback(void (*cb)(uint32_t ended_mask)) {
    OF_SVC->mixer_set_end_callback(cb);
}

/* Volume group constants live above the OF_PC fence — see top of file. */
static inline void of_mixer_set_group(int voice, int group) {
    OF_SVC->mixer_set_group(voice, group);
}

static inline void of_mixer_set_group_volume(int group, int volume) {
    OF_SVC->mixer_set_group_volume(group, volume);
}

static inline void of_mixer_set_master_volume(int volume) {
    OF_SVC->mixer_set_master_volume(volume);
}

/* The retired per-voice SVF filter API (of_mixer_set_filter/_h) was removed:
 * the mixer HW has no state-variable filter, so the calls were pure no-ops
 * with no callers.  The of_services_table mixer_set_filter / mixer_set_filter_h
 * slots were deleted too, so this is a services-ABI revision — all app
 * binaries must be rebuilt against this header. */

/* Group-aware atomic alloc-and-tag.  MUSIC scans low→high, SFX (and
 * untagged/other groups) scans high→low so the two groups land at
 * opposite ends of the slot range under low load.  Steal path prefers
 * a same-group victim before crossing groups, so a busy MUSIC track
 * can't silence an in-flight SFX (and vice versa) while its own
 * lower-priority voices are still around to give up.  Returns voice
 * index, or -1 if every slot is taken by an equal-or-higher priority
 * voice.  Falls back to of_mixer_play + of_mixer_set_group on older
 * firmware where the OF_SVC entry is absent. */
static inline int of_mixer_alloc_for_group(int group, const uint8_t *pcm_s16,
                                           uint32_t sample_count,
                                           uint32_t sample_rate,
                                           int priority, int volume) {
    if (OF_SVC_HAS_FIELD(mixer_alloc_for_group) && OF_SVC->mixer_alloc_for_group)
        return OF_SVC->mixer_alloc_for_group(group, pcm_s16, sample_count,
                                             sample_rate, priority, volume);
    int v = OF_SVC->mixer_play(pcm_s16, sample_count, sample_rate,
                               priority, volume);
    if (v >= 0) OF_SVC->mixer_set_group(v, group);
    return v;
}

static inline of_mixer_handle_t
of_mixer_alloc_for_group_h(int group, const uint8_t *pcm_s16,
                           uint32_t sample_count,
                           uint32_t sample_rate,
                           int priority, int volume) {
    if (OF_SVC_HAS_FIELD(mixer_alloc_for_group_h) && OF_SVC->mixer_alloc_for_group_h)
        return OF_SVC->mixer_alloc_for_group_h(group, pcm_s16, sample_count,
                                               sample_rate, priority, volume);
    return OF_MIXER_HANDLE_INVALID;
}

/* Read the group tag for a voice slot.  Used by ISR-context callers
 * (e.g. the SW MIDI envelope tick) to validate that a slot they
 * allocated previously still belongs to their group before writing
 * to it — if it has been reassigned, the caller drops its stale
 * reference rather than corrupting the new owner.  Returns -1 if
 * the voice index is out of range, or on older firmware where the
 * OF_SVC entry is absent (treat that as "ownership unknown"). */
static inline int of_mixer_voice_group(int voice) {
    if (OF_SVC_HAS_FIELD(mixer_voice_group) && OF_SVC->mixer_voice_group)
        return OF_SVC->mixer_voice_group(voice);
    return -1;
}

#else /* OF_PC */

#include <stdlib.h>

static inline void of_mixer_init(int max_voices, int output_rate) {
    (void)max_voices; (void)output_rate;
}
static inline int of_mixer_play(const uint8_t *pcm_s16, uint32_t sample_count,
                                uint32_t sample_rate, int priority, int volume) {
    (void)pcm_s16; (void)sample_count; (void)sample_rate;
    (void)priority; (void)volume;
    return -1;
}
static inline int of_mixer_play_8bit(const uint8_t *pcm_s8, uint32_t sample_count,
                                     uint32_t sample_rate, int priority, int volume) {
    (void)pcm_s8; (void)sample_count; (void)sample_rate;
    (void)priority; (void)volume;
    return -1;
}
static inline void of_mixer_stop(int voice) { (void)voice; }
static inline void of_mixer_stop_all(void) {}
static inline void of_mixer_set_volume(int voice, int volume) {
    (void)voice; (void)volume;
}
static inline void of_mixer_pump(void) {}
static inline int of_mixer_voice_active(int voice) { (void)voice; return 0; }
static inline void of_mixer_set_pan(int voice, int pan) {
    (void)voice; (void)pan;
}
static inline void of_mixer_set_loop(int voice, int loop_start, int loop_end) {
    (void)voice; (void)loop_start; (void)loop_end;
}
static inline void of_mixer_set_rate(int voice, int sample_rate_hz) {
    (void)voice; (void)sample_rate_hz;
}
static inline void of_mixer_set_vol_lr(int voice, int vol_l, int vol_r) {
    (void)voice; (void)vol_l; (void)vol_r;
}
static inline void of_mixer_set_bidi(int voice, int enable) {
    (void)voice; (void)enable;
}
static inline int of_mixer_get_position(int voice) { (void)voice; return 0; }
static inline void of_mixer_set_position(int voice, int sample_offset) {
    (void)voice; (void)sample_offset;
}
static inline void of_mixer_set_voice(int voice, int sample_rate_hz,
                                      int vol_l, int vol_r) {
    (void)voice; (void)sample_rate_hz; (void)vol_l; (void)vol_r;
}
static inline void of_mixer_set_rate_raw(int voice, uint32_t rate_fp16) {
    (void)voice; (void)rate_fp16;
}
static inline void of_mixer_set_voice_raw(int voice, uint32_t rate_fp16,
                                          int vol_l, int vol_r) {
    (void)voice; (void)rate_fp16; (void)vol_l; (void)vol_r;
}
static inline void of_mixer_set_volume_ramp(int voice, int rate) {
    (void)voice; (void)rate;
}
static inline uint32_t of_mixer_poll_ended(void) { return 0; }
static inline void *of_mixer_alloc_samples(size_t size) { return malloc(size); }
static inline void of_mixer_free_samples(void) { /* no-op on PC */ }
static inline void of_mixer_set_group(int voice, int group) {
    (void)voice; (void)group;
}
static inline void of_mixer_set_group_volume(int group, int volume) {
    (void)group; (void)volume;
}
static inline void of_mixer_set_master_volume(int volume) {
    (void)volume;
}
static inline void of_mixer_retrigger(int voice, const uint8_t *pcm_s16,
                                      uint32_t sample_count, uint32_t sample_rate,
                                      int volume) {
    (void)voice; (void)pcm_s16; (void)sample_count;
    (void)sample_rate; (void)volume;
}
static inline void of_mixer_set_end_callback(void (*cb)(uint32_t ended_mask)) {
    (void)cb;
}
static inline int of_mixer_alloc_for_group(int group, const uint8_t *pcm_s16,
                                           uint32_t sample_count,
                                           uint32_t sample_rate,
                                           int priority, int volume) {
    (void)group; (void)pcm_s16; (void)sample_count;
    (void)sample_rate; (void)priority; (void)volume;
    return -1;
}
static inline int of_mixer_voice_group(int voice) { (void)voice; return -1; }

static inline of_mixer_handle_t
of_mixer_play_h(const uint8_t *pcm_s16, uint32_t sample_count,
                uint32_t sample_rate, int priority, int volume) {
    (void)pcm_s16; (void)sample_count; (void)sample_rate;
    (void)priority; (void)volume;
    return OF_MIXER_HANDLE_INVALID;
}
static inline of_mixer_handle_t
of_mixer_play_8bit_h(const uint8_t *pcm_s8, uint32_t sample_count,
                     uint32_t sample_rate, int priority, int volume) {
    (void)pcm_s8; (void)sample_count; (void)sample_rate;
    (void)priority; (void)volume;
    return OF_MIXER_HANDLE_INVALID;
}
static inline of_mixer_handle_t
of_mixer_alloc_for_group_h(int group, const uint8_t *pcm_s16,
                           uint32_t sample_count, uint32_t sample_rate,
                           int priority, int volume) {
    (void)group; (void)pcm_s16; (void)sample_count;
    (void)sample_rate; (void)priority; (void)volume;
    return OF_MIXER_HANDLE_INVALID;
}
static inline of_mixer_handle_t
of_mixer_retrigger_h(of_mixer_handle_t handle, const uint8_t *pcm_s16,
                     uint32_t sample_count, uint32_t sample_rate, int volume) {
    (void)handle; (void)pcm_s16; (void)sample_count; (void)sample_rate; (void)volume;
    return OF_MIXER_HANDLE_INVALID;
}
static inline void of_mixer_stop_h(of_mixer_handle_t handle) { (void)handle; }
static inline int of_mixer_handle_active(of_mixer_handle_t handle) { (void)handle; return 0; }
static inline int of_mixer_handle_group(of_mixer_handle_t handle) { (void)handle; return -1; }
static inline int of_mixer_handle_voice(of_mixer_handle_t handle) { (void)handle; return -1; }
static inline void of_mixer_set_volume_h(of_mixer_handle_t handle, int volume) {
    (void)handle; (void)volume;
}
static inline void of_mixer_set_pan_h(of_mixer_handle_t handle, int pan) {
    (void)handle; (void)pan;
}
static inline void of_mixer_set_loop_h(of_mixer_handle_t handle, int loop_start, int loop_end) {
    (void)handle; (void)loop_start; (void)loop_end;
}
static inline void of_mixer_set_rate_h(of_mixer_handle_t handle, int sample_rate_hz) {
    (void)handle; (void)sample_rate_hz;
}
static inline void of_mixer_set_rate_raw_h(of_mixer_handle_t handle, uint32_t rate_fp16) {
    (void)handle; (void)rate_fp16;
}
static inline void of_mixer_set_vol_lr_h(of_mixer_handle_t handle, int vol_l, int vol_r) {
    (void)handle; (void)vol_l; (void)vol_r;
}
static inline void of_mixer_set_bidi_h(of_mixer_handle_t handle, int enable) {
    (void)handle; (void)enable;
}
static inline int of_mixer_get_position_h(of_mixer_handle_t handle) {
    (void)handle; return -1;
}
static inline void of_mixer_set_position_h(of_mixer_handle_t handle, int sample_offset) {
    (void)handle; (void)sample_offset;
}
static inline void of_mixer_set_voice_h(of_mixer_handle_t handle,
                                        int sample_rate_hz,
                                        int vol_l, int vol_r) {
    (void)handle; (void)sample_rate_hz; (void)vol_l; (void)vol_r;
}
static inline void of_mixer_set_voice_raw_h(of_mixer_handle_t handle,
                                            uint32_t rate_fp16,
                                            int vol_l, int vol_r) {
    (void)handle; (void)rate_fp16; (void)vol_l; (void)vol_r;
}
static inline void of_mixer_set_volume_ramp_h(of_mixer_handle_t handle, int rate) {
    (void)handle; (void)rate;
}
static inline uint32_t of_mixer_poll_ended_h(of_mixer_handle_t *out_handles,
                                             uint32_t max_handles) {
    (void)out_handles; (void)max_handles;
    return 0;
}

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_MIXER_H */
