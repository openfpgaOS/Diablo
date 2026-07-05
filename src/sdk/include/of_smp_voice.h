//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_smp_voice.h -- Software voice engine for sample-based MIDI synthesis.
 *
 * Manages simultaneous sample voices with DAHDSR envelopes and dual
 * LFOs, driving the hardware mixer.  All math is fixed-point Q16.16;
 * runs in the 1 kHz timer ISR on RV32IMFC.
 */

#ifndef OF_SMP_VOICE_H
#define OF_SMP_VOICE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "of_smp_bank.h"

/* Conservative SDK default.  Apps with their own BRAM budget can raise this
 * at compile time, as long as they stay below OF_MIXER_MAX_VOICES (32) and
 * keep smp_voice_tick comfortably inside the 1 kHz timer budget. */
#ifndef SMP_MAX_VOICES
#define SMP_MAX_VOICES 12
#endif

typedef enum {
    ENV_OFF = 0, ENV_DELAY, ENV_ATTACK, ENV_HOLD,
    ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE, ENV_DONE
} env_stage_t;

typedef struct {
    env_stage_t stage;
    int32_t level;      /* Q16.16 (0=silence, 0x10000=full) */
    int32_t rate;       /* level delta per tick (sign depends on stage) */
    int32_t target;     /* level at end of current stage */
    int32_t timer;      /* ticks remaining in delay/hold (counts down) */
} env_state_t;

typedef struct {
    int32_t phase;      /* Q16.16, wraps at 0x10000 (one full cycle) */
    int32_t rate;       /* phase increment per tick */
    int32_t delay_ticks;/* delay countdown (>0 = LFO silent) */
} lfo_state_t;

typedef struct {
    int active;
    const ofsf_zone_t *zone;
    uint8_t midi_ch;
    uint8_t note;
    uint8_t velocity;
    uint8_t voice_base_vol;  /* Pre-baked at note-on: (velocity_gain × initial_attn_scale) >> 8.
                                Collapses two multiplies into one slot, drops one mul/tick. */
    uint8_t sustain_held; /* CC64 holding this note in sustain */
    uint64_t mixer_voice; /* stable hardware mixer handle */
    env_state_t vol_env;
    env_state_t mod_env;
    lfo_state_t mod_lfo;
    lfo_state_t vib_lfo;
    uint32_t base_rate_fp16; /* base 16.16 playback rate (no bend/LFO) */
    /* Cached L/R pan multipliers (Q0.PAN_SHIFT).  Recomputed at note-on and
     * when CC10 changes for this channel, so the per-tick volume path is two
     * multiplies + shift with no divide.  One side is always full-scale
     * (1<<PAN_SHIFT); the other carries the equal-volume pan attenuation. */
    int32_t pan_mul_l;
    int32_t pan_mul_r;
    uint32_t age;
    /* tick_counter at which this voice last entered (or has been held in)
     * ENV_SUSTAIN.  Reset every tick the voice is in any other stage, so
     * tick_counter - sustain_since measures time spent CONTINUOUSLY in
     * sustain -- used by the hung-voice guard (SMP_VOICE_MAX_SUSTAIN_TICKS). */
    uint32_t sustain_since;
    /* Countdown of smp_voice_tick calls until the underlying non-looping
     * sample has played to its natural end.  0 = not tracked (looping
     * sample, or untracked).  When this reaches 0 from a positive value,
     * we force ENV_DONE so the voice slot is reclaimed promptly.
     *
     * Why:  of_mixer_play on a one-shot sample eventually walks off the
     * end of LEN and stops emitting audio — but smp_voice has no way to
     * know that happened, so the slot remains allocated for the full
     * length of the SF2 volume envelope (which for drums can be
     * multi-second SUSTAIN, parking the voice forever).  During dense
     * drum tracks this fills all 28 software voices and every new note
     * must steal, producing "old data" artifacts.  Tracking the
     * expected end lets us release slots as soon as the audio has
     * actually finished. */
    int32_t sample_ticks_remaining;
} smp_voice_t;

void smp_voice_init(void);
int  smp_voice_note_on(const ofsf_zone_t *zone, int midi_ch, int note,
                       int velocity, const void *sample_base);
void smp_voice_note_off(int midi_ch, int note);
void smp_voice_tick(void);  /* 1 kHz ISR */

/* Diagnostic stats for smp_voice_tick cost.  Task #10 probe: detect
 * whether a 1 kHz voice tick exceeds the 2 ms pump cap.
 * Fields are named cycles_* but actually hold microseconds — the
 * VexiiRiscv here does not expose rdcycle to user mode, so of_time_us()
 * (kernel ecall) is used instead. */
typedef struct {
    uint32_t cycles_max;     /* worst-case microseconds for a single tick */
    uint32_t cycles_last;    /* microseconds of most recent tick */
    uint32_t spike_count;    /* ticks where microseconds > 2000 */
    uint32_t tick_count;     /* total ticks since reset */
    uint8_t  active_peak;    /* max active voices seen since reset */
    /* Stage histogram — snapshot at the most recent tick. */
    uint8_t  stage_sustain;  /* voices in ENV_SUSTAIN (waiting for note-off) */
    uint8_t  stage_release;  /* voices in ENV_RELEASE (fading out) */
    uint8_t  stage_decay;    /* voices in ENV_DECAY (between attack and sustain) */
    uint8_t  sustain_held;   /* voices with CC64 sustain pedal still holding them */
    /* Per-MIDI-channel active voice count — snapshot at most recent tick.
     * Lets you see which channels are actually producing sound (and
     * release tails), vs. channels that should be silent.  Index 9 is
     * the drum channel. */
    uint8_t  ch_active[16];

    /* MMIO-write counters — incremented on every actual HW write since
     * last reset (after the cache-skip guards, so these are the writes
     * that genuinely hit the mixer bus). */
    uint32_t filter_writes;  /* retired filter path; currently always 0 */
    uint32_t rate_writes;    /* pitch / voice rate register writes */
    uint32_t vol_writes;     /* volume register writes */

    /* Pump() interval stats — populated by smp_voice_tick_record_pump(),
     * lets us see whether of_midi_pump is called smoothly or in bursts. */
    uint32_t pump_count;             /* total of_midi_pump calls since reset */
    uint32_t pump_interval_max_us;   /* worst-case gap between pumps */
    uint32_t pump_interval_min_us;   /* best-case gap (UINT32_MAX if no data) */
    uint32_t pump_burst_count;       /* pumps where >1 ticks fired */
    uint32_t pump_budget_exceeded;   /* pumps where tick_budget==0 at end */
} smp_tick_stats_t;

void smp_voice_tick_get_stats(smp_tick_stats_t *out);
void smp_voice_tick_reset_stats(void);

/* Feed pump() timing into the probe.  `elapsed_us` is the wallclock gap
 * since the previous pump; `ticks_fired` is how many smp_voice_tick calls
 * the pump dispatched; `budget_exceeded` is 1 iff the pump hit its
 * tick_budget cap (i.e. had to drop accumulated ticks). */
void smp_voice_tick_record_pump(uint32_t elapsed_us, int ticks_fired,
                                int budget_exceeded);
void smp_voice_update_volume(int midi_ch, int volume, int expression);
void smp_voice_update_pan(int midi_ch, int pan);
void smp_voice_update_bend(int midi_ch, int bend);
void smp_voice_update_mod(int midi_ch, int mod_depth);
void smp_voice_update_sustain(int midi_ch, int sustain_on);
void smp_voice_update_filter(int midi_ch, int brightness, int resonance);
/* CC91 (reverb send) and CC93 (chorus send), 0..127 — stored per channel
 * but not yet acted on (the CPU mixer has no per-voice send paths). */
void smp_voice_update_reverb_send(int midi_ch, int send_0_127);
void smp_voice_update_chorus_send(int midi_ch, int send_0_127);
void smp_voice_all_off(int midi_ch);
void smp_voice_all_off_global(void);
void smp_voice_set_master_volume(int vol);

#ifdef __cplusplus
}
#endif

#endif /* OF_SMP_VOICE_H */
