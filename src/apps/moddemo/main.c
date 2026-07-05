//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * moddemo — minimal 4-channel ProTracker MOD player
 *
 * Canonical example of:
 *   - Mapping a tracker channel to a *pinned* hardware mixer voice via
 *     of_mixer_play (note-on) + of_mixer_retrigger (subsequent notes).
 *     Stop+play would let the allocator re-pick the slot and channels
 *     would steal each other — see project memory `mod_voice_pinning`.
 *   - Uploading the 8-bit MOD samples to the SDRAM mixer sample pool via
 *     of_mixer_alloc_samples (converting to 16-bit on the way in). The HW
 *     mixer's voice-fetch master reads this pool directly and the OS keeps
 *     it coherent — this is the pattern the SDK's own mixer tests use, and
 *     what the mixer reliably plays from on this platform.
 *   - Per-voice rate updates with of_mixer_set_rate_raw (Q16.16) on
 *     the tracker's tick boundary — no per-sample CPU mixing
 *
 * The CPU runs the tracker engine (period table, effect column,
 * volume slides, arpeggios) at ~50 Hz; everything per-sample is the
 * HW mixer's job.  This is what makes a MOD demo cheap on Pocket.
 *
 * Slot map (per instance.json):
 *   slot:4  primary MOD file
 *   slot:5  alternate MOD file (A button switches)
 *
 * Controls:
 *   A   switch between slot:4 and slot:5
 *   B   skip to next pattern in the song
 */

#include "of.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static void mod_idle_wait(void) {
#ifdef OF_PC
    usleep(1000);
#else
    __asm__ volatile("wfi");
#endif
}

/* ======================================================================
 * MOD format structures
 * ====================================================================== */

#define MOD_NUM_SAMPLES   31
#define MOD_NUM_CHANNELS  4
#define MOD_ROWS_PER_PAT  64
#define MOD_MAX_PATTERNS  128

typedef struct {
    char     name[23];
    uint16_t length;       /* in words (×2 for bytes) */
    int8_t   finetune;     /* -8 to +7 */
    uint8_t  volume;       /* 0-64 */
    uint16_t loop_start;   /* in words */
    uint16_t loop_length;  /* in words (>1 = looping) */
} mod_sample_t;

typedef struct {
    uint8_t  sample;
    uint16_t period;
    uint8_t  effect;
    uint8_t  param;
} mod_note_t;

typedef struct {
    char         title[21];
    mod_sample_t samples[MOD_NUM_SAMPLES];
    uint8_t      song_length;
    uint8_t      restart_pos;
    uint8_t      order[MOD_MAX_PATTERNS];
    int          num_patterns;
    uint8_t     *pattern_data;  /* raw pattern bytes */
    int8_t      *sample_data[MOD_NUM_SAMPLES]; /* pointers into SDRAM sample pool */
} mod_file_t;

/* ======================================================================
 * Amiga period table (octaves 1-3, C-1 to B-3)
 * ====================================================================== */

static const uint16_t period_table[36] = {
    856,808,762,720,678,640,604,570,538,508,480,453,  /* Octave 1 */
    428,404,381,360,339,320,302,285,269,254,240,226,  /* Octave 2 */
    214,202,190,180,170,160,151,143,135,127,120,113   /* Octave 3 */
};

/* Convert Amiga period to playback rate in Hz.
 * Amiga base clock = 7093789.2 Hz (PAL), period is clock divider. */
#define AMIGA_CLOCK 7093789

/* Pre-computed 16.16 fixed-point rates for each period */
static uint32_t period_to_rate_fp16(uint16_t period) {
    if (period == 0) return 0;
    uint32_t hz = AMIGA_CLOCK / (period * 2);
    return OF_MIXER_RATE_FP16(hz);
}

/* Find note index (0-35) from period, or -1 */
static int period_to_note(uint16_t period) {
    for (int i = 0; i < 36; i++)
        if (period_table[i] == period) return i;
    /* Closest match */
    for (int i = 0; i < 35; i++)
        if (period <= period_table[i] && period >= period_table[i + 1])
            return i;
    return -1;
}

/* ======================================================================
 * Per-channel state
 * ====================================================================== */

typedef struct {
    int      voice;          /* hardware voice index, or -1 */
    int      sample_idx;     /* current sample (1-31, 0=none) */
    uint16_t period;         /* current period */
    uint32_t rate_fp16;      /* current rate as 16.16 FP */
    int      volume;         /* current volume 0-64 */
    int      pan;            /* pan 0-255 (default: LRRL) */
    /* Effect state */
    uint16_t target_period;  /* portamento to note target */
    int      porta_speed;
    int      porta_up;       /* 1xx portamento up speed */
    int      porta_down;     /* 2xx portamento down speed */
    int      vol_slide;
    int      arp_note;       /* base note index */
    uint8_t  arp_x, arp_y;
    /* Vibrato / Tremolo */
    int      vib_speed;      /* 4xy: x = speed */
    int      vib_depth;      /* 4xy: y = depth */
    int      vib_pos;        /* vibrato phase (0-63) */
    int      trem_speed;     /* 7xy: x = speed */
    int      trem_depth;     /* 7xy: y = depth */
    int      trem_pos;       /* tremolo phase (0-63) */
    int      out_volume;     /* final volume after tremolo (0-64) */
    /* Extended effects */
    uint8_t  effect;         /* current effect number */
    uint8_t  effect_param;   /* current effect parameter */
    int      note_delay;     /* EDx: ticks to delay note */
    int      note_cut;       /* ECx: tick at which to cut */
    int      retrig;         /* E9x: retrigger interval */
    int      loop_row;       /* E6x: pattern loop start row */
    int      loop_count;     /* E6x: remaining loop iterations */
    /* Delayed note data */
    uint16_t delay_period;
    uint8_t  delay_sample;
} channel_t;

static channel_t channels[MOD_NUM_CHANNELS];

#define MAX_SONGS 2
static mod_file_t songs[MAX_SONGS];
static int num_songs = 0;
static int cur_song = 0;
static mod_file_t *mod;

/* Default panning: LRRL (Amiga stereo) */
static const int default_pan[4] = { 64, 192, 192, 64 };

/* ======================================================================
 * MOD file parsing
 * ====================================================================== */

static uint16_t read_be16(const uint8_t *p) {
    return (p[0] << 8) | p[1];
}

static int parse_mod(mod_file_t *m, const uint8_t *data, uint32_t len) {
    if (len < 1084) return -1;

    /* Check for M.K. signature at offset 1080 */
    if (memcmp(data + 1080, "M.K.", 4) != 0 &&
        memcmp(data + 1080, "4CHN", 4) != 0 &&
        memcmp(data + 1080, "FLT4", 4) != 0 &&
        memcmp(data + 1080, "M!K!", 4) != 0) {
        printf("Not a 4-channel MOD file\n");
        return -1;
    }

    /* Title */
    memcpy(m->title, data, 20);
    m->title[20] = '\0';
    printf("Title: %s\n", m->title);

    /* Sample descriptors */
    const uint8_t *p = data + 20;
    for (int i = 0; i < MOD_NUM_SAMPLES; i++) {
        memcpy(m->samples[i].name, p, 22);
        m->samples[i].name[22] = '\0';
        m->samples[i].length      = read_be16(p + 22) * 2;
        m->samples[i].finetune    = p[24] & 0x0F;
        if (m->samples[i].finetune > 7) m->samples[i].finetune -= 16;
        m->samples[i].volume      = p[25];
        m->samples[i].loop_start  = read_be16(p + 26) * 2;
        m->samples[i].loop_length = read_be16(p + 28) * 2;
        p += 30;
    }

    /* Song info */
    m->song_length  = data[950];
    m->restart_pos  = data[951];
    memcpy(m->order, data + 952, 128);

    /* Count patterns (highest pattern number in order table + 1) */
    m->num_patterns = 0;
    for (int i = 0; i < m->song_length; i++)
        if (m->order[i] >= m->num_patterns)
            m->num_patterns = m->order[i] + 1;

    printf("Patterns: %d, Length: %d\n", m->num_patterns, m->song_length);

    /* Pattern data starts at offset 1084 */
    m->pattern_data = (uint8_t *)(data + 1084);

    /* Sample data follows patterns */
    uint32_t sample_offset = 1084 + m->num_patterns * MOD_ROWS_PER_PAT * MOD_NUM_CHANNELS * 4;

    /* Upload samples to the SDRAM mixer pool, converting 8-bit signed to 16-bit signed.
     * The hardware mixer expects 16-bit samples via of_mixer_play. */
    for (int i = 0; i < MOD_NUM_SAMPLES; i++) {
        uint32_t slen = m->samples[i].length;
        if (slen == 0) {
            m->sample_data[i] = NULL;
            continue;
        }
        if (sample_offset + slen > len) {
            m->sample_data[i] = NULL;
            continue;
        }

        /* Allocate 2 bytes per sample (16-bit) in the mixer sample pool. */
        int16_t *cram = (int16_t *)of_mixer_alloc_samples(slen * 2);
        if (!cram) {
            printf("sample pool full at sample %d\n", i + 1);
            m->sample_data[i] = NULL;
            continue;
        }
        /* Convert 8-bit signed → 16-bit signed */
        const int8_t *src = (const int8_t *)(data + sample_offset);
        for (uint32_t j = 0; j < slen; j++)
            cram[j] = (int16_t)src[j] << 8;

        m->sample_data[i] = (int8_t *)cram;
        sample_offset += slen;
    }

    return 0;
}

/* ======================================================================
 * Pattern reading
 * ====================================================================== */

static mod_note_t read_note(int pattern, int row, int channel) {
    uint32_t offset = (pattern * MOD_ROWS_PER_PAT * MOD_NUM_CHANNELS + row * MOD_NUM_CHANNELS + channel) * 4;
    const uint8_t *p = mod->pattern_data + offset;
    mod_note_t n;
    n.sample = (p[0] & 0xF0) | (p[2] >> 4);
    n.period = ((p[0] & 0x0F) << 8) | p[1];
    n.effect = p[2] & 0x0F;
    n.param  = p[3];
    return n;
}

/* ======================================================================
 * Playback engine
 * ====================================================================== */

static int current_order = 0;
static int current_row = 0;
static int current_tick = 0;
static int ticks_per_row = 6;     /* default MOD speed */
static int bpm = 125;             /* default MOD tempo */
static int playing = 1;
static int pattern_break = 0;     /* set by effect 0xD/0xB to prevent double-break */
static int pattern_delay = 0;     /* EEx: extra row repeats remaining */

/* channels[ch].voice is hard-pinned to `ch` (Paula-style); we always
 * retrigger that slot, never alloc, so channels can't steal each other. */
static void trigger_note(channel_t *c, uint16_t period, int sample_offset) {
    c->period = period;
    c->rate_fp16 = period_to_rate_fp16(period);
    c->arp_note = period_to_note(period);

    int si = c->sample_idx - 1;
    if (si >= 0 && mod->sample_data[si]) {
        mod_sample_t *s = &mod->samples[si];
        int vol = c->volume * 255 / 64;
        int offset_bytes = sample_offset * 256;  /* 9xx: offset in 256-byte units, ×2 for 16-bit */
        if (offset_bytes >= (int)s->length) offset_bytes = 0;

        const uint8_t *pcm = (const uint8_t *)mod->sample_data[si] + offset_bytes * 2;
        uint32_t scnt = s->length - offset_bytes;
        uint32_t srate = AMIGA_CLOCK / (period * 2);

        of_mixer_retrigger(c->voice, pcm, scnt, srate, vol);

        if (s->loop_length > 2) {
            int ls = s->loop_start - offset_bytes;
            if (ls < 0) ls = 0;
            of_mixer_set_loop(c->voice, ls, ls + (int)s->loop_length);
        } else {
            /* Slot may have looped on the previous sample — clear the bit. */
            of_mixer_set_loop(c->voice, -1, 0);
        }
    }
}

/* Vibrato sine table (64 entries, 0-255) */
static const uint8_t vib_table[64] = {
      0, 24, 49, 74, 97,120,141,161,180,197,212,224,235,244,250,253,
    255,253,250,244,235,224,212,197,180,161,141,120, 97, 74, 49, 24,
      0, 24, 49, 74, 97,120,141,161,180,197,212,224,235,244,250,253,
    255,253,250,244,235,224,212,197,180,161,141,120, 97, 74, 49, 24,
};

static void play_note(int ch, mod_note_t *n) {
    channel_t *c = &channels[ch];

    /* Store effect for tick processing */
    c->effect = n->effect;
    c->effect_param = n->param;

    /* Reset per-row state */
    c->vol_slide = 0;
    c->porta_up = 0;
    c->porta_down = 0;
    c->arp_x = 0;
    c->arp_y = 0;
    c->note_delay = 0;
    c->note_cut = -1;
    c->retrig = 0;

    /* New sample? */
    if (n->sample > 0 && n->sample <= MOD_NUM_SAMPLES) {
        c->sample_idx = n->sample;
        c->volume = mod->samples[n->sample - 1].volume;
    }

    /* Handle EDx (note delay) — save note, don't trigger yet */
    if (n->effect == 0xE && (n->param >> 4) == 0xD) {
        c->note_delay = n->param & 0xF;
        c->delay_period = n->period;
        c->delay_sample = n->sample;
        goto parse_effects;
    }

    /* New note? */
    if (n->period > 0 && n->effect != 0x3 && n->effect != 0x5) {
        int sample_offset = (n->effect == 0x9) ? n->param : 0;
        trigger_note(c, n->period, sample_offset);
        c->vib_pos = 0;  /* reset vibrato phase on new note */
    } else if (n->period > 0 && (n->effect == 0x3 || n->effect == 0x5)) {
        c->target_period = n->period;
    }

parse_effects:
    /* Sticky parameters */
    if (n->effect == 0x3 && n->param) c->porta_speed = n->param;
    if (n->effect == 0x4) {
        if (n->param >> 4) c->vib_speed = n->param >> 4;
        if (n->param & 0xF) c->vib_depth = n->param & 0xF;
    }
    if (n->effect == 0x7) {
        if (n->param >> 4) c->trem_speed = n->param >> 4;
        if (n->param & 0xF) c->trem_depth = n->param & 0xF;
    }

    switch (n->effect) {
    case 0x0:  /* Arpeggio */
        if (n->param) {
            c->arp_x = n->param >> 4;
            c->arp_y = n->param & 0xF;
        }
        break;
    case 0x1:  /* Portamento up */
        c->porta_up = n->param;
        break;
    case 0x2:  /* Portamento down */
        c->porta_down = n->param;
        break;
    case 0x5:  /* Porta to note + volume slide */
        c->vol_slide = (n->param >> 4) - (n->param & 0xF);
        break;
    case 0x6:  /* Vibrato + volume slide */
        c->vol_slide = (n->param >> 4) - (n->param & 0xF);
        break;
    case 0x8:  /* Set panning */
        c->pan = n->param;
        break;
    case 0xA:  /* Volume slide */
        c->vol_slide = (n->param >> 4) - (n->param & 0xF);
        break;
    case 0xB:  /* Position jump */
        if (!pattern_break) {
            current_order = n->param;
            if (current_order >= mod->song_length)
                current_order = 0;
            current_row = -1;
            pattern_break = 1;
        }
        break;
    case 0xC:  /* Set volume */
        c->volume = n->param;
        if (c->volume > 64) c->volume = 64;
        break;
    case 0xD:  /* Pattern break */
        if (!pattern_break) {
            int break_row = (n->param >> 4) * 10 + (n->param & 0xF);
            if (break_row >= MOD_ROWS_PER_PAT) break_row = 0;
            current_row = break_row - 1;
            current_order++;
            if (current_order >= mod->song_length)
                current_order = mod->restart_pos;
            pattern_break = 1;
        }
        break;
    case 0xE:  /* Extended effects */
        switch (n->param >> 4) {
        case 0x1:  /* Fine portamento up */
            c->period -= n->param & 0xF;
            if (c->period < 113) c->period = 113;
            c->rate_fp16 = period_to_rate_fp16(c->period);
            break;
        case 0x2:  /* Fine portamento down */
            c->period += n->param & 0xF;
            if (c->period > 856) c->period = 856;
            c->rate_fp16 = period_to_rate_fp16(c->period);
            break;
        case 0x5:  /* Set finetune */
            if (c->sample_idx > 0)
                mod->samples[c->sample_idx - 1].finetune = (n->param & 0xF) > 7
                    ? (int8_t)((n->param & 0xF) - 16) : (int8_t)(n->param & 0xF);
            break;
        case 0x6:  /* Pattern loop */
            if ((n->param & 0xF) == 0) {
                c->loop_row = current_row;  /* set loop start */
            } else {
                if (c->loop_count == 0)
                    c->loop_count = n->param & 0xF;
                else
                    c->loop_count--;
                if (c->loop_count > 0)
                    current_row = c->loop_row - 1;  /* -1: tick advance will +1 */
            }
            break;
        case 0x9:  /* Retrigger note */
            c->retrig = n->param & 0xF;
            break;
        case 0xA:  /* Fine volume slide up */
            c->volume += n->param & 0xF;
            if (c->volume > 64) c->volume = 64;
            break;
        case 0xB:  /* Fine volume slide down */
            c->volume -= n->param & 0xF;
            if (c->volume < 0) c->volume = 0;
            break;
        case 0xC:  /* Note cut */
            c->note_cut = n->param & 0xF;
            break;
        case 0xE:  /* Pattern delay */
            if (pattern_delay == 0)
                pattern_delay = n->param & 0xF;
            break;
        }
        break;
    case 0xF:  /* Set speed/tempo */
        if (n->param < 32)
            ticks_per_row = n->param ? n->param : 1;
        else
            bpm = n->param;
        break;
    }

    c->out_volume = c->volume;
}

static void tick_effects(int ch) {
    channel_t *c = &channels[ch];
    c->out_volume = c->volume;

    /* Note delay — trigger on the right tick */
    if (c->note_delay > 0 && current_tick == c->note_delay) {
        if (c->delay_period > 0)
            trigger_note(c, c->delay_period, 0);
        c->note_delay = 0;
    }

    /* Note cut */
    if (c->note_cut >= 0 && current_tick == c->note_cut) {
        c->volume = 0;
        c->note_cut = -1;
    }

    /* Retrigger */
    if (c->retrig > 0 && current_tick > 0 && (current_tick % c->retrig) == 0) {
        if (c->period > 0)
            trigger_note(c, c->period, 0);
    }

    if (c->voice < 0) return;

    /* Arpeggio */
    if (c->arp_x || c->arp_y) {
        int offsets[3] = { 0, c->arp_x, c->arp_y };
        int note = c->arp_note + offsets[current_tick % 3];
        if (note >= 0 && note < 36)
            c->rate_fp16 = period_to_rate_fp16(period_table[note]);
    }

    /* Portamento up (1xx) */
    if (c->porta_up) {
        int p = (int)c->period - c->porta_up;
        if (p < 113) p = 113;
        c->period = (uint16_t)p;
        c->rate_fp16 = period_to_rate_fp16(c->period);
    }

    /* Portamento down (2xx) */
    if (c->porta_down) {
        int p = (int)c->period + c->porta_down;
        if (p > 856) p = 856;
        c->period = (uint16_t)p;
        c->rate_fp16 = period_to_rate_fp16(c->period);
    }

    /* Portamento to note (3xx / 5xx) */
    if (c->porta_speed && c->target_period &&
        (c->effect == 0x3 || c->effect == 0x5)) {
        int p = (int)c->period;
        int t = (int)c->target_period;
        if (p > t) {
            p -= c->porta_speed;
            if (p < t) p = t;
        } else if (p < t) {
            p += c->porta_speed;
            if (p > t) p = t;
        }
        c->period = (uint16_t)p;
        c->rate_fp16 = period_to_rate_fp16(c->period);
    }

    /* Vibrato (4xx / 6xx) */
    if (c->vib_depth && (c->effect == 0x4 || c->effect == 0x6)) {
        int delta = (vib_table[c->vib_pos & 63] * c->vib_depth) >> 7;
        /* Apply vibrato: oscillate around c->period */
        uint16_t vib_period;
        if (c->vib_pos < 32)
            vib_period = c->period + delta;
        else
            vib_period = c->period - delta;
        c->rate_fp16 = period_to_rate_fp16(vib_period);
        c->vib_pos = (c->vib_pos + c->vib_speed) & 63;
    }

    /* Tremolo (7xy) — modulates out_volume, doesn't change c->volume */
    if (c->trem_depth && c->effect == 0x7) {
        int delta = (vib_table[c->trem_pos & 63] * c->trem_depth) >> 6;
        if (c->trem_pos >= 32) delta = -delta;
        int tv = c->volume + delta;
        if (tv < 0) tv = 0;
        if (tv > 64) tv = 64;
        c->out_volume = tv;
        c->trem_pos = (c->trem_pos + c->trem_speed) & 63;
    }

    /* Volume slide (Axy / 5xy / 6xy) */
    if (c->vol_slide) {
        c->volume += c->vol_slide;
        if (c->volume < 0) c->volume = 0;
        if (c->volume > 64) c->volume = 64;
    }

}

static void update_hardware(void) {
    for (int ch = 0; ch < MOD_NUM_CHANNELS; ch++) {
        channel_t *c = &channels[ch];
        if (c->voice < 0) continue;

        int vol = c->out_volume * 255 / 64;
        int vol_l = (vol * (256 - c->pan)) >> 8;
        int vol_r = (vol * c->pan) >> 8;
        of_mixer_set_voice_raw(c->voice, c->rate_fp16, vol_l, vol_r);
    }
}

/* ======================================================================
 * Timer callback — runs at tick rate
 * ====================================================================== */

static volatile int tick_pending = 0;

/* The kernel pins of_timer_set_callback at a 1 kHz HW tick regardless
 * of the requested hz, so divide down here to the MOD tick rate. */
static volatile uint32_t tick_accum_ms = 0;
static volatile uint32_t tick_period_ms = 20;  /* BPM 125 → 50 Hz → 20 ms */

static void mod_timer_tick(void) {
    if (++tick_accum_ms >= tick_period_ms) {
        tick_accum_ms = 0;
        tick_pending = 1;
    }
}

static inline void set_mod_rate_from_bpm(int bpm_val) {
    uint32_t ms = (bpm_val > 0) ? (uint32_t)(2500 / bpm_val) : 20u;
    if (ms == 0) ms = 1;
    tick_period_ms = ms;
}

static void process_tick(void) {
    if (!playing) return;

    if (current_tick == 0) {
        /* Row tick: read new notes */
        pattern_break = 0;
        int pattern = mod->order[current_order];
        for (int ch = 0; ch < MOD_NUM_CHANNELS; ch++) {
            mod_note_t n = read_note(pattern, current_row, ch);
            play_note(ch, &n);
        }
    } else {
        /* Inter-row tick: process effects */
        for (int ch = 0; ch < MOD_NUM_CHANNELS; ch++)
            tick_effects(ch);
    }

    /* Write final values to hardware */
    update_hardware();

    /* Advance tick/row/order */
    current_tick++;
    if (current_tick >= ticks_per_row) {
        current_tick = 0;
        /* EEx pattern delay: repeat current row for extra ticks */
        if (pattern_delay > 0) {
            pattern_delay--;
        } else {
            current_row++;
            if (current_row >= MOD_ROWS_PER_PAT) {
                current_row = 0;
                current_order++;
                if (current_order >= mod->song_length)
                    current_order = mod->restart_pos;
            }
        }
    }
}

/* ======================================================================
 * Effect Diagnostics
 *
 * Plays short test patterns to verify each effect.  Uses the first
 * loaded MOD's sample 1 for melodic tests and the first available
 * sample for percussive tests.
 * ====================================================================== */

typedef struct {
    const char *name;
    uint8_t effect;
    uint8_t param;
    uint16_t period;        /* 0 = use default C-2 (428) */
    uint16_t period2;       /* second note for porta-to-note, 0 = unused */
    int      ticks;         /* how many ticks to run */
} effect_test_t;

static const effect_test_t effect_tests[] = {
    { "Arpeggio 047",     0x0, 0x47, 428, 0,   48 },
    { "Porta Up",         0x1, 0x10, 428, 0,   48 },
    { "Porta Down",       0x2, 0x10, 428, 0,   48 },
    { "Porta to Note",    0x3, 0x08, 428, 214, 96 },  /* needs special handling */
    { "Vibrato deep",     0x4, 0x4F, 428, 0,   96 },
    { "Vibrato gentle",   0x4, 0x24, 428, 0,   96 },
    { "Tremolo deep",     0x7, 0x4F, 428, 0,   96 },
    { "Tremolo gentle",   0x7, 0x24, 428, 0,   96 },
    { "Vol Slide Up",     0xA, 0x40, 428, 0,   48 },
    { "Vol Slide Down",   0xA, 0x04, 428, 0,   48 },
    { "Set Volume 32",    0xC, 0x20, 428, 0,   24 },
    { "Set Volume 64",    0xC, 0x40, 428, 0,   24 },
    { "Speed 3",          0xF, 0x03, 428, 0,   24 },
    { "Speed 6",          0xF, 0x06, 428, 0,   24 },
    { "Sample Offset 10", 0x9, 0x10, 428, 0,   48 },
    { "Fine Porta Up",    0xE, 0x13, 428, 0,   24 },
    { "Fine Porta Down",  0xE, 0x23, 428, 0,   24 },
    { "Note Cut tick 3",  0xE, 0xC3, 428, 0,   24 },
    { "Retrigger /3",     0xE, 0x93, 428, 0,   48 },
    { "Fine VolUp +8",    0xE, 0xA8, 428, 0,   24 },
    { "Fine VolDown -8",  0xE, 0xB8, 428, 0,   24 },
    { "Pan Left",         0x8, 0x20, 428, 0,   48 },
};
#define NUM_EFFECT_TESTS (sizeof(effect_tests) / sizeof(effect_tests[0]))

static void run_effect_test(int idx) {
    if (!mod || !mod->sample_data[0]) return;
    const effect_test_t *t = &effect_tests[idx];

    /* Reset channel 0 but keep its pinned voice slot so retrigger has
     * a valid target. */
    channel_t *c = &channels[0];
    memset(c, 0, sizeof(*c));
    c->voice = 0;
    c->pan = 128;
    c->sample_idx = 1;

    /* Pick a starting volume that makes the effect audible */
    int is_vol_up   = (t->effect == 0xA && (t->param >> 4) > 0) ||
                      (t->effect == 0xE && (t->param >> 4) == 0xA);
    int is_vol_down = (t->effect == 0xA && (t->param & 0xF) > 0 && (t->param >> 4) == 0) ||
                      (t->effect == 0xE && (t->param >> 4) == 0xB);
    if (is_vol_up)        c->volume = 0;
    else if (is_vol_down) c->volume = 64;
    else                  c->volume = 48;

    /* Use a looping sample for effects that need sustain */
    int needs_sustain = (t->effect == 0x4 || t->effect == 0x7 ||
                         t->effect == 0x1 || t->effect == 0x2);
    if (needs_sustain) {
        for (int s = 0; s < MOD_NUM_SAMPLES; s++) {
            if (mod->samples[s].loop_length > 2 && mod->sample_data[s] &&
                mod->samples[s].volume > 0) {
                c->sample_idx = s + 1;
                break;
            }
        }
    }

    current_tick = 0;
    ticks_per_row = 6;
    bpm = 125;

    /* Porta-to-note needs a note already playing, then the effect on
     * a second "row" with the target period. */
    if (t->effect == 0x3 && t->period2) {
        mod_note_t first = {0};
        first.sample = 1;
        first.period = t->period;
        play_note(0, &first);
        update_hardware();
        /* Let it sound for a few ticks */
        for (int i = 0; i < 6; i++) {
            usleep(20 * 1000);
            current_tick = (i + 1) % ticks_per_row;
            update_hardware();
        }
        /* Now apply porta-to-note on a new "row" */
        mod_note_t porta = {0};
        porta.period = t->period2;
        porta.effect = 0x3;
        porta.param  = t->param;
        current_tick = 0;
        play_note(0, &porta);
        update_hardware();
        for (int tick = 1; tick < t->ticks; tick++) {
            usleep(20 * 1000);
            current_tick = tick % ticks_per_row;
            tick_effects(0);
            update_hardware();
        }
    } else {
        mod_note_t note = {0};
        note.period = t->period ? t->period : 428;
        note.effect = t->effect;
        note.param  = t->param;

        play_note(0, &note);
        update_hardware();
        {
            int vol = c->out_volume * 255 / 64;
            int vol_l = (vol * (255 - c->pan)) >> 8;
            int vol_r = (vol * c->pan) >> 8;
            printf("  t0: v=%d ov=%d per=%d rate=%lu vl=%d vr=%d voice=%d pan=%d\n",
                   c->volume, c->out_volume, c->period,
                   (unsigned long)c->rate_fp16, vol_l, vol_r, c->voice, c->pan);
        }

        for (int tick = 1; tick < t->ticks; tick++) {
            usleep(20 * 1000);
            current_tick = tick % ticks_per_row;
            tick_effects(0);
            update_hardware();
            if (tick <= 6 || tick == t->ticks - 1) {
                int vol = c->out_volume * 255 / 64;
                int vol_l = (vol * (255 - c->pan)) >> 8;
                int vol_r = (vol * c->pan) >> 8;
                int pos = (c->voice >= 0) ? of_mixer_get_position(c->voice) : -1;
                int active = (c->voice >= 0) ? of_mixer_voice_active(c->voice) : 0;
                printf("  t%d: ov=%d rate=%lu vl=%d vr=%d pos=%d act=%d\n",
                       tick, c->out_volume,
                       (unsigned long)c->rate_fp16, vol_l, vol_r, pos, active);
            }
        }
    }

    of_mixer_stop(c->voice);
    /* The SELECT-toggle handler will restore channel state on return. */
}

/* ======================================================================
 * Main
 * ====================================================================== */

int main(void) {
    /* Audio-only app — stay in terminal mode for status output */
    printf("=== MOD Player Demo ===\n\n");

    /* A tracker is nothing without the mixer — gate on the caps bit. */
    if (!of_has_feature(OF_HW_MIXER)) {
        printf("No mixer on this platform (OF_HW_MIXER clear).\n");
        for (;;) usleep(100 * 1000);
    }

    /* Initialize mixer */
    of_mixer_init(32, OF_MIXER_OUTPUT_RATE);

    /* Load MOD files from slots 4 and 5 */
    static const char *slot_names[] = { "slot:4", "slot:5" };
    static uint8_t *file_bufs[MAX_SONGS];
    (void)file_bufs;  /* kept alive so pattern_data pointers remain valid */

    for (int s = 0; s < MAX_SONGS; s++) {
        printf("Loading MOD from %s...\n", slot_names[s]);
        FILE *f = fopen(slot_names[s], "rb");
        if (!f) { printf("  not found, skipping\n"); continue; }

        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (size < 1084) { printf("  too small, skipping\n"); fclose(f); continue; }

        uint8_t *data = (uint8_t *)malloc(size);
        if (!data) { printf("  out of memory\n"); fclose(f); continue; }

        size_t got = fread(data, 1, size, f);
        fclose(f);

        if (parse_mod(&songs[num_songs], data, (uint32_t)got) == 0) {
            file_bufs[num_songs] = data;  /* keep alive — pattern_data points into it */
            num_songs++;
        } else {
            free(data);
        }
    }

    if (num_songs == 0) {
        printf("No valid MOD files found in slots 4-5.\n");
        for (;;) usleep(100000);
    }

    printf("Loaded %d song(s)\n", num_songs);
    cur_song = 0;
    mod = &songs[cur_song];

    /* Initialize channels — pin each MOD channel to a fixed mixer
     * voice slot (0..3) for the lifetime of the app.  Voices are
     * activated in-place via of_mixer_retrigger, never reallocated. */
    for (int i = 0; i < MOD_NUM_CHANNELS; i++) {
        channels[i].voice = i;
        channels[i].pan = default_pan[i];
        channels[i].volume = 0;
    }

    /* MOD tick rate = BPM * 2 / 5.  HW timer is pinned at 1 kHz;
     * mod_timer_tick divides down to tick_period_ms. */
    set_mod_rate_from_bpm(bpm);
    printf("Tick rate: %d Hz (BPM=%d, speed=%d)\n",
           (bpm * 2) / 5, bpm, ticks_per_row);
    of_timer_set_callback(mod_timer_tick, 1000);

    printf("\nA=next song B=next pattern SELECT=effects\n\n");

    /* HW register update test: play note, change rate after 500ms */
    if (mod->sample_data[0]) {
        mod_sample_t *s = &mod->samples[0];
        uint32_t rate1 = OF_MIXER_RATE_FP16(8363);
        uint32_t rate2 = OF_MIXER_RATE_FP16(16726);  /* double pitch */
        int v = of_mixer_play((const uint8_t *)mod->sample_data[0],
                              s->length, 8363, 0, 200);
        if (v >= 0 && s->loop_length > 2)
            of_mixer_set_loop(v, s->loop_start, s->loop_start + (int)s->loop_length);
        printf("HW test: voice=%d rate1=%lu\n", v, (unsigned long)rate1);
        usleep(500 * 1000);
        if (v >= 0) {
            printf("  changing rate to %lu...\n", (unsigned long)rate2);
            of_mixer_set_rate_raw(v, rate2);
        }
        usleep(500 * 1000);
        if (v >= 0) {
            printf("  changing rate back to %lu...\n", (unsigned long)rate1);
            of_mixer_set_rate_raw(v, rate1);
        }
        usleep(500 * 1000);
        if (v >= 0) {
            printf("  setting volume to 0...\n");
            of_mixer_set_vol_lr(v, 0, 0);
        }
        usleep(200 * 1000);
        if (v >= 0) {
            printf("  setting volume to 200...\n");
            of_mixer_set_vol_lr(v, 200, 200);
        }
        usleep(500 * 1000);
        if (v >= 0) of_mixer_stop(v);
        printf("HW test done\n");
    }

    int mode = 0;  /* 0 = play, 1 = effect diag */
    int diag_idx = 0;

    /* Main loop.  On target, sleep with wfi between timer ISRs so MIE stays
     * on; usleep would ECALL into a kernel busy-wait with MIE cleared and
     * silently drop most of the 1 kHz timer fires. */
    int display_throttle = 0;
    for (;;) {
        if (mode == 0 && tick_pending) {
            tick_pending = 0;
            process_tick();
            set_mod_rate_from_bpm(bpm);  /* Fxx may have changed bpm */
        }

        /* Poll input + status print at ~10 Hz.  Each printf is a heavy
         * ECALL (synchronous UART/FB writes run with MIE off), so keep
         * them infrequent to avoid losing timer ISRs. */
        display_throttle++;
        if (display_throttle >= 100) {
            display_throttle = 0;

            of_input_poll();

            if (of_btn_pressed(OF_BTN_SELECT)) {
                of_mixer_stop_all();
                for (int i = 0; i < MOD_NUM_CHANNELS; i++) {
                    channels[i].voice = i;
                    channels[i].pan = default_pan[i];
                    channels[i].volume = 0;
                }
                mode = !mode;
                if (mode == 0) {
                    current_order = current_row = current_tick = 0;
                    ticks_per_row = 6;
                    bpm = 125;
                    set_mod_rate_from_bpm(bpm);
                }
                printf("\r\033[K %s\n", mode ? "EFFECT DIAGNOSTICS" : "PLAY MODE");
            }

            if (mode == 0) {
                if (of_btn_pressed(OF_BTN_A) && num_songs > 1) {
                    of_mixer_stop_all();
                    cur_song = (cur_song + 1) % num_songs;
                    mod = &songs[cur_song];
                    current_order = current_row = current_tick = 0;
                    ticks_per_row = 6;
                    bpm = 125;
                    set_mod_rate_from_bpm(bpm);
                    printf("\r\033[K>>> Song %d: %s\n", cur_song + 1, mod->title);
                }
                if (of_btn_pressed(OF_BTN_B)) {
                    current_row = current_tick = 0;
                    current_order++;
                    if (current_order >= mod->song_length)
                        current_order = 0;
                }
                printf("\r\033[K Song:%d Ord:%02d Pat:%02d Row:%02d Spd:%d BPM:%d",
                       cur_song + 1, current_order, mod->order[current_order],
                       current_row, ticks_per_row, bpm);
            } else {
                int changed = -1;
                if (of_btn_pressed(OF_BTN_RIGHT))
                    changed = (diag_idx + 1) % (int)NUM_EFFECT_TESTS;
                if (of_btn_pressed(OF_BTN_LEFT))
                    changed = (diag_idx + (int)NUM_EFFECT_TESTS - 1) % (int)NUM_EFFECT_TESTS;
                if (of_btn_pressed(OF_BTN_A))
                    changed = diag_idx;

                if (changed >= 0) {
                    diag_idx = changed;
                    printf("\r\033[K [%2d/%d] %s\n",
                           diag_idx + 1, (int)NUM_EFFECT_TESTS,
                           effect_tests[diag_idx].name);
                    run_effect_test(diag_idx);
                    printf("\r\033[K vol=%d voice=%d",
                           channels[0].volume, channels[0].voice);
                }
            }
        }

        mod_idle_wait();
    }
}
