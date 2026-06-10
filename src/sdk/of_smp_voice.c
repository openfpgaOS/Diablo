//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_smp_voice.c -- Sample voice engine for sample-based MIDI with
 *                   DAHDSR envelopes, dual LFOs, and pitch bend.
 *
 * Hardware-only: the voice engine drives the HW mixer through OF_SVC.
 * On OF_PC the file collapses to silent stubs at the bottom so apps
 * that pull it in for desktop testing still link.
 */

#ifndef OF_PC

#include "include/of_smp_voice.h"
#include "include/of_smp_bank.h"
#include "include/of_smp_tables.h"
#include "include/of_mixer.h"
#include "include/of_cache.h"
#include "include/of_timer.h"
#include "include/of_services.h"
#include "include/of_fastram.h"
#include <string.h>

#ifndef SMP_VOICE_ENABLE_TICK_STATS
#define SMP_VOICE_ENABLE_TICK_STATS 0
#endif

/* ------------------------------------------------------------------ */
/* Static state                                                       */
/* ------------------------------------------------------------------ */

/* Voice + channel state pinned to BRAM (OF_FASTDATA → .app_fastdata for
 * SDK apps, .fastdata for OS) — smp_voice_tick runs from the 1 kHz
 * timer ISR and writes these every tick.  ISR SDRAM stores race with
 * GPU/bridge bus traffic (see fence comment in start.S::_trap_entry);
 * pinning to BRAM breaks the race. */

static OF_FASTDATA smp_voice_t voices[SMP_MAX_VOICES];
static OF_FASTDATA uint32_t    tick_counter;
/* Count of voices currently marked STEAL_PENDING.  voice_cleanup_stolen()
 * runs every tick; this lets it skip the full SMP_MAX_VOICES scan in the
 * common case (nothing pending).  Only voice_force_off() sets STEAL_PENDING
 * (and bumps this); voice_cleanup_stolen() clears them all and zeroes it. */
static OF_FASTDATA uint8_t     steal_pending_count;

/* ------------------------------------------------------------------ */
/* Tick-cost probe (Task #10)                                         */
/* ------------------------------------------------------------------ */
/* NOTE: VexiiRiscv here does not expose rdcycle to user mode, so we use
 * OF_SVC->timer_get_us() (direct service-table call — NOT the ecall
 * of_time_us(), which would nest-trap when smp_voice_tick runs from
 * the MIDI timer ISR). Stats are in microseconds. */

/* Stats are incremented from the timer ISR — keep in BRAM with the rest
 * of the ISR-touched state so we don't reintroduce the SDRAM race. */
static OF_FASTDATA uint32_t tick_us_max;
static OF_FASTDATA uint32_t tick_us_last;
static OF_FASTDATA uint32_t tick_spike_count;
static OF_FASTDATA uint32_t tick_stat_count;
static OF_FASTDATA uint8_t  tick_active_peak;
static OF_FASTDATA uint8_t  tick_stage_sustain;
static OF_FASTDATA uint8_t  tick_stage_release;
static OF_FASTDATA uint8_t  tick_stage_decay;
static OF_FASTDATA uint8_t  tick_sustain_held;
static OF_FASTDATA uint8_t  tick_ch_active[16];

/* A/B/C instrumentation counters — see smp_tick_stats_t for descriptions.
 * Incremented at actual HW-write sites (post-cache) and from
 * smp_voice_tick_record_pump(), then snapshotted by get_stats and zeroed
 * by reset_stats.  All in BRAM: the writes happen in the ISR. */
static OF_FASTDATA uint32_t stat_rate_writes;
static OF_FASTDATA uint32_t stat_vol_writes;
static OF_FASTDATA uint32_t stat_pump_count;
static OF_FASTDATA uint32_t stat_pump_interval_max_us;
static OF_FASTDATA uint32_t stat_pump_interval_min_us = 0xFFFFFFFFu;
static OF_FASTDATA uint32_t stat_pump_burst_count;
static OF_FASTDATA uint32_t stat_pump_budget_exceeded;

/* A single 1 kHz voice tick should stay comfortably below the pump cap. */
#define SMP_TICK_SPIKE_US  2000u

void smp_voice_tick_get_stats(smp_tick_stats_t *out)
{
    if (!out) return;
    /* Note: field is named cycles_* for ABI stability but holds microseconds. */
    out->cycles_max    = tick_us_max;
    out->cycles_last   = tick_us_last;
    out->spike_count   = tick_spike_count;
    out->tick_count    = tick_stat_count;
    out->active_peak   = tick_active_peak;
    out->stage_sustain = tick_stage_sustain;
    out->stage_release = tick_stage_release;
    out->stage_decay   = tick_stage_decay;
    out->sustain_held  = tick_sustain_held;
    for (int i = 0; i < 16; i++)
        out->ch_active[i] = tick_ch_active[i];

    out->filter_writes         = 0;
    out->rate_writes           = stat_rate_writes;
    out->vol_writes            = stat_vol_writes;
    out->pump_count            = stat_pump_count;
    out->pump_interval_max_us  = stat_pump_interval_max_us;
    out->pump_interval_min_us  = stat_pump_interval_min_us;
    out->pump_burst_count      = stat_pump_burst_count;
    out->pump_budget_exceeded  = stat_pump_budget_exceeded;
}

void smp_voice_tick_reset_stats(void)
{
    tick_us_max      = 0;
    tick_spike_count = 0;
    tick_stat_count  = 0;
    tick_active_peak = 0;

    stat_rate_writes          = 0;
    stat_vol_writes           = 0;
    stat_pump_count           = 0;
    stat_pump_interval_max_us = 0;
    stat_pump_interval_min_us = 0xFFFFFFFFu;
    stat_pump_burst_count     = 0;
    stat_pump_budget_exceeded = 0;
}

void smp_voice_tick_record_pump(uint32_t elapsed_us, int ticks_fired,
                                int budget_exceeded)
{
    stat_pump_count++;
    if (elapsed_us > stat_pump_interval_max_us)
        stat_pump_interval_max_us = elapsed_us;
    if (elapsed_us < stat_pump_interval_min_us)
        stat_pump_interval_min_us = elapsed_us;
    if (ticks_fired > 1) stat_pump_burst_count++;
    if (budget_exceeded) stat_pump_budget_exceeded++;
}

/* Per-channel state (16 MIDI channels).  These stay in BRAM because the
 * 1 kHz MIDI ISR reads them for every active voice, but most are native
 * MIDI 7-bit values and do not need 32-bit storage. */
static OF_FASTDATA uint8_t ch_volume[16];        /* CC7  (0-127) */
static OF_FASTDATA uint8_t ch_expression[16];    /* CC11 (0-127) */
static OF_FASTDATA uint8_t ch_pan[16];           /* CC10 (0-127, 64=center) */
static OF_FASTDATA int16_t ch_bend[16];          /* -8192..+8191 */
static OF_FASTDATA int16_t ch_bend_cents[16];    /* pitch-bend contribution in cents */
static OF_FASTDATA uint8_t ch_mod_depth[16];     /* CC1  (0-127) */
static OF_FASTDATA uint8_t ch_sustain[16];       /* CC64 on/off */
static OF_FASTDATA uint8_t ch_brightness[16];    /* CC74 (0-127) */
static OF_FASTDATA uint8_t ch_resonance[16];     /* CC71 (0-127) */
static OF_FASTDATA uint8_t ch_reverb_send[16];   /* CC91 (0-127), default 40 = GM tasteful default */
static OF_FASTDATA uint8_t ch_chorus_send[16];   /* CC93 (0-127), default 0 */
static OF_FASTDATA uint8_t ch_vol_combined[16];  /* (CC7 * CC11) / 127, cached */
static OF_FASTDATA int16_t ch_pan_midi[16];      /* CC10 mapped to roughly -507..+500 */
static OF_FASTDATA int master_vol = 255;

/* Cached mixer state to avoid redundant CDC writes */
static OF_FASTDATA uint32_t prev_rate[SMP_MAX_VOICES];
static OF_FASTDATA uint8_t  prev_vol_l[SMP_MAX_VOICES];
static OF_FASTDATA uint8_t  prev_vol_r[SMP_MAX_VOICES];

/* Voices pending steal (waiting for hardware fade-out) */
#define STEAL_PENDING -2

/* Minimum envelope level before we consider it done */
#define ENV_FLOOR 0x100

/* Pitch bend range in cents (standard: +/-2 semitones) */
#define BEND_RANGE_CENTS 200

/* ------------------------------------------------------------------ */
/* Fixed-point helpers                                                */
/* ------------------------------------------------------------------ */

static int32_t triangle_wave(int32_t phase)
{
    /* phase is Q16.16 wrapping at 0x10000.
     * Output: -0x10000 .. +0x10000 (Q16.16 signed) */
    phase &= 0xFFFF;
    if (phase < 0x4000)
        return (phase << 2);                    /* 0..0x10000 */
    else if (phase < 0xC000)
        return 0x20000 - (phase << 2);          /* 0x10000..-0x10000 */
    else
        return (phase << 2) - 0x40000;          /* -0x10000..0 */
}

static int clamp_midi7(int v)
{
    if (v < 0) return 0;
    if (v > 127) return 127;
    return v;
}

/* 128-entry velocity→gain table, baked once in smp_voice_init().  note_on
 * runs from the MIDI ISR, so this replaces the per-note-on curve math
 * (multiply + divide + blend) with a single BRAM read. */
static OF_FASTDATA uint8_t vel_gain_lut[128];

static int midi_velocity_gain_compute(int velocity)
{
    int v = clamp_midi7(velocity);
    if (v <= 0) return 0;

    int linear = (v << 1) + 1;
    if (linear > 255) linear = 255;

    /* Gentle perceptual lift for low/mid MIDI velocities.  A pure sqrt-like
     * curve makes soft notes too loud; this keeps 75% of the old linear
     * response and blends in 25% of a quadratic ease-out curve.
     *
     * Examples:
     *   v=32:  65 ->  77
     *   v=64: 129 -> 145
     *   v=96: 193 -> 205
     *   v=127:       255
     */
    int inv = 127 - v;
    int ease = 255 - ((inv * inv * 255 + (127 * 127 / 2)) / (127 * 127));
    int gain = ((linear * 3) + ease + 2) >> 2;
    if (gain > 255) gain = 255;
    if (gain < 0) gain = 0;
    return gain;
}

/* ------------------------------------------------------------------ */
/* Envelope helpers                                                   */
/* ------------------------------------------------------------------ */

/* All SF2-unit conversions are pre-baked into the OFSF v3 zone
 * (ticks, per-tick rates, and Q16.16 sustain levels) by sf2_to_ofsf.
 * env_init / env_advance just copy those baked fields into the per-voice
 * env_state_t; the arithmetic is identical to what this code used to do
 * at every transition but happens once offline instead of per voice. */

static void env_init(env_state_t *e, uint32_t delay_ticks,
                     uint32_t attack_rate)
{
    e->level = 0;
    e->target = 0;

    if (delay_ticks > 0) {
        e->stage = ENV_DELAY;
        e->timer = (int32_t)delay_ticks;
        e->rate = 0;
        return;
    }

    e->stage = ENV_ATTACK;
    e->rate = (int32_t)attack_rate;
    e->target = 0x10000;
    e->timer = 0;
}

static void env_advance(env_state_t *e, const ofsf_zone_t *z, int is_vol)
{
    switch (e->stage) {
    case ENV_OFF:
    case ENV_DONE:
        return;

    case ENV_DELAY:
        if (--e->timer <= 0) {
            uint32_t atk_rate = is_vol ? z->vol_attack_rate : z->mod_attack_rate;
            e->stage = ENV_ATTACK;
            e->rate = (int32_t)atk_rate;
            e->target = 0x10000;
        }
        return;

    case ENV_ATTACK:
        e->level += e->rate;
        if (e->level >= 0x10000) {
            e->level = 0x10000;
            uint32_t hold_ticks = is_vol ? z->vol_hold_ticks : z->mod_hold_ticks;
            if (hold_ticks > 0) {
                e->stage = ENV_HOLD;
                e->timer = (int32_t)hold_ticks;
                e->rate = 0;
            } else {
                goto start_decay;
            }
        }
        return;

    case ENV_HOLD:
        if (--e->timer <= 0) {
start_decay: ;
            uint32_t sus_level  = is_vol ? z->vol_sustain_level : z->mod_sustain_level;
            uint32_t decay_rate = is_vol ? z->vol_decay_rate    : z->mod_decay_rate;
            e->stage = ENV_DECAY;
            e->target = (int32_t)sus_level;
            e->rate = (int32_t)decay_rate;
        }
        return;

    case ENV_DECAY:
        e->level -= e->rate;
        if (e->level <= e->target) {
            e->level = e->target;
            e->stage = ENV_SUSTAIN;
            e->rate = 0;
        }
        return;

    case ENV_SUSTAIN:
        return;

    case ENV_RELEASE:
        e->level -= e->rate;
        if (e->level <= ENV_FLOOR) {
            e->level = 0;
            e->stage = ENV_DONE;
        }
        return;
    }
}

static void env_start_release(env_state_t *e, uint32_t release_ticks)
{
    if (e->stage == ENV_OFF || e->stage == ENV_DONE)
        return;

    if (release_ticks < 1) release_ticks = 1;

    e->stage = ENV_RELEASE;
    e->target = 0;
    e->rate = e->level / (int32_t)release_ticks;
    if (e->rate < 1) e->rate = 1;
}

/* ------------------------------------------------------------------ */
/* LFO helpers                                                        */
/* ------------------------------------------------------------------ */

static void lfo_init(lfo_state_t *l, uint32_t delay_ticks, uint32_t rate)
{
    /* Delay ticks and per-tick phase increment are pre-baked in the
     * OFSF v3 zone (smp_timecents_to_ticks + smp_lfo_freq_cents_to_rate
     * applied offline by sf2_to_ofsf).  No conversion needed here. */
    l->phase = 0;
    l->delay_ticks = (int32_t)delay_ticks;
    l->rate = (int32_t)rate;
    if (l->rate < 1) l->rate = 1;
}

static int32_t lfo_advance(lfo_state_t *l)
{
    if (l->delay_ticks > 0) {
        l->delay_ticks--;
        return 0;
    }
    l->phase += l->rate;
    l->phase &= 0xFFFF;
    return triangle_wave(l->phase);
}

/* ------------------------------------------------------------------ */
/* Voice allocation                                                   */
/* ------------------------------------------------------------------ */

static void voice_force_off(int idx);
static void voice_cleanup_stolen(void);

static int voice_hw_owned_by_music(const smp_voice_t *v)
{
    if (v->mixer_voice == OF_MIXER_HANDLE_INVALID)
        return 0;
    /* of_mixer_handle_group() validates the handle against active_shadow +
     * generation and returns -1 for an invalid / inactive / stolen handle, so
     * this single NON-reaping query replaces the old handle_active +
     * handle_group pair (one mixer call per voice instead of two).  The HW
     * voice-end queue is reaped once per tick at the top of smp_voice_tick()
     * instead of once per voice here — see the reap there. */
    int group = of_mixer_handle_group(v->mixer_voice);
    if (group < 0)
        return 0;
    return group == OF_MIXER_GROUP_MUSIC;
}

static void voice_stop_hw_if_owned(smp_voice_t *v)
{
    if (voice_hw_owned_by_music(v))
        of_mixer_stop_h(v->mixer_voice);
}

static int voice_drop_if_stale(smp_voice_t *v)
{
    if (voice_hw_owned_by_music(v))
        return 0;

    v->active = 0;
    v->mixer_voice = OF_MIXER_HANDLE_INVALID;
    return 1;
}

/* Reclaim a slot for immediate reuse: free the hardware mixer voice and
 * mark the slot inactive.  voice_alloc's steal passes call this so the
 * caller (smp_voice_note_on) can write fresh state without leaking the
 * previous hardware voice. */
static void voice_reclaim(int idx)
{
    smp_voice_t *v = &voices[idx];
    voice_stop_hw_if_owned(v);
    v->mixer_voice = OF_MIXER_HANDLE_INVALID;
    v->active = 0;
}

static int voice_alloc(void)
{
    /* Pass 1: find a free slot */
    for (int i = 0; i < SMP_MAX_VOICES; i++) {
        if (!voices[i].active)
            return i;
    }

    /* Pass 2: steal ENV_DONE (oldest first).  Skip STEAL_PENDING slots —
     * voice_cleanup_stolen owns those and will free them next tick. */
    int best = -1;
    uint32_t best_age = UINT32_MAX;
    for (int i = 0; i < SMP_MAX_VOICES; i++) {
        if (voices[i].active != STEAL_PENDING &&
            voices[i].vol_env.stage == ENV_DONE && voices[i].age < best_age) {
            best = i;
            best_age = voices[i].age;
        }
    }
    if (best >= 0) {
        voice_reclaim(best);
        return best;
    }

    /* Pass 3: steal the quietest ENV_RELEASE voice (lowest envelope
     * level).  Stealing by audible-level minimizes the click from the
     * hard stop in voice_reclaim — a voice already near-silent fades
     * to zero with no perceptible discontinuity.  "Oldest by age" was
     * a poor proxy because per-zone release rates differ widely (a
     * drum with 50 ms release started 2 s ago is silent; a piano with
     * 3 s release started 2 s ago is still loud). */
    int32_t best_level = INT32_MAX;
    for (int i = 0; i < SMP_MAX_VOICES; i++) {
        if (voices[i].active != STEAL_PENDING &&
            voices[i].vol_env.stage == ENV_RELEASE &&
            voices[i].vol_env.level < best_level) {
            best = i;
            best_level = voices[i].vol_env.level;
        }
    }
    if (best >= 0) {
        voice_reclaim(best);
        return best;
    }

    /* Pass 4: steal the quietest voice of any stage (last resort).
     * Same rationale as pass 3 — steal whatever is least audible. */
    best_level = INT32_MAX;
    for (int i = 0; i < SMP_MAX_VOICES; i++) {
        if (voices[i].active != STEAL_PENDING &&
            voices[i].vol_env.level < best_level) {
            best = i;
            best_level = voices[i].vol_env.level;
        }
    }
    if (best >= 0)
        voice_reclaim(best);

    return best;
}

/* Schedule a voice for shutdown without reusing its slot.  Used by
 * kill_exclusive_class — the new note allocates a fresh slot and the
 * old one fades out via voice_cleanup_stolen on the next tick.
 *
 * Ramp rate must be high enough that the HW vol_lr reaches 0 BEFORE
 * voice_cleanup_stolen fires 1 ms later and snaps vol_lr/ctrl to 0
 * (otherwise the snap from non-zero to 0 is an audible click).  At
 * 48 kHz audio, 1 ms = 48 ramp steps; rate=16 → fade in 16 samples
 * (~0.33 ms), well under the 1 ms cleanup gap.  Old rate=4 needed
 * 64 samples (~1.33 ms) to fade — finished AFTER cleanup, leaving
 * ~63 of 255 LSBs to be snapped, ~25% full-scale step, audible. */
static void voice_force_off(int idx)
{
    smp_voice_t *v = &voices[idx];
    if (voice_drop_if_stale(v))
        return;

    of_mixer_set_vol_lr_h(v->mixer_voice, 0, 0);
    of_mixer_set_volume_ramp_h(v->mixer_voice, 16);
    v->active = STEAL_PENDING;
    steal_pending_count++;
}

static void voice_cleanup_stolen(void)
{
    /* Common case: nothing pending — skip the full scan. */
    if (steal_pending_count == 0)
        return;
    for (int i = 0; i < SMP_MAX_VOICES; i++) {
        if (voices[i].active == STEAL_PENDING) {
            voice_stop_hw_if_owned(&voices[i]);
            voices[i].active = 0;
            voices[i].mixer_voice = OF_MIXER_HANDLE_INVALID;
        }
    }
    steal_pending_count = 0;
}

/* ------------------------------------------------------------------ */
/* Exclusive class                                                    */
/* ------------------------------------------------------------------ */

static void kill_exclusive_class(int midi_ch, uint8_t excl_class)
{
    if (excl_class == 0) return;
    for (int i = 0; i < SMP_MAX_VOICES; i++) {
        smp_voice_t *v = &voices[i];
        if (v->active && v->active != STEAL_PENDING &&
            v->midi_ch == midi_ch && v->zone &&
            v->zone->exclusive_class == excl_class) {
            voice_force_off(i);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Volume / pan / pitch computation                                   */
/* ------------------------------------------------------------------ */

/* Q0.PAN_SHIFT fixed-point for the cached pan multipliers.  PAN_SHIFT=16
 * keeps vol(<=255) * pan_mul(<=1<<16) well inside int32. */
#define PAN_SHIFT 16

/* Recompute a voice's cached L/R pan multipliers from its (constant) zone pan
 * and the current channel CC10.  Called at note-on and whenever CC10 changes,
 * so the per-tick hot path never divides.  The equal-volume law is unchanged:
 *   pan -500: L=vol, R=0 ;  pan 0: L=R=vol ;  pan +500: L=0, R=vol
 * One side stays full-scale; the other carries (500-|pan|)/500 as a reciprocal
 * multiply (matches the old integer divide to within 1 LSB). */
static void voice_recompute_pan(smp_voice_t *v)
{
    int zone_pan = v->zone ? v->zone->pan : 0;
    int pan = zone_pan + ch_pan_midi[v->midi_ch];
    if (pan < -500) pan = -500;
    if (pan > 500)  pan = 500;

    const int32_t full = (int32_t)1 << PAN_SHIFT;
    if (pan <= 0) {
        v->pan_mul_l = full;
        v->pan_mul_r = (int32_t)(((int64_t)(500 + pan) << PAN_SHIFT) / 500);
    } else {
        v->pan_mul_l = (int32_t)(((int64_t)(500 - pan) << PAN_SHIFT) / 500);
        v->pan_mul_r = full;
    }
}

static void compute_vol_lr(smp_voice_t *v, int *out_l, int *out_r)
{
    /* env_vol: Q16.16 -> 0..256 */
    int32_t env_vol = v->vol_env.level >> 8;
    if (env_vol > 255) env_vol = 255;
    if (env_vol < 0)   env_vol = 0;

    int ch = v->midi_ch;

    /* Design-doc compose: VOICE_BASE_VOL × RAMP0_LEVEL × CH_VOL × CH_EXPR × MASTER.
     * voice_base_vol (0..255) = (vel_scale × initial_attn_scale) >> 8, baked at
     * note-on so this function does one less multiply per tick. */
    int32_t vol = env_vol;
    vol = (vol * v->voice_base_vol) >> 8;
    vol = (vol * ch_vol_combined[ch]) >> 7;
    vol = (vol * master_vol) >> 8;
    if (vol > 255) vol = 255;
    if (vol < 0)   vol = 0;

    /* Pan applied via the cached per-voice multipliers (see
     * voice_recompute_pan) — two multiplies + shift, no divide, no clamp. */
    *out_l = (int)((vol * v->pan_mul_l) >> PAN_SHIFT);
    *out_r = (int)((vol * v->pan_mul_r) >> PAN_SHIFT);
}

/* filter_update retired in v3 — the mixer RTL has no SVF, so the
 * cents→Q0.16 conversion + redundant-write skip + delta tracking
 * was producing zero audible effect.  Each tick was paying ~50–100
 * cycles per active voice for math whose only consumer was the
 * since-removed no-op of_mixer_set_filter().  If SVF returns to
 * the RTL, reintroduce a runtime cap-gated path. */

/* Apply a cents offset to a voice's base playback rate.  Shared by the fast
 * (bend-only) and slow (LFO / mod-env) paths of compute_pitch so the clamp +
 * multiplier lookup + Q16.16 multiply lives in exactly one place. */
static inline uint32_t pitch_from_cents(uint32_t base_rate_fp16, int32_t cents_offset)
{
    if (cents_offset == 0)
        return base_rate_fp16;
    if (cents_offset > 12000)  cents_offset = 12000;
    if (cents_offset < -12000) cents_offset = -12000;
    uint32_t mult = smp_cents_to_multiplier(cents_offset);
    return (uint32_t)(((uint64_t)base_rate_fp16 * mult) >> 16);
}

static uint32_t compute_pitch(smp_voice_t *v)
{
    int ch = v->midi_ch;
    const ofsf_zone_t *z = v->zone;
    int32_t cents_offset = ch_bend_cents[ch];

    if (!z)
        return v->base_rate_fp16;

    if (z->vib_lfo_to_pitch == 0 &&
        z->mod_env_to_pitch == 0 &&
        (z->mod_lfo_to_pitch == 0 || ch_mod_depth[ch] == 0)) {
        return pitch_from_cents(v->base_rate_fp16, cents_offset);
    }

    /* Vibrato LFO */
    if (z->vib_lfo_to_pitch != 0) {
        int32_t lfo_out = triangle_wave(v->vib_lfo.phase);
        cents_offset += (lfo_out * z->vib_lfo_to_pitch) >> 16;
    }

    /* Mod LFO to pitch */
    if (z->mod_lfo_to_pitch != 0) {
        int32_t lfo_out = triangle_wave(v->mod_lfo.phase);
        int32_t depth = z->mod_lfo_to_pitch;
        /* Scale by CC1 mod wheel */
        depth = (depth * ch_mod_depth[ch]) / 127;
        cents_offset += (lfo_out * depth) >> 16;
    }

    /* Mod envelope to pitch */
    if (z->mod_env_to_pitch != 0) {
        cents_offset += ((int64_t)v->mod_env.level * z->mod_env_to_pitch) >> 16;
    }

    return pitch_from_cents(v->base_rate_fp16, cents_offset);
}

static void channel_recompute_cached(int ch)
{
    ch_vol_combined[ch] = (ch_volume[ch] * ch_expression[ch]) / 127;
    ch_pan_midi[ch] = ((ch_pan[ch] - 64) * 500) / 63;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void smp_voice_init(void)
{
    for (int v = 0; v < 128; v++)
        vel_gain_lut[v] = (uint8_t)midi_velocity_gain_compute(v);

    for (int i = 0; i < SMP_MAX_VOICES; i++) {
        voices[i].active = 0;
        voices[i].mixer_voice = OF_MIXER_HANDLE_INVALID;
    }

    for (int i = 0; i < 16; i++) {
        ch_volume[i]     = 100;
        ch_expression[i] = 127;
        ch_pan[i]        = 64;
        ch_bend[i]       = 0;
        ch_bend_cents[i] = 0;
        ch_mod_depth[i]  = 0;
        ch_sustain[i]    = 0;
        ch_brightness[i] = 64;
        ch_resonance[i]  = 0;
        ch_reverb_send[i] = 40;   /* GM tasteful default ~31 % wet */
        ch_chorus_send[i] = 0;
        channel_recompute_cached(i);
    }

    master_vol = 255;
    tick_counter = 0;
    steal_pending_count = 0;
}

int smp_voice_note_on(const ofsf_zone_t *zone, int midi_ch, int note,
                      int velocity, const void *sample_base)
{
    if (!zone || midi_ch < 0 || midi_ch > 15 || velocity <= 0)
        return -1;

    kill_exclusive_class(midi_ch, zone->exclusive_class);

    /* Same-channel same-note stealing: if this MIDI note is already
     * sounding on this channel (envelope hasn't reached ENV_DONE),
     * force-release the old voice before allocating a fresh one.
     * Without this, rapid retriggers of the same note (common for
     * sustained guitar/pad parts in Doom's MIDI) stack N copies of the
     * same pitch at slightly different envelope phases — sounds like
     * chorus/phaser/reverb instead of clean retrigger.  Drum channel
     * (ch 9) is skipped because percussion zones often WANT overlap
     * (hi-hat bounce etc.) and use SF2 exclusive_class for cutoffs
     * when needed. */
    if (midi_ch != 9) {
        for (int i = 0; i < SMP_MAX_VOICES; i++) {
            smp_voice_t *ov = &voices[i];
            if (ov->active && ov->active != STEAL_PENDING &&
                ov->midi_ch == midi_ch && ov->note == note &&
                ov->vol_env.stage != ENV_DONE) {
                env_start_release(&ov->vol_env, ov->zone->vol_release_ticks);
                env_start_release(&ov->mod_env, ov->zone->mod_release_ticks);
            }
        }
    }

    int idx = voice_alloc();
    if (idx < 0)
        return -1;

    smp_voice_t *v = &voices[idx];

    v->active = 1;
    v->zone = zone;
    v->midi_ch = (uint8_t)midi_ch;
    v->note = (uint8_t)note;
    v->velocity = (uint8_t)clamp_midi7(velocity);
    v->sustain_held = 0;
    v->mixer_voice = OF_MIXER_HANDLE_INVALID;
    v->age = tick_counter;

    /* Pre-bake voice_base_vol = (velocity_gain × initial_attn_scale) >> 8.
     * One u8 field now replaces the two multiplies the old compute_vol_lr
     * did per tick. */
    {
        int vel_scale = vel_gain_lut[clamp_midi7(velocity)];
        int attn_scale = zone ? zone->initial_attn_scale : 255;
        int bv = (vel_scale * attn_scale) >> 8;
        if (bv > 255) bv = 255;
        v->voice_base_vol = (uint8_t)bv;
    }

    /* Compute base playback rate:
     * rate = (sample_rate / 48000) * 2^((note - root + coarse)*100 + fine) / 1200)
     * We split into the sample_rate ratio and the pitch offset. */
    const ofsf_header_t *hdr = of_smp_bank_get();
    uint32_t sr = hdr ? hdr->sample_rate : 44100;
    uint32_t base_fp16 = OF_MIXER_RATE_FP16(sr);

    int32_t total_cents = ((int32_t)note - (int32_t)zone->root_key) * 100
                        + (int32_t)zone->coarse_tune * 100
                        + (int32_t)zone->fine_tune;
    uint32_t pitch_mult = smp_cents_to_multiplier(total_cents);
    v->base_rate_fp16 = (uint32_t)(((uint64_t)base_fp16 * pitch_mult) >> 16);

    /* Compute sample address. sample_base points to the start of the
     * SDRAM sample blob and sample_offset is in bytes from that base. */
    const uint8_t *sample_ptr = (const uint8_t *)sample_base
                              + zone->sample_offset;

    of_mixer_handle_t mhv = of_mixer_alloc_for_group_h(OF_MIXER_GROUP_MUSIC,
                                                       sample_ptr,
                                                       zone->sample_length,
                                                       sr, 0, 200);
    if (mhv == OF_MIXER_HANDLE_INVALID) { v->active = 0; return -1; }


    v->mixer_voice = mhv;
    of_mixer_set_rate_raw_h(mhv, v->base_rate_fp16);
    stat_rate_writes++;

    /* Loop setup */
    if (zone->loop_mode == OFSF_LOOP_FORWARD || zone->loop_mode == OFSF_LOOP_BIDI) {
        of_mixer_set_loop_h(mhv, zone->loop_start, zone->loop_end);
        if (zone->loop_mode == OFSF_LOOP_BIDI)
            of_mixer_set_bidi_h(mhv, 1);
        /* Looping voice: let the envelope decide when it ends. */
        v->sample_ticks_remaining = 0;
    } else {
        /* One-shot: compute how many software ticks (1 kHz) until the
         * sample walks off its natural end.
         *
         *   samples consumed per 48 kHz output tick = base_rate_fp16 / 65536
         *   seconds to finish = sample_length * 65536 / (base_rate_fp16 * 48000)
         *   SW ticks (1 kHz)  = sample_length * 65536 / (base_rate_fp16 * 48)
         *
         * Adds 20 % headroom so modest pitch bends (drums: typically none)
         * don't truncate audible content. */
        if (v->base_rate_fp16 > 0 && zone->sample_length > 0) {
            uint64_t num = (uint64_t)zone->sample_length * 65536u * 6u;
            uint64_t den = (uint64_t)v->base_rate_fp16 * 48u * 5u;
            uint64_t ticks = num / (den ? den : 1);
            if (ticks < 1) ticks = 1;
            if (ticks > 0x7FFFFFFFu) ticks = 0x7FFFFFFFu;
            v->sample_ticks_remaining = (int32_t)ticks;
        } else {
            v->sample_ticks_remaining = 0;
        }
    }

    /* Initialize envelopes and LFOs from pre-baked OFSF v3 fields. */
    env_init(&v->vol_env, zone->vol_delay_ticks, zone->vol_attack_rate);
    env_init(&v->mod_env, zone->mod_delay_ticks, zone->mod_attack_rate);

    lfo_init(&v->mod_lfo, zone->mod_lfo_delay_ticks, zone->mod_lfo_rate);
    lfo_init(&v->vib_lfo, zone->vib_lfo_delay_ticks, zone->vib_lfo_rate);

    /* Advance the envelope one tick so the level is non-zero before
     * the ISR runs — otherwise the ISR writes volume 0 immediately. */
    env_advance(&v->vol_env, zone, 1);

    /* Cache pan multipliers before the first compute_vol_lr (and before the
     * ISR ticks this voice), so the hot path is divide-free from tick one. */
    voice_recompute_pan(v);

    int vl, vr;
    compute_vol_lr(v, &vl, &vr);
    of_mixer_set_vol_lr_h(mhv, vl, vr);
    stat_vol_writes++;
    prev_vol_l[idx] = vl;
    prev_vol_r[idx] = vr;
    prev_rate[idx]  = v->base_rate_fp16;

    return idx;
}

void smp_voice_note_off(int midi_ch, int note)
{
    for (int i = 0; i < SMP_MAX_VOICES; i++) {
        smp_voice_t *v = &voices[i];
        if (!v->active || v->active == STEAL_PENDING)
            continue;
        if (v->midi_ch != midi_ch || v->note != note)
            continue;

        if (ch_sustain[midi_ch]) {
            v->sustain_held = 1;
        } else {
            env_start_release(&v->vol_env, v->zone->vol_release_ticks);
            env_start_release(&v->mod_env, v->zone->mod_release_ticks);
        }
    }
}

void smp_voice_tick(void)
{
#if SMP_VOICE_ENABLE_TICK_STATS
    uint32_t _probe_t0 = OF_SVC->timer_get_us();
    uint8_t  _probe_active = 0;
    uint8_t  _probe_sustain = 0;
    uint8_t  _probe_release = 0;
    uint8_t  _probe_decay = 0;
    uint8_t  _probe_held = 0;
    uint8_t  _probe_ch[16] = {0};
#endif

    tick_counter++;

    /* Reap the HW mixer's voice-end queue ONCE per tick.
     * of_mixer_handle_active() runs mixer_reap_ended_pending() first — a
     * non-destructive reap that updates active_shadow and pushes ended voices
     * to the ended-queue WITHOUT consuming it (SFX end-polling is unaffected) —
     * then validates the handle.  We pass an INVALID handle purely to trigger
     * that reap; the per-voice voice_hw_owned_by_music() checks below then use
     * the non-reaping of_mixer_handle_group(), so we no longer pay one MMIO
     * MIX_IRQ_PENDING read per active voice per tick. */
    (void)of_mixer_handle_active(OF_MIXER_HANDLE_INVALID);

    voice_cleanup_stolen();

    for (int i = 0; i < SMP_MAX_VOICES; i++) {
        smp_voice_t *v = &voices[i];
        if (!v->active || v->active == STEAL_PENDING)
            continue;
        if (voice_drop_if_stale(v))
            continue;

#if SMP_VOICE_ENABLE_TICK_STATS
        _probe_active++;
        if (v->vol_env.stage == ENV_SUSTAIN) _probe_sustain++;
        else if (v->vol_env.stage == ENV_RELEASE) _probe_release++;
        else if (v->vol_env.stage == ENV_DECAY) _probe_decay++;
        if (v->sustain_held) _probe_held++;
        if (v->midi_ch < 16) _probe_ch[v->midi_ch]++;
#endif

        /* Natural sample-end check for one-shots.  When the sample has
         * played to its end the mixer is already emitting silence, so we
         * can force-DONE without any audible click and reclaim the slot
         * immediately — otherwise the envelope's long SUSTAIN parks the
         * voice (especially SF2 drum zones with very long vol_sustain)
         * and fills all SMP_MAX_VOICES soft voices during dense drum tracks. */
        if (v->sample_ticks_remaining > 0) {
            if (--v->sample_ticks_remaining == 0) {
                v->vol_env.stage = ENV_DONE;
                v->vol_env.level = 0;
            }
        }

        /* Caller fires this tick at 1 kHz (every 1 ms — matches
         * of_smp_tables.c's envelope baking).  Advance envelopes
         * and LFOs ONCE per call.  The previous "advance twice
         * per call" pattern existed because the pump used to fire
         * at 500 Hz and the inner double-step compensated to reach
         * effective 1 kHz; with 1 kHz pump the second pass would
         * make envelopes 2× too fast (snappy attacks, premature
         * releases). */
        const ofsf_zone_t *z = v->zone;
        env_advance(&v->vol_env, z, 1);
        env_advance(&v->mod_env, z, 0);
        /* Only advance an LFO whose phase a routing target actually reads —
         * compute_pitch is the sole consumer and gates on these same zone
         * fields.  Most GM melodic zones and all drum zones route no LFO to
         * pitch, so this skips both phase updates for them every tick.
         * INVARIANT: if an amplitude- or filter-LFO consumer is ever added,
         * widen this gate to cover its routing field too. */
        if (z) {
            if (z->vib_lfo_to_pitch) lfo_advance(&v->vib_lfo);
            if (z->mod_lfo_to_pitch) lfo_advance(&v->mod_lfo);
        }

        if (v->vol_env.stage == ENV_DONE) {
            voice_stop_hw_if_owned(v);
            v->active = 0;
            v->mixer_voice = OF_MIXER_HANDLE_INVALID;
            continue;
        }

        int vl, vr;
        compute_vol_lr(v, &vl, &vr);
        uint32_t rate = compute_pitch(v);
        int rate_changed = (rate != prev_rate[i]);
        int vol_changed  = (vl != prev_vol_l[i] || vr != prev_vol_r[i]);
        if (rate_changed || vol_changed) {
            /* This voice's handle was just validated this tick by
             * voice_drop_if_stale() above, and smp_voice_tick() runs inside
             * the machine-timer ISR (interrupts already masked on trap
             * entry).  So bypass the _h API's redundant handle re-validation
             * + IRQ save/restore: resolve the HW voice index once and issue
             * only the field(s) that actually changed.  For a sustaining note
             * the rate is stable while the volume envelope ramps, so RATE is
             * usually unchanged and skipped entirely. */
            int hw_voice = of_mixer_handle_voice(v->mixer_voice);
            if (hw_voice >= 0) {
                if (rate_changed) {
                    of_mixer_set_rate_raw(hw_voice, rate);
                    stat_rate_writes++;
                }
                if (vol_changed) {
                    of_mixer_set_vol_lr(hw_voice, vl, vr);
                    stat_vol_writes++;
                }
            }
            prev_vol_l[i] = vl;
            prev_vol_r[i] = vr;
            prev_rate[i] = rate;
        }
    }

#if SMP_VOICE_ENABLE_TICK_STATS
    uint32_t _probe_dt = OF_SVC->timer_get_us() - _probe_t0;
    tick_us_last = _probe_dt;
    if (_probe_dt > tick_us_max) tick_us_max = _probe_dt;
    if (_probe_dt > SMP_TICK_SPIKE_US) tick_spike_count++;
    if (_probe_active > tick_active_peak) tick_active_peak = _probe_active;
    tick_stage_sustain = _probe_sustain;
    tick_stage_release = _probe_release;
    tick_stage_decay   = _probe_decay;
    tick_sustain_held  = _probe_held;
    for (int i = 0; i < 16; i++)
        tick_ch_active[i] = _probe_ch[i];
#endif
    tick_stat_count++;
}

void smp_voice_update_volume(int midi_ch, int volume, int expression)
{
    if (midi_ch < 0 || midi_ch > 15) return;
    volume = clamp_midi7(volume);
    expression = clamp_midi7(expression);
    ch_volume[midi_ch]     = volume;
    ch_expression[midi_ch] = expression;
    channel_recompute_cached(midi_ch);
}

void smp_voice_update_pan(int midi_ch, int pan)
{
    if (midi_ch < 0 || midi_ch > 15) return;
    pan = clamp_midi7(pan);
    ch_pan[midi_ch] = pan;
    channel_recompute_cached(midi_ch);   /* refreshes ch_pan_midi[midi_ch] */
    /* Pan multipliers are per-voice (zone pan + channel pan); refresh every
     * live voice on this channel so the hot path stays divide-free. */
    for (int i = 0; i < SMP_MAX_VOICES; i++) {
        smp_voice_t *v = &voices[i];
        if (v->active && v->active != STEAL_PENDING && v->midi_ch == midi_ch)
            voice_recompute_pan(v);
    }
}

void smp_voice_update_bend(int midi_ch, int bend)
{
    if (midi_ch < 0 || midi_ch > 15) return;
    if (bend < -8192) bend = -8192;
    if (bend > 8191) bend = 8191;
    ch_bend[midi_ch] = bend;
    ch_bend_cents[midi_ch] = ((int32_t)bend * BEND_RANGE_CENTS) / 8192;
}

void smp_voice_update_mod(int midi_ch, int mod_depth)
{
    if (midi_ch < 0 || midi_ch > 15) return;
    mod_depth = clamp_midi7(mod_depth);
    ch_mod_depth[midi_ch] = mod_depth;
}

void smp_voice_update_sustain(int midi_ch, int sustain_on)
{
    if (midi_ch < 0 || midi_ch > 15) return;
    ch_sustain[midi_ch] = sustain_on;

    if (!sustain_on) {
        for (int i = 0; i < SMP_MAX_VOICES; i++) {
            smp_voice_t *v = &voices[i];
            if (v->active && v->active != STEAL_PENDING &&
                v->midi_ch == midi_ch && v->sustain_held) {
                v->sustain_held = 0;
                env_start_release(&v->vol_env, v->zone->vol_release_ticks);
                env_start_release(&v->mod_env, v->zone->mod_release_ticks);
            }
        }
    }
}

void smp_voice_update_filter(int midi_ch, int brightness, int resonance)
{
    if (midi_ch < 0 || midi_ch > 15) return;
    brightness = clamp_midi7(brightness);
    resonance = clamp_midi7(resonance);
    ch_brightness[midi_ch] = brightness;
    ch_resonance[midi_ch]  = resonance;
}

void smp_voice_update_reverb_send(int midi_ch, int send_0_127)
{
    if (midi_ch < 0 || midi_ch > 15) return;
    send_0_127 = clamp_midi7(send_0_127);
    ch_reverb_send[midi_ch] = send_0_127;
}

void smp_voice_update_chorus_send(int midi_ch, int send_0_127)
{
    if (midi_ch < 0 || midi_ch > 15) return;
    send_0_127 = clamp_midi7(send_0_127);
    ch_chorus_send[midi_ch] = send_0_127;
}

void smp_voice_all_off(int midi_ch)
{
    for (int i = 0; i < SMP_MAX_VOICES; i++) {
        smp_voice_t *v = &voices[i];
        if (v->active && v->active != STEAL_PENDING && v->midi_ch == midi_ch) {
            voice_stop_hw_if_owned(v);
            v->active = 0;
            v->mixer_voice = OF_MIXER_HANDLE_INVALID;
        }
    }
}

void smp_voice_all_off_global(void)
{
    for (int i = 0; i < SMP_MAX_VOICES; i++) {
        smp_voice_t *v = &voices[i];
        if (v->active) {
            voice_stop_hw_if_owned(v);
            v->active = 0;
            v->mixer_voice = OF_MIXER_HANDLE_INVALID;
        }
    }
}

void smp_voice_set_master_volume(int vol)
{
    if (vol < 0)   vol = 0;
    if (vol > 255) vol = 255;
    master_vol = vol;
}

#else /* OF_PC — desktop has no HW mixer voice path; silent stubs */

#include "include/of_smp_voice.h"
#include <string.h>
#include <stdint.h>

void smp_voice_init(void) {}
int  smp_voice_note_on(const ofsf_zone_t *zone, int midi_ch, int note,
                       int velocity, const void *sample_base) {
    (void)zone; (void)midi_ch; (void)note; (void)velocity; (void)sample_base;
    return -1;
}
void smp_voice_note_off(int midi_ch, int note) { (void)midi_ch; (void)note; }
void smp_voice_tick(void) {}

void smp_voice_tick_get_stats(smp_tick_stats_t *out) {
    if (out) memset(out, 0, sizeof(*out));
}
void smp_voice_tick_reset_stats(void) {}
void smp_voice_tick_record_pump(uint32_t elapsed_us, int ticks_fired,
                                int budget_exceeded) {
    (void)elapsed_us; (void)ticks_fired; (void)budget_exceeded;
}

void smp_voice_update_volume(int ch, int vol, int exp) { (void)ch; (void)vol; (void)exp; }
void smp_voice_update_pan(int ch, int pan)             { (void)ch; (void)pan; }
void smp_voice_update_bend(int ch, int bend)           { (void)ch; (void)bend; }
void smp_voice_update_mod(int ch, int depth)           { (void)ch; (void)depth; }
void smp_voice_update_sustain(int ch, int on)          { (void)ch; (void)on; }
void smp_voice_update_filter(int ch, int b, int r)     { (void)ch; (void)b; (void)r; }
void smp_voice_update_reverb_send(int ch, int v)       { (void)ch; (void)v; }
void smp_voice_update_chorus_send(int ch, int v)       { (void)ch; (void)v; }
void smp_voice_all_off(int ch)                         { (void)ch; }
void smp_voice_all_off_global(void)                    {}
void smp_voice_set_master_volume(int vol)              { (void)vol; }

#endif /* OF_PC */
