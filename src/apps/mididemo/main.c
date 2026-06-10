//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * mididemo — General-MIDI playback via the SoundFont-driven sample synth
 *
 * Canonical example of:
 *   - Loading a SoundFont bank with of_smp_bank_load (or reusing the
 *     kernel's auto-loaded preload buffer when one is exposed via
 *     OF_SVC->smp_bank_preload_base)
 *   - Driving the CPU MIDI voice engine with of_smp_voice_*: note_on,
 *     note_off, channel CC updates (volume, expression, pan, bend),
 *     and the 1 kHz logical envelope tick dispatched by the timer ISR
 *   - of_midi_play() to spool a SMF file through the synth — the
 *     parser runs in main(), but the actual mixer slot ops happen
 *     inside the timer ISR via of_midi_pump (see project memory
 *     `midi_isr_pump`)
 *   - smp_voice_tick_get_stats() for live voice-load diagnostics
 *
 * Modes:
 *   Default          play the MIDI file at slot:4
 *   D-pad UP toggle  diagnostic instrument loop — sustained notes for
 *                    every program so you can audit each preset
 *
 * Controls:
 *   START      play / pause
 *   SELECT     restart
 *   D-pad UP   diagnostic mode toggle
 *   L1 / R1    master volume down / up
 */

#include "of.h"
#include "of_smp_bank.h"
#include "of_smp_voice.h"
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

/* Maximum MIDI file size we support */
#define MIDI_MAX_SIZE   (256 * 1024)

/* Buffer for the loaded MIDI file (aligned for DMA) */
static uint8_t midi_buf[MIDI_MAX_SIZE] __attribute__((aligned(512)));
static uint32_t midi_len;

/* Per-instrument diagnostic MIDIs.
 * Each plays ONE sustained note for ~2 seconds so you can clearly hear
 * the timbre. The loop in main() steps through them with screen labels. */
typedef struct {
    const char *name;
    int program;     /* GM program (0-127), or -1 for drums */
    int channel;     /* MIDI channel */
    int note;        /* MIDI note number */
    uint8_t buf[256];
    uint32_t len;
} diag_inst_t;

static diag_inst_t diag_inst[] = {
    /* GM melodic instruments — each on its own channel */
    { "00 Acoustic Grand Piano",     0, 0, 60, {0}, 0 },
    { "24 Nylon Guitar",            24, 0, 52, {0}, 0 },
    { "27 Electric Guitar Clean",   27, 0, 52, {0}, 0 },
    { "29 Overdriven Guitar",       29, 0, 52, {0}, 0 },
    { "30 Distortion Guitar",       30, 0, 52, {0}, 0 },
    { "33 Electric Bass finger",    33, 1, 28, {0}, 0 },
    { "34 Electric Bass pick",      34, 1, 28, {0}, 0 },
    { "38 Synth Bass 1",            38, 1, 28, {0}, 0 },
    { "56 Trumpet",                 56, 0, 60, {0}, 0 },
    { "65 Alto Sax",                65, 0, 60, {0}, 0 },
    { "73 Flute",                   73, 0, 72, {0}, 0 },
    { "80 Square Lead",             80, 0, 60, {0}, 0 },
    /* Drums (channel 9) */
    { "Drum: Bass Drum",            -1, 9, 35, {0}, 0 },
    { "Drum: Acoustic Snare",       -1, 9, 38, {0}, 0 },
    { "Drum: Closed Hi-Hat",        -1, 9, 42, {0}, 0 },
    { "Drum: Crash Cymbal",         -1, 9, 49, {0}, 0 },
};
#define DIAG_INST_COUNT  (sizeof(diag_inst)/sizeof(diag_inst[0]))
#define RAW_MAX_LAYERS   4
#define RAW_COPY_BYTES   (256 * 1024)

static int raw_voice_ids[RAW_MAX_LAYERS] = { -1, -1, -1, -1 };
static int16_t *raw_copy_bufs[RAW_MAX_LAYERS];
static int raw_use_copy;
static int midi_ready;

static int vlq_append(uint8_t *p, int *t, uint32_t v) {
    int n = 0;
    if (v < 0x80) {
        p[(*t)++] = v;
        n = 1;
    } else if (v < 0x4000) {
        p[(*t)++] = (v >> 7) | 0x80;
        p[(*t)++] = v & 0x7F;
        n = 2;
    } else {
        p[(*t)++] = (v >> 14) | 0x80;
        p[(*t)++] = ((v >> 7) & 0x7F) | 0x80;
        p[(*t)++] = v & 0x7F;
        n = 3;
    }
    return n;
}

/* Build a tiny single-note MIDI for one instrument. */
static void build_diag_inst(diag_inst_t *d) {
    static const uint8_t hdr[] = {
        'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0,96
    };
    memcpy(d->buf, hdr, sizeof(hdr));

    static uint8_t track[128];
    int t = 0;

    /* Tempo: 120 BPM */
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x51; track[t++] = 0x03;
    track[t++] = 0x07; track[t++] = 0xA1; track[t++] = 0x20;

    /* Program change (skip for drums) */
    if (d->program >= 0) {
        track[t++] = 0;
        track[t++] = 0xC0 | d->channel;
        track[t++] = d->program;
    }
    /* Channel volume max */
    track[t++] = 0; track[t++] = 0xB0 | d->channel; track[t++] = 7; track[t++] = 127;

    /* Sustained note for ~2 seconds (384 ticks at 120 BPM, 96 tpq) */
    track[t++] = 0; track[t++] = 0x90 | d->channel; track[t++] = d->note; track[t++] = 100;
    vlq_append(track, &t, 384);
    track[t++] = 0x80 | d->channel; track[t++] = d->note; track[t++] = 0;

    /* End of track */
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x2F; track[t++] = 0x00;

    /* MTrk */
    int o = sizeof(hdr);
    d->buf[o++] = 'M'; d->buf[o++] = 'T'; d->buf[o++] = 'r'; d->buf[o++] = 'k';
    d->buf[o++] = (t >> 24) & 0xFF;
    d->buf[o++] = (t >> 16) & 0xFF;
    d->buf[o++] = (t >> 8) & 0xFF;
    d->buf[o++] = t & 0xFF;
    memcpy(d->buf + o, track, t);
    d->len = o + t;
}

static void build_all_diag(void) {
    for (unsigned i = 0; i < DIAG_INST_COUNT; i++)
        build_diag_inst(&diag_inst[i]);
}

static void raw_stop_all(void) {
    for (int i = 0; i < RAW_MAX_LAYERS; i++) {
        if (raw_voice_ids[i] >= 0) {
            of_mixer_stop(raw_voice_ids[i]);
            raw_voice_ids[i] = -1;
        }
    }
}

static int16_t *raw_copy_buf_get(int layer) {
    if (layer < 0 || layer >= RAW_MAX_LAYERS)
        return 0;
    if (!raw_copy_bufs[layer])
        raw_copy_bufs[layer] = (int16_t *)of_mixer_alloc_samples(RAW_COPY_BYTES);
    return raw_copy_bufs[layer];
}

static int ensure_midi_ready(void) {
    if (midi_ready)
        return 0;

    int rc = of_midi_init();
    if (rc < 0) {
        printf("\033[12;2H MIDI init failed rc=%d          ", rc);
        return rc;
    }

    midi_ready = 1;
    return 0;
}

static void raw_play_inst(int idx, int note) {
    const ofsf_zone_t *zones[RAW_MAX_LAYERS];
    const ofsf_header_t *hdr = of_smp_bank_get();
    const uint8_t *sbase = (const uint8_t *)of_smp_bank_sample_base();
    int bank = (diag_inst[idx].channel == 9) ? 128 : 0;
    int program = (diag_inst[idx].program >= 0) ? diag_inst[idx].program : 0;
    int played = 0;

    if (note < 0) note = 0;
    if (note > 127) note = 127;

    raw_stop_all();

    int n = 0;
    if (hdr && sbase)
        n = of_smp_zone_lookup(bank, program, note, 100, zones, RAW_MAX_LAYERS);

    printf("\033[12;2H Raw src: %-6s                           ",
           raw_use_copy ? "copy" : "bank");
    printf("\033[14;2H Raw note=%3d bank=%3d prog=%3d zones=%d          ",
           note, bank, program, n);

    for (int i = 0; i < n; i++) {
        const uint8_t *sample_ptr = sbase + zones[i]->sample_offset;
        uint32_t sample_count = zones[i]->sample_length;
        uint32_t sample_bytes = sample_count * sizeof(int16_t);
        const uint8_t *play_ptr = sample_ptr;

        if (raw_use_copy) {
            int16_t *copy_buf = raw_copy_buf_get(i);
            if (!copy_buf || sample_bytes > RAW_COPY_BYTES) {
                printf("\033[15;2H Raw copy fail: layer=%d bytes=%u       ", i, (unsigned)sample_bytes);
                continue;
            }
            memcpy(copy_buf, sample_ptr, sample_bytes);
            play_ptr = (const uint8_t *)copy_buf;
        }

        int v = of_mixer_play(play_ptr,
                              sample_count,
                              hdr->sample_rate,
                              0, 220);
        if (v < 0)
            continue;
        raw_voice_ids[played++] = v;
        of_mixer_set_group(v, OF_MIXER_GROUP_MUSIC);
        if (zones[i]->loop_mode == OFSF_LOOP_FORWARD || zones[i]->loop_mode == OFSF_LOOP_BIDI) {
            of_mixer_set_loop(v, zones[i]->loop_start, zones[i]->loop_end);
            if (zones[i]->loop_mode == OFSF_LOOP_BIDI)
                of_mixer_set_bidi(v, 1);
        }
    }

    printf("\033[15;2H Raw voices: %d/%d                      ", played, n);
}


__attribute__((unused))
static int load_midi_file(void) {
    FILE *f = fopen("music.mid", "rb");
    if (!f) return -1;

    size_t n = fread(midi_buf, 1, MIDI_MAX_SIZE, f);
    fclose(f);

    if (n < 14)
        return -2;

    if (midi_buf[0] != 'M' || midi_buf[1] != 'T' ||
        midi_buf[2] != 'h' || midi_buf[3] != 'd')
        return -10;

    midi_len = (uint32_t)n;
    return 0;
}

/* Mode: 0 = MIDI file player, 1 = instrument diagnostic,
 *       2 = raw sample (direct mixer) */
#define MODE_PLAY  0
#define MODE_DIAG  1
#define MODE_RAW   2
#define MODE_COUNT 3

int main(void) {
    printf("\033[2J\033[H");
    printf("    openfpgaOS MIDI Demo\n");
    printf("    ====================\n\n");

    build_all_diag();

    /* Initialize mixer — required by the sample-based MIDI backend */
    of_mixer_init(OF_MIXER_MAX_VOICES, OF_MIXER_OUTPUT_RATE);
    of_mixer_set_master_volume(255);
    of_mixer_set_group_volume(OF_MIXER_GROUP_MUSIC, 255);
    of_mixer_set_group_volume(OF_MIXER_GROUP_SFX, 255);

    /* Sample bank is auto-loaded by the kernel at boot — no init call
     * is needed. If no .ofsf was staged, of_smp_bank_get() returns NULL. */
    const ofsf_header_t *bhdr = of_smp_bank_get();
    if (!bhdr) {
        printf(" No SoundFont found!\n");
        printf(" Place a .ofsf in a data slot\n");
        while (1) {}
    }
    printf(" Bank loaded (%.1f KB)\n",
           bhdr->sample_data_size / 1024.0f);

    /* Try to load the MIDI file (music.mid) by filename */
    int have_midi = (load_midi_file() == 0);
    if (have_midi)
        printf(" MIDI file: %u bytes\n", (unsigned)midi_len);
    else
        printf(" No MIDI file found\n");

    printf(" %u diagnostic instruments\n", (unsigned)DIAG_INST_COUNT);

    int mode = have_midi ? MODE_PLAY : MODE_DIAG;
    int volume = 255;
    int paused = 0;
    int idx = 0;
    int auto_advance = 1;
    int raw_octave = 0;
    uint32_t note_start_ms = 0;
    uint32_t last_probe_ms = 0;

    printf("\n SELECT=switch mode  L1/R1=volume\n");
    printf(" Play: START=pause A=restart\n");
    printf(" Diag: LEFT/RIGHT=prev/next START=auto A=replay\n");
    printf(" Raw:  LEFT/RIGHT=prev/next START=src X/Y=octave\n\n");

    of_input_state_t state;
    memset(&state, 0, sizeof(state));
    goto enter_mode;

    while (1) {
        of_input_poll();
        of_input_state(0, &state);

        /* SELECT = switch mode */
        if (state.buttons_pressed & OF_BTN_SELECT) {
            if (midi_ready)
                of_midi_stop();
            raw_stop_all();
            paused = 0;
            idx = 0;
            raw_octave = 0;

            /* Cycle through playback, diagnostic, and raw mixer modes. */
            mode = (mode + 1) % MODE_COUNT;
            if (mode == MODE_PLAY && !have_midi) mode = MODE_DIAG;

enter_mode:
            printf("\033[10;2H                                       ");
            if (mode == MODE_PLAY) {
                printf("\033[10;2H MODE: MIDI File Player");
                if (ensure_midi_ready() == 0) {
                    int prc = of_midi_play(midi_buf, midi_len, 1);
                    printf("\033[14;2H play rc=%d len=%u     ", prc, (unsigned)midi_len);
                }
            } else if (mode == MODE_DIAG) {
                printf("\033[10;2H MODE: Instrument Diagnostic");
                if (ensure_midi_ready() == 0) {
                    int prc = of_midi_play(diag_inst[idx].buf, diag_inst[idx].len, 0);
                    printf("\033[14;2H diag rc=%d len=%u     ", prc, diag_inst[idx].len);
                    note_start_ms = of_time_ms();
                }
            } else if (mode == MODE_RAW) {
                printf("\033[10;2H MODE: Raw Sample Playback");
                raw_play_inst(idx, diag_inst[idx].note);
            }
            printf("\033[11;2H >>> %-30s",
                   mode == MODE_PLAY ? "Playing MIDI file" : diag_inst[idx].name);
        }

        /* Volume (all modes) */
        if (state.buttons_pressed & OF_BTN_L1) {
            volume -= 32; if (volume < 0) volume = 0;
            if (mode == MODE_RAW)
                of_mixer_set_group_volume(OF_MIXER_GROUP_MUSIC, volume);
            else
                of_midi_set_volume(volume);
            printf("\033[13;2H Vol: %3d/255  ", volume);
        }
        if (state.buttons_pressed & OF_BTN_R1) {
            volume += 32; if (volume > 255) volume = 255;
            if (mode == MODE_RAW)
                of_mixer_set_group_volume(OF_MIXER_GROUP_MUSIC, volume);
            else
                of_midi_set_volume(volume);
            printf("\033[13;2H Vol: %3d/255  ", volume);
        }

        /* Mode-specific controls */
        if (mode == MODE_PLAY) {
            if (state.buttons_pressed & OF_BTN_START) {
                paused = !paused;
                if (midi_ready) {
                    if (paused) of_midi_pause(); else of_midi_resume();
                }
                printf("\033[12;2H %s   ", paused ? "PAUSED " : "PLAYING");
            }
            if (state.buttons_pressed & OF_BTN_A) {
                if (ensure_midi_ready() == 0) {
                    of_midi_stop();
                    of_midi_play(midi_buf, midi_len, 1);
                    paused = 0;
                    printf("\033[12;2H RESTART       ");
                }
            }
        } else if (mode == MODE_DIAG) {
            int change = -1;
            if (state.buttons_pressed & OF_BTN_RIGHT)
                change = (idx + 1) % (int)DIAG_INST_COUNT;
            if (state.buttons_pressed & OF_BTN_LEFT)
                change = (idx + (int)DIAG_INST_COUNT - 1) % (int)DIAG_INST_COUNT;
            if (state.buttons_pressed & OF_BTN_A)
                change = idx;
            if (state.buttons_pressed & OF_BTN_START) {
                auto_advance = !auto_advance;
                printf("\033[12;2H Auto: %s   ", auto_advance ? "ON " : "OFF");
            }
            if (auto_advance && change < 0) {
                uint32_t now = of_time_ms();
                if (now - note_start_ms > 2500)
                    change = (idx + 1) % (int)DIAG_INST_COUNT;
            }
            if (change >= 0) {
                idx = change;
                if (ensure_midi_ready() == 0) {
                    of_midi_stop();
                    of_midi_play(diag_inst[idx].buf, diag_inst[idx].len, 0);
                    note_start_ms = of_time_ms();
                }
                printf("\033[11;2H >>> %-30s", diag_inst[idx].name);
            }
        } else if (mode == MODE_RAW) {
            int change = -1;
            int replay = 0;

            if (state.buttons_pressed & OF_BTN_RIGHT)
                change = (idx + 1) % (int)DIAG_INST_COUNT;
            if (state.buttons_pressed & OF_BTN_LEFT)
                change = (idx + (int)DIAG_INST_COUNT - 1) % (int)DIAG_INST_COUNT;
            if (state.buttons_pressed & OF_BTN_A)
                replay = 1;
            if (state.buttons_pressed & OF_BTN_START) {
                raw_use_copy = !raw_use_copy;
                replay = 1;
            }
            if (state.buttons_pressed & OF_BTN_X) {
                if (raw_octave > -2) raw_octave--;
                replay = 1;
            }
            if (state.buttons_pressed & OF_BTN_Y) {
                if (raw_octave < 2) raw_octave++;
                replay = 1;
            }

            if (change >= 0) {
                idx = change;
                raw_octave = 0;
                replay = 1;
                printf("\033[11;2H >>> %-30s", diag_inst[idx].name);
            }

            if (replay)
                raw_play_inst(idx, diag_inst[idx].note + raw_octave * 12);
        }

        /* Tick-cost probe: print stats every ~1 s.
         * Budget is 2000 us per MIDI pump callback.
         *
         * of_midi_pump() is now driven by the machine-timer ISR at 50 Hz
         * (installed by of_midi_play), so printf stalls on the main thread
         * no longer starve the mixer.  We must NOT call of_midi_pump() from
         * here — doing so would race the ISR on M/voice state. */
        uint32_t now_ms = of_time_ms();
        if (now_ms - last_probe_ms >= 1000) {
            last_probe_ms = now_ms;
            smp_tick_stats_t s;
            smp_voice_tick_get_stats(&s);
            printf("\033[17;2H tick max=%4u us last=%4u us spikes=%u/%u peak_v=%u  ",
                   (unsigned)s.cycles_max, (unsigned)s.cycles_last,
                   (unsigned)s.spike_count, (unsigned)s.tick_count,
                   (unsigned)s.active_peak);
            printf("\033[18;2H stages: sus=%u rel=%u dec=%u held=%u          ",
                   (unsigned)s.stage_sustain, (unsigned)s.stage_release,
                   (unsigned)s.stage_decay, (unsigned)s.sustain_held);
            /* Per-channel active voice count + GM program number.
             * Each channel prints as "Cc=Vp" where c=channel (0-F hex),
             * V=voice count, p=program number.  Channels with 0 voices
             * are hidden so only the currently-producing channels show. */
            char chbuf[128];
            int clen = 0;
            for (int ch = 0; ch < 16 && clen < (int)sizeof(chbuf) - 12; ch++) {
                if (s.ch_active[ch] == 0) continue;
                clen += snprintf(chbuf + clen, sizeof(chbuf) - clen,
                                 "%X:%u/%d ", ch,
                                 (unsigned)s.ch_active[ch],
                                 of_midi_get_program(ch));
            }
            printf("\033[19;2H ch(v/prg): %-80s", chbuf);
            /* Instrumentation — MMIO write counts + pump intervals.
             * mmio: how many HW writes actually fired in the last second
             *   (after the cache-skip guards).  Saturation shows up here.
             * pump: intervals between of_midi_pump() calls; "brst" counts
             *   pumps that fired >1 tick (ticks bursting) and "over"
             *   counts pumps that blew the tick_budget (catch-up dropped). */
            unsigned pmin = (s.pump_interval_min_us == 0xFFFFFFFFu)
                              ? 0u : s.pump_interval_min_us;
            printf("\033[20;2H mmio: filt=%5u rate=%5u vol=%5u           ",
                   (unsigned)s.filter_writes,
                   (unsigned)s.rate_writes,
                   (unsigned)s.vol_writes);
            printf("\033[21;2H pump: n=%5u int=%u..%uus brst=%u over=%u      ",
                   (unsigned)s.pump_count,
                   pmin, (unsigned)s.pump_interval_max_us,
                   (unsigned)s.pump_burst_count,
                   (unsigned)s.pump_budget_exceeded);
            smp_voice_tick_reset_stats();
        }

        usleep(1 * 1000);
    }

    return 0;
}
