//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_smp_bank.h -- On-device runtime bank format (.ofsf) for SF2
 *                  sample-based MIDI synthesis.
 *
 * A .ofsf file is a flat, pre-resolved binary loaded into SDRAM with
 * no on-device parsing.  All SF2 generators are baked into
 * per-zone fields by the offline converter (tools/sf2_to_ofsf.c).
 *
 * v3 (current): timecents, centibels, and LFO-cents are pre-converted
 * by the offline converter into per-tick rates and Q16.16 levels using
 * the helpers in of_smp_tables.h.  Runtime note-on / envelope-stage
 * transitions become straight field reads — no per-call helper invocation.
 * Pitch-mod and filter-mod amounts stay in cents because the runtime
 * accumulates them with channel CC values before composing.
 *
 * Layout:  [header 96B] [preset_index 1024B] [zones N*112B] [sample blob]
 */

#ifndef OF_SMP_BANK_H
#define OF_SMP_BANK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define OFSF_MAGIC      0x4F465346  /* 'OFSF' */
#define OFSF_VERSION    3

#define OFSF_PRESET_COUNT   256     /* 128 melodic (bank 0) + 128 drum (bank 128) */

/* Metadata strings (null-terminated, truncated if longer) */
#define OFSF_NAME_MAX       32
#define OFSF_AUTHOR_MAX     32

/* Loop modes */
#define OFSF_LOOP_NONE      0
#define OFSF_LOOP_FORWARD   1
#define OFSF_LOOP_BIDI      3

/* ---- File header (96 bytes) ----
 *
 * bank_name and bank_author are pulled from the SF2 INFO LIST (INAM/IENG)
 * by sf2_to_ofsf so the OS can display them at boot without opening the
 * SF2 source. */
typedef struct __attribute__((packed)) {
    uint32_t magic;             /* OFSF_MAGIC */
    uint32_t version;           /* OFSF_VERSION */
    uint32_t sample_rate;       /* native rate of all samples (e.g. 44100) */
    uint32_t zone_count;        /* total zones in file */
    uint32_t sample_data_offset;/* bytes from file start to sample blob */
    uint32_t sample_data_size;  /* sample blob size in bytes */
    uint32_t flags;             /* reserved, 0 */
    uint32_t reserved;
    char     bank_name[OFSF_NAME_MAX];     /* SF2 INAM, null-terminated */
    char     bank_author[OFSF_AUTHOR_MAX]; /* SF2 IENG, null-terminated */
} ofsf_header_t;

/* ---- Preset index (256 x 4 bytes = 1024 bytes) ---- */

typedef struct __attribute__((packed)) {
    uint16_t zone_start;        /* first zone index for this preset */
    uint16_t zone_count;        /* number of zones */
} ofsf_preset_t;

/* ---- Zone (112 bytes, OFSF v3) ----
 *
 * Static SF2 generators (envelope timing, sustain levels, LFO frequency,
 * initial attenuation) are pre-converted by the converter using the
 * helpers in of_smp_tables.h.  Mod-matrix amounts (the *_to_pitch /
 * *_to_filter / pan / filter cutoff fields) stay as raw SF2 units
 * because they are accumulated with channel CC values at runtime before
 * the final cents→rate conversion. */

typedef struct __attribute__((packed)) {
    /* Key/velocity range -- offset 0 */
    uint8_t  key_lo;
    uint8_t  key_hi;
    uint8_t  vel_lo;
    uint8_t  vel_hi;

    /* Sample reference (offsets relative to sample blob start) -- offset 4 */
    uint32_t sample_offset;     /* bytes from sample blob start */
    uint32_t sample_length;     /* in samples */
    uint32_t loop_start;        /* in samples */
    uint32_t loop_end;          /* in samples */

    /* -- offset 20 */
    uint8_t  loop_mode;         /* OFSF_LOOP_* */
    uint8_t  root_key;          /* MIDI note of original pitch */
    int8_t   fine_tune;         /* cents, -99..+99 */
    int8_t   coarse_tune;       /* semitones */

    /* Volume envelope (DAHDSR), pre-baked -- offset 24 */
    uint32_t vol_delay_ticks;       /* 1 kHz ticks; 0 = skip stage */
    uint32_t vol_attack_rate;       /* Q16.16 incr/tick (0x10000 / atk_ticks) */
    uint32_t vol_hold_ticks;        /* 0 = skip stage */
    uint32_t vol_decay_rate;        /* Q16.16 decr/tick from 0x10000 → sustain */
    uint32_t vol_sustain_level;     /* Q16.16, 0..0x10000 */
    uint32_t vol_release_ticks;     /* baked ticks; runtime computes
                                       rate = current_level / ticks */

    /* Modulation envelope (DAHDSR), pre-baked -- offset 48 */
    uint32_t mod_delay_ticks;
    uint32_t mod_attack_rate;
    uint32_t mod_hold_ticks;
    uint32_t mod_decay_rate;
    uint32_t mod_sustain_level;
    uint32_t mod_release_ticks;

    /* Modulation LFO -- offset 72 */
    uint32_t mod_lfo_delay_ticks;
    uint32_t mod_lfo_rate;          /* Q16.16 phase increment per 1 ms tick */
    int16_t  mod_lfo_to_pitch;      /* cents */
    int16_t  mod_lfo_to_filter;     /* cents */

    /* Vibrato LFO -- offset 84 */
    uint32_t vib_lfo_delay_ticks;
    uint32_t vib_lfo_rate;
    int16_t  vib_lfo_to_pitch;      /* cents */
    int16_t  _pad_vib;              /* keeps subsequent fields 4-byte aligned */

    /* Modulation envelope routing (cents, runtime accumulator) -- offset 96 */
    int16_t  mod_env_to_pitch;
    int16_t  mod_env_to_filter;

    /* Filter (cents / centibels, runtime accumulator) -- offset 100 */
    int16_t  initial_fc;            /* cents, 13500 ≈ wide-open */
    int16_t  initial_q;             /* centibels, 0-960 */

    /* Output -- offset 104 */
    uint16_t initial_attn_scale;    /* baked, 0..255 (255 = unity gain) */
    int16_t  pan;                   /* SF2 pan, -500..+500 */

    /* Misc -- offset 108 */
    uint8_t  exclusive_class;       /* 0 = none */
    uint8_t  _pad[3];
} ofsf_zone_t;

#ifndef __cplusplus
_Static_assert(sizeof(ofsf_zone_t)   == 112, "ofsf_zone_t must be 112 bytes (v3)");
_Static_assert(sizeof(ofsf_header_t) ==  96, "ofsf_header_t must be 96 bytes");
_Static_assert(sizeof(ofsf_preset_t) ==   4, "ofsf_preset_t must be 4 bytes");
#endif

/* ---- Runtime API ----
 *
 * No load/unload: the kernel auto-loads a .ofsf file staged in any data
 * slot at boot, and an SDK constructor binds this module to it before
 * main() runs. Apps just use the query functions below. */

const ofsf_header_t *of_smp_bank_get(void);
const void          *of_smp_bank_sample_base(void);

/* Bind this module to the kernel-preloaded .ofsf bank.
 *
 * Apps normally do not need to call this directly: an SDK constructor runs it
 * before main(), and of_midi_init() retries it.  The explicit entry exists so
 * apps/tests can force a retry and distinguish "no bank" from "metadata bind
 * failed" without starting MIDI playback.
 *
 * Returns 1 when a bank is bound, 0 when no preloaded bank is available, and
 * a negative value for malformed preload data or metadata allocation failure.
 */
int of_smp_bank_bind_preloaded(void);

int of_smp_zone_lookup(int bank, int program, int key, int velocity,
                       const ofsf_zone_t **zones_out, int max_zones);

#ifdef __cplusplus
}
#endif

#endif /* OF_SMP_BANK_H */
