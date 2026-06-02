//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_smp_tables.h -- shared SF2 unit-conversion helpers.
 *
 * Used at runtime by the SW voice engine for cents→rate and at offline
 * convert time by tools/sf2_to_ofsf.c to bake static SF2 fields into
 * pre-resolved OFSF v3 zone fields.  Both sides include this header so
 * the bake produces bit-identical results to whatever the runtime would
 * have computed at note-on.
 */

#ifndef OF_SMP_TABLES_H
#define OF_SMP_TABLES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* 2^(c/1200) * 65536 for c in [0, 1199].  Octave shifts handled by the
 * caller via bit shifts.  Defined in of_smp_tables.c. */
extern const uint32_t smp_cents_to_mult[1200];

/* Q16.16 multiplier for an arbitrary cents value (handles octave folding). */
uint32_t smp_cents_to_multiplier(int32_t cents);

/* SF2 timecents -> 1 kHz tick count.  Returns 0 for tc <= -12000
 * (instantaneous) and clamps long values to ~110 sec. */
int32_t smp_timecents_to_ticks(int16_t tc);

/* SF2 centibels of attenuation -> Q16.16 linear level (0..0x10000).
 * 0 cB = 0x10000 (unity), 960 cB ≈ 0.  Proper exponential matching
 * 10^(-cB/200), used for envelope sustain levels. */
int32_t smp_cb_to_level(int16_t cb);

/* SF2 LFO frequency in absolute cents -> Q16.16 phase increment per
 * 1 kHz tick.  Reference frequency 8.176 Hz at cents=0. */
uint32_t smp_lfo_freq_cents_to_rate(int16_t cents);

/* SF2 initialAttenuation centibels -> 8-bit linear scale (0..255).
 * Matches the per-tick formula in the SW voice engine's compute_vol_lr. */
uint8_t smp_cb_to_attn_scale(int16_t cb);

#ifdef __cplusplus
}
#endif

#endif /* OF_SMP_TABLES_H */
