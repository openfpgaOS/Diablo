//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_midi.h -- Standard MIDI File playback for openfpgaOS
 *
 * Renders Format 0 / Format 1 .mid files with the sample-based
 * synthesizer (of_smp_voice + of_smp_bank).  Non-blocking: call
 * of_midi_pump() from your game loop or idle hook.
 *
 * Requirements:
 *   - A .ofsf bank must be staged in a data slot. The kernel detects
 *     and loads it at boot — no app-side init is required. Ship the
 *     SC-55-derived bank in assets/banks/sc55.ofsf, or roll your own
 *     with tools/sf2_to_ofsf. Users can swap the file to pick a
 *     different SoundFont.
 *
 * Usage:
 *   of_midi_init();
 *   of_midi_play(midi_data, midi_len, 1);  // loop
 *   while (1) {
 *       // ... game logic ...  (pump runs from the timer ISR)
 *   }
 *
 * Note: of_midi_play() installs of_midi_pump as the machine-timer ISR
 * callback.  The callback WAKES at 50 Hz and, on each wake, advances the
 * synth envelopes at their baked 1 kHz rate by firing N = elapsed-ms catch-up
 * ticks (of_smp_tables.c bakes envelopes at 1 kHz).  So the mixer is driven
 * independently of the main thread.  Do NOT call of_midi_pump() from the main
 * loop while playback is active — the ISR owns it and concurrent calls race.
 */

#ifndef OF_MIDI_H
#define OF_MIDI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Error codes */
#define OF_MIDI_OK            0
#define OF_MIDI_ERR_NOT_INIT  (-1)
#define OF_MIDI_ERR_BAD_HDR   (-2)
#define OF_MIDI_ERR_FORMAT    (-3)
#define OF_MIDI_ERR_NO_TRACKS (-4)
#define OF_MIDI_ERR_PLAYING   (-5)
#define OF_MIDI_ERR_NO_BANK   (-6)

/* Initialize the MIDI front-end and the sample voice engine. */
int of_midi_init(void);

/* Start playback of a Standard MIDI File in memory.
 * data/len: raw .mid file bytes.
 * loop:     1 = restart on end-of-track, 0 = play once.
 * Returns OF_MIDI_OK or an error code. */
int of_midi_play(const uint8_t *data, uint32_t len, int loop);

/* Stop playback, silence all voices. */
void of_midi_stop(void);

/* Pause / resume (notes sustain during pause). */
void of_midi_pause(void);
void of_midi_resume(void);

/* Process pending MIDI events and advance envelopes.
 * Installed as the machine-timer ISR callback by of_midi_play(); the
 * callback wakes at 50 Hz and advances envelopes at their baked 1 kHz rate
 * via an elapsed-time catch-up loop.  Do NOT invoke from the main loop
 * while playback is active. */
void of_midi_pump(void);

/* Query state */
int of_midi_playing(void);
int of_midi_paused(void);

/* Master volume (0 = silent, 255 = full). Default: 255. */
void of_midi_set_volume(int volume);
int  of_midi_get_volume(void);

/* Diagnostic: current GM program number on a channel (0-127).  Channel 9
 * is the drum channel; its program selects the drum kit.  Returns 0 for
 * out-of-range channels. */
int  of_midi_get_program(int ch);

#ifdef __cplusplus
}
#endif

#endif /* OF_MIDI_H */
