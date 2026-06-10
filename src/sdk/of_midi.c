//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_midi.c -- Standard MIDI File parser driving the sample voice engine.
 *
 * No synthesis state lives here: the parser unpacks events and forwards
 * them to of_smp_voice_* calls.  Per-channel controller state (volume,
 * expression, sustain, filter) is kept here because it's tracked at the
 * MIDI-event level and needs to survive across note-ons.
 *
 * MIDI is hardware-only; on the SDL2 desktop build (OF_PC) this file
 * collapses to no-op stubs at the bottom so apps that pull it in still
 * link without dragging in OF_SVC / fastram dependencies.
 */

#ifndef OF_PC

#include "include/of_midi.h"
#include "include/of_smp_bank.h"
#include "include/of_smp_voice.h"
#include "include/of_timer.h"
#include "include/of_services.h"
#include "include/of_fastram.h"

#include <stdint.h>
#include <stddef.h>

/* ========================================================================
 * Playback state
 * ======================================================================== */

#define MIDI_MAX_TRACKS 32

typedef struct {
    const uint8_t *data;
    uint32_t len;
    uint32_t pos;
    int64_t  pending_us;   /* signed: can go negative during tick processing */
    uint8_t  running;
    int      done;
} midi_track_t;

/* Pinned to BRAM (OF_FASTDATA): the timer ISR updates this struct every
 * tick (track cursors, tick_accum_us, channel state).  ISR SDRAM stores
 * race with GPU/bridge bus traffic — BRAM breaks the race. */
static OF_FASTDATA struct {
    int inited;
    int playing;
    int paused;
    int looping;

    const uint8_t *data;
    uint32_t len;

    uint16_t format;
    uint16_t num_tracks;
    uint16_t division;

    uint32_t us_per_beat;
    uint64_t us_per_tick_q16;  /* (us_per_beat << 16) / division, cached */
    uint32_t last_pump_us;
    uint32_t tick_accum_us;   /* accumulates until >= 1000 µs → one voice tick (matches of_smp_tables.c 1 kHz envelope rate) */

    midi_track_t tracks[MIDI_MAX_TRACKS];

    /* Per-MIDI-channel controller state */
    uint8_t program[16];
    uint8_t volume[16];      /* CC7 */
    uint8_t expression[16];  /* CC11 */
    uint8_t pan[16];         /* CC10 */
    uint8_t sustain[16];     /* CC64 */
    uint8_t mod_wheel[16];   /* CC1 */
    uint8_t brightness[16];  /* CC74 */
    uint8_t resonance[16];   /* CC71 */

    int master_volume;
} M;

/* ========================================================================
 * SMF byte helpers
 * ======================================================================== */

static uint16_t rd16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

static uint32_t read_var(midi_track_t *t) {
    uint32_t val = 0;
    const uint8_t *d = t->data;
    for (int i = 0; i < 4 && t->pos < t->len; i++) {
        uint8_t b = d[t->pos++];
        val = (val << 7) | (b & 0x7F);
        if (!(b & 0x80)) return val;
    }
    return val;
}

/* ========================================================================
 * Tempo cache
 * ======================================================================== */

/* Precompute microseconds-per-MIDI-tick in Q16.16.  Recomputed only when the
 * tempo (us_per_beat) or the file's division changes, so read_next_delta —
 * called once per MIDI event — replaces the old per-event 64-bit division
 * (delta * us_per_beat / division, a costly __divdi3 on RV32) with a 64-bit
 * multiply + shift.  Q16 keeps sub-microsecond precision so accumulated timing
 * drift is lower than the old integer-truncating divide. */
static void recompute_us_per_tick(void) {
    M.us_per_tick_q16 = M.division
        ? (((uint64_t)M.us_per_beat << 16) / M.division)
        : 0;
}

/* ========================================================================
 * Channel-state reset
 * ======================================================================== */

static void reset_channels(void) {
    for (int i = 0; i < 16; i++) {
        M.program[i]    = 0;
        M.volume[i]     = 100;
        M.expression[i] = 127;
        M.pan[i]        = 64;
        M.sustain[i]    = 0;
        M.mod_wheel[i]  = 0;
        M.brightness[i] = 64;
        M.resonance[i]  = 64;
    }
    M.us_per_beat = 500000;  /* 120 BPM */
    recompute_us_per_tick();
}

/* ========================================================================
 * Event dispatch — MIDI → voice engine
 * ======================================================================== */

static void note_on(int ch, int note, int vel) {
    const ofsf_zone_t *zones[4];
    int bank_idx = (ch == 9) ? 128 : 0;
    int n = of_smp_zone_lookup(bank_idx, M.program[ch], note, vel, zones, 4);
    const void *sbase = of_smp_bank_sample_base();
    for (int i = 0; i < n; i++)
        smp_voice_note_on(zones[i], ch, note, vel, sbase);
}

static void control_change(int ch, int cc, int val) {
    switch (cc) {
    case 1:  /* Mod wheel */
        M.mod_wheel[ch] = (uint8_t)val;
        smp_voice_update_mod(ch, val);
        break;
    case 7:  /* Channel volume */
        M.volume[ch] = (uint8_t)val;
        smp_voice_update_volume(ch, val, M.expression[ch]);
        break;
    case 10: /* Pan */
        M.pan[ch] = (uint8_t)val;
        smp_voice_update_pan(ch, val);
        break;
    case 11: /* Expression */
        M.expression[ch] = (uint8_t)val;
        smp_voice_update_volume(ch, M.volume[ch], val);
        break;
    case 64: /* Sustain pedal */
        M.sustain[ch] = (uint8_t)val;
        smp_voice_update_sustain(ch, val >= 64);
        break;
    case 71: /* Resonance */
        M.resonance[ch] = (uint8_t)val;
        smp_voice_update_filter(ch, M.brightness[ch], val);
        break;
    case 74: /* Brightness */
        M.brightness[ch] = (uint8_t)val;
        smp_voice_update_filter(ch, val, M.resonance[ch]);
        break;
    case 91: /* Reverb send (CC91) */
        smp_voice_update_reverb_send(ch, val);
        break;
    case 93: /* Chorus send (CC93) */
        smp_voice_update_chorus_send(ch, val);
        break;
    case 120: /* All Sound Off */
    case 123: /* All Notes Off */
        smp_voice_all_off(ch);
        break;
    case 121: /* Reset All Controllers */
        M.expression[ch] = 127;
        M.sustain[ch]    = 0;
        M.mod_wheel[ch]  = 0;
        M.brightness[ch] = 64;
        M.resonance[ch]  = 64;
        smp_voice_update_sustain(ch, 0);
        /* Re-sync the voice engine's cached per-channel state.  Without this,
         * a prior CC11 expression dip / CC1 mod-wheel stays latched in
         * ch_vol_combined / ch_mod_depth and keeps scaling the channel until
         * the next explicit CC7/CC11.  GM RAC leaves CC7 volume and CC10 pan
         * untouched; it does recentre pitch bend. */
        smp_voice_update_volume(ch, M.volume[ch], 127);
        smp_voice_update_mod(ch, 0);
        smp_voice_update_filter(ch, 64, 64);
        smp_voice_update_bend(ch, 0);
        break;
    }
}

/* ========================================================================
 * SMF track processor
 * ======================================================================== */

static inline int trk_need(midi_track_t *t, uint32_t n) {
    if (t->pos + n > t->len) { t->done = 1; return 0; }
    return 1;
}

static int process_event(midi_track_t *t) {
    if (t->done || t->pos >= t->len)
        return 0;

    uint8_t status = t->data[t->pos];
    if (status < 0x80) {
        status = t->running;
        if (status == 0) { t->done = 1; return 0; }
    } else {
        t->pos++;
        if (status < 0xF0)
            t->running = status;
    }

    uint8_t cmd = status & 0xF0;
    uint8_t ch  = status & 0x0F;

    /* System messages (0xF0–0xFF) are not channel-voice messages and must be
     * handled before the channel dispatch, since `cmd` would collapse them
     * all to 0xF0. */
    if (status == 0xFF) {
        /* Meta event */
        if (!trk_need(t, 1)) return 0;
        uint8_t meta = t->data[t->pos++];
        uint32_t mlen = read_var(t);
        if (mlen > t->len - t->pos) { t->done = 1; return 0; }
        if (meta == 0x51 && mlen == 3) {
            M.us_per_beat = ((uint32_t)t->data[t->pos] << 16) |
                            ((uint32_t)t->data[t->pos+1] << 8) |
                            t->data[t->pos+2];
            recompute_us_per_tick();
        } else if (meta == 0x2F) {
            t->done = 1;
        }
        t->pos += mlen;
        return !t->done;
    }
    if (status >= 0xF0 && status <= 0xF7) {
        /* SysEx */
        uint32_t slen = read_var(t);
        if (slen > t->len - t->pos) { t->done = 1; return 0; }
        t->pos += slen;
        return !t->done;
    }

    switch (cmd) {
    case 0x90: {  /* Note on (velocity 0 == note off) */
        if (!trk_need(t, 2)) return 0;
        uint8_t note = t->data[t->pos++];
        uint8_t vel  = t->data[t->pos++];
        if (vel > 0) note_on(ch, note, vel);
        else         smp_voice_note_off(ch, note);
        break;
    }
    case 0x80: {  /* Note off */
        if (!trk_need(t, 2)) return 0;
        uint8_t note = t->data[t->pos++];
        t->pos++;
        smp_voice_note_off(ch, note);
        break;
    }
    case 0xC0:    /* Program change */
        if (!trk_need(t, 1)) return 0;
        M.program[ch] = t->data[t->pos++];
        break;
    case 0xB0: {  /* Control change */
        if (!trk_need(t, 2)) return 0;
        uint8_t cc  = t->data[t->pos++];
        uint8_t val = t->data[t->pos++];
        control_change(ch, cc, val);
        break;
    }
    case 0xE0: {  /* Pitch bend */
        if (!trk_need(t, 2)) return 0;
        uint8_t lsb = t->data[t->pos++];
        uint8_t msb = t->data[t->pos++];
        int16_t bend = (int16_t)(((uint16_t)msb << 7) | lsb) - 8192;
        smp_voice_update_bend(ch, bend);
        break;
    }
    case 0xD0:    /* Channel pressure (1 data byte) */
        if (!trk_need(t, 1)) return 0;
        t->pos += 1;
        break;
    case 0xA0:    /* Polyphonic aftertouch (2 data bytes) */
    default:      /* Any other voice message: skip its 2 data bytes */
        if (!trk_need(t, 2)) return 0;
        t->pos += 2;
        break;
    }

    return !t->done;
}

static void read_next_delta(midi_track_t *t) {
    if (t->done || t->pos >= t->len) {
        t->done = 1;
        return;
    }
    uint32_t delta = read_var(t);
    /* delta * (us_per_beat / division), via the cached Q16.16 µs-per-tick — no
     * per-event 64-bit divide.  (division==0 is rejected in parse_header, so
     * us_per_tick_q16 is always valid while playing.) */
    t->pending_us += (int64_t)(((uint64_t)delta * M.us_per_tick_q16) >> 16);
}

/* ========================================================================
 * SMF header parsing
 * ======================================================================== */

static int parse_header(void) {
    if (M.len < 14) return OF_MIDI_ERR_BAD_HDR;
    if (M.data[0] != 'M' || M.data[1] != 'T' ||
        M.data[2] != 'h' || M.data[3] != 'd')
        return OF_MIDI_ERR_BAD_HDR;

    uint32_t hdr_len = rd32(M.data + 4);
    M.format     = rd16(M.data + 8);
    M.num_tracks = rd16(M.data + 10);
    M.division   = rd16(M.data + 12);

    if (M.format > 1)      return OF_MIDI_ERR_FORMAT;
    if (M.num_tracks == 0) return OF_MIDI_ERR_NO_TRACKS;
    /* division is the per-tick divisor in read_next_delta; 0 would divide by
     * zero (and SMPTE-format division with bit 15 set is not supported). */
    if (M.division == 0)   return OF_MIDI_ERR_BAD_HDR;
    if (M.num_tracks > MIDI_MAX_TRACKS) M.num_tracks = MIDI_MAX_TRACKS;

    uint32_t offset = 8 + hdr_len;
    int found = 0;
    for (int i = 0; i < M.num_tracks && offset + 8 <= M.len; i++) {
        if (M.data[offset] == 'M' && M.data[offset+1] == 'T' &&
            M.data[offset+2] == 'r' && M.data[offset+3] == 'k') {
            uint32_t tlen = rd32(M.data + offset + 4);
            M.tracks[found].data       = M.data + offset + 8;
            M.tracks[found].len        = tlen;
            M.tracks[found].pos        = 0;
            M.tracks[found].pending_us = 0;
            M.tracks[found].running    = 0;
            M.tracks[found].done       = 0;
            found++;
            offset += 8 + tlen;
        } else {
            uint32_t clen = rd32(M.data + offset + 4);
            offset += 8 + clen;
        }
    }

    M.num_tracks = (uint16_t)found;
    if (found == 0) return OF_MIDI_ERR_NO_TRACKS;

    return OF_MIDI_OK;
}

static void reset_tracks(void) {
    for (int i = 0; i < M.num_tracks; i++) {
        M.tracks[i].pos        = 0;
        M.tracks[i].pending_us = 0;
        M.tracks[i].running    = 0;
        M.tracks[i].done       = 0;
        read_next_delta(&M.tracks[i]);
    }
}

/* ========================================================================
 * Public API
 * ======================================================================== */

int of_midi_init(void) {
    (void)of_smp_bank_bind_preloaded();
    smp_voice_init();
    reset_channels();
    M.inited        = 1;
    M.playing       = 0;
    M.paused        = 0;
    M.tick_accum_us = 0;
    /* Default below full-scale to give the mixer headroom for dense
     * polyphony.  The HW mixer sums voices into s32 accumulators and
     * saturates to s16 — at master=255 a 20-28 voice MIDI passage
     * hard-clips at peaks and sounds like voice "breakup".  128 halves
     * the per-voice peak so roughly 4× more concurrent voices fit
     * without clipping.  Apps can raise it with of_midi_set_volume()
     * if they know polyphony is low. */
    M.master_volume = 128;
    smp_voice_set_master_volume(M.master_volume);
    return OF_MIDI_OK;
}

int of_midi_play(const uint8_t *data, uint32_t len, int loop) {
    if (!M.inited)                      return OF_MIDI_ERR_NOT_INIT;
    if (M.playing)                      return OF_MIDI_ERR_PLAYING;
    if (of_smp_bank_get() == NULL)      return OF_MIDI_ERR_NO_BANK;

    M.data    = data;
    M.len     = len;
    M.looping = loop;

    int rc = parse_header();
    if (rc != OF_MIDI_OK) return rc;

    reset_channels();
    smp_voice_all_off_global();
    reset_tracks();

    M.playing      = 1;
    M.paused       = 0;
    M.last_pump_us = of_time_us();

    /* Run the MIDI pump at 50 Hz. Combined with the 2 ms PUMP_BUDGET_US
     * cap below, timer callback CPU load is bounded to 100 ms/sec. */
    of_timer_set_callback(of_midi_pump, 50);
    return OF_MIDI_OK;
}

void of_midi_stop(void) {
    /* Detach the ISR before mutating state — otherwise the ISR could
     * preempt mid-teardown and race on M.playing / voice state. */
    of_timer_set_callback(NULL, 0);
    smp_voice_all_off_global();
    M.playing = 0;
    M.paused  = 0;
}

void of_midi_pause(void) {
    if (M.playing) M.paused = 1;
}

void of_midi_resume(void) {
    if (M.paused) {
        M.paused       = 0;
        M.last_pump_us = of_time_us();
    }
}

void of_midi_pump(void) {
    if (!M.playing || M.paused) return;

    /* Called from the machine-timer ISR.  DO NOT use of_time_us() here
     * — it issues an ECALL which triggers a nested trap, clobbering
     * mscratch + the existing trap frame and hanging the CPU.  Read
     * the monotonic timer via the direct service-table pointer
     * instead; it reads the cycle CSR in-line from M-mode. */
    uint32_t now = OF_SVC->timer_get_us();
    int64_t elapsed = (int64_t)(now - M.last_pump_us);
    M.last_pump_us = now;

    if (elapsed > 500000) elapsed = 500000;

    if (elapsed > 0) {
        M.tick_accum_us += (uint32_t)elapsed;

        /* OVERRUN GUARD.  Cap the ENTIRE pump call (envelope ticks +
         * MIDI event dispatch below) to PUMP_BUDGET_US of wall-clock
         * work.  Both loops re-check the budget every iteration; on
         * overrun we drop unprocessed envelope accumulation AND stop
         * dispatching MIDI events for this call.  Avoids ISR-monopoly
         * starvation of the main thread. */
        const uint32_t PUMP_BUDGET_US = 2000;   /* 2 ms hard cap */
        uint32_t pump_start_us = OF_SVC->timer_get_us();

        /* of_smp_tables.c bakes envelope rates assuming 1 kHz (1 ms/tick),
         * so smp_voice_tick MUST fire every 1000 µs.  Half-rate (every
         * 2 ms) made every attack/decay/release run at 2× duration —
         * audible as muddy notes that overlap into the next tone.
         * Doubled the tick budget so a moderate polyphony burst still
         * stays inside the pump-side wall-clock cap. */
        int tick_budget = 500;
        int ticks_fired = 0;
        int overrun = 0;
        while (M.tick_accum_us >= 1000 && tick_budget > 0) {
            smp_voice_tick();
            M.tick_accum_us -= 1000;
            tick_budget--;
            ticks_fired++;
            if ((OF_SVC->timer_get_us() - pump_start_us) > PUMP_BUDGET_US) {
                overrun = 1;
                break;
            }
        }
        int budget_exceeded = (tick_budget == 0) || overrun;
        if (budget_exceeded)
            M.tick_accum_us = 0;
        smp_voice_tick_record_pump((uint32_t)elapsed, ticks_fired,
                                   budget_exceeded);

        /* Single pass: decrement each live track's pending clock and fire its
         * due events.  (The decrement and dispatch were two loops; merging is
         * safe because the per-track work is independent and the overrun goto
         * only fires inside a non-done track, which has already set
         * any_active.) */
        int any_active = 0;
        int safety = 10000;
        for (int i = 0; i < M.num_tracks; i++) {
            midi_track_t *t = &M.tracks[i];
            if (t->done) continue;
            any_active = 1;
            t->pending_us -= elapsed;
            while (!t->done && t->pending_us <= 0 && safety > 0) {
                process_event(t);
                if (!t->done) read_next_delta(t);
                safety--;
                /* Same overrun cap as the envelope loop.  Pending MIDI
                 * events stay queued (their pending_us is still <=0)
                 * so the next pump call picks them up — no notes lost,
                 * just delayed. */
                if ((OF_SVC->timer_get_us() - pump_start_us) > PUMP_BUDGET_US)
                    goto pump_done;
            }
        }
        pump_done: ;

        if (!any_active) {
            if (M.looping) {
                smp_voice_all_off_global();
                reset_channels();
                reset_tracks();
                /* ISR context — no ECALL, see note in of_midi_pump. */
                M.last_pump_us = OF_SVC->timer_get_us();
            } else {
                smp_voice_all_off_global();
                M.playing = 0;
            }
        }
    }
}

int of_midi_playing(void)    { return M.playing; }
int of_midi_paused(void)     { return M.paused; }
int of_midi_get_volume(void) { return M.master_volume; }

void of_midi_set_volume(int volume) {
    if (volume < 0)   volume = 0;
    if (volume > 255) volume = 255;
    M.master_volume = volume;
    smp_voice_set_master_volume(volume);
}

int of_midi_get_program(int ch) {
    if (ch < 0 || ch > 15) return 0;
    return M.program[ch];
}

#else /* OF_PC — desktop has no hardware MIDI path; provide silent stubs */

#include "include/of_midi.h"
#include <stdint.h>

int  of_midi_init(void)                                 { return 0; }
int  of_midi_play(const uint8_t *d, uint32_t l, int lp) { (void)d; (void)l; (void)lp; return 0; }
void of_midi_stop(void)                                 {}
void of_midi_pause(void)                                {}
void of_midi_resume(void)                               {}
void of_midi_pump(void)                                 {}
int  of_midi_playing(void)                              { return 0; }
int  of_midi_paused(void)                               { return 0; }
int  of_midi_get_volume(void)                           { return 100; }
void of_midi_set_volume(int v)                          { (void)v; }
int  of_midi_get_program(int ch)                        { (void)ch; return 0; }

#endif /* OF_PC */
