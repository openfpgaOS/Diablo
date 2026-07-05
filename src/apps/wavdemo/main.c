//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * wavdemo — load a PCM WAV and play it through the hardware mixer
 *
 * Canonical example of:
 *   - Reading a binary file via fopen("slot:N", "rb") + fread
 *   - Loading the converted samples into the SDRAM mixer sample pool via
 *     of_mixer_alloc_samples — the pool the HW mixer's voice-fetch master
 *     reads, kept coherent by the OS (the pattern the SDK mixer tests use).
 *   - The handle + group mixer API: of_mixer_alloc_for_group_h() returns
 *     an of_mixer_handle_t that stays valid across voice stealing, so we
 *     never touch a raw voice index. _handle_active() reports whether our
 *     sound still owns its voice; _get_position_h() / _set_position_h()
 *     drive a real pause/resume seek; _stop_h() silences it.
 *   - of_timer_set_callback for a 30 Hz progress redraw — the ISR only
 *     sets a volatile dirty flag; the redraw runs in main() so we never
 *     printf from interrupt context.
 *
 * The WAV must be 8 or 16-bit PCM, mono or stereo (stereo is folded to
 * mono on load — a mixer voice is mono-source plus pan).
 *
 * Controls:
 *   START   pause / resume (resume seeks back to where it paused)
 *   SELECT  restart from the beginning
 */

#include "of.h"
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#define MAX_WAV_SIZE    (4 * 1024 * 1024)

/* WAV file lives in BSS; large but fine — BSS is in SDRAM and
 * zero-initialised at app start.  Kept aligned(4) so the WAV header
 * 32-bit reads (fmt/sample_rate/byte_rate) hit aligned addresses on
 * any compiler. */
static uint8_t wav_buf[MAX_WAV_SIZE] __attribute__((aligned(4)));

/* Converted 16-bit signed mono PCM, in a plain malloc'd SDRAM buffer. */
static int16_t  *sample_buf;
static uint32_t  total_samples;
static uint32_t  sample_rate;

/* The voice handle stays valid even if the mixer steals our voice for a
 * higher-priority sound — of_mixer_handle_active() tells us if we still
 * own it.  Compare against OF_MIXER_HANDLE_INVALID, never 0-as-voice. */
static of_mixer_handle_t voice = OF_MIXER_HANDLE_INVALID;

/* Playback state machine. */
enum { ST_PAUSED, ST_PLAYING, ST_DONE };
static int      state = ST_PAUSED;
static int      pause_pos;            /* sample offset captured at pause */
static uint32_t play_start_ms;        /* wall-clock anchor for the fallback bar */
static volatile int progress_dirty;   /* set by the 30 Hz timer ISR */

/* Timer ISR: only flag the bar for redraw — no printf from interrupt ctx. */
static void timer_tick(void) {
    if (state == ST_PLAYING)
        progress_dirty = 1;
}

/* ── WAV parsing ─────────────────────────────────────────────────────── */

typedef struct {
    uint16_t format, channels;
    uint32_t sample_rate, byte_rate;
    uint16_t block_align, bits_per_sample;
    const uint8_t *data;
    uint32_t data_size;
} wav_info_t;

static uint16_t read16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static int16_t read_s16(const uint8_t *p) {
    return (int16_t)read16(p);
}
static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int wav_parse(const uint8_t *buf, uint32_t len, wav_info_t *info) {
    if (len < 44) return -1;
    if (memcmp(buf, "RIFF", 4) != 0) return -2;
    if (memcmp(buf + 8, "WAVE", 4) != 0) return -3;
    uint32_t pos = 12;
    int have_fmt = 0;
    while (pos + 8 <= len) {
        uint32_t csz = read32(buf + pos + 4);
        if (memcmp(buf + pos, "fmt ", 4) == 0) {
            if (csz < 16) return -4;
            info->format = read16(buf + pos + 8);
            info->channels = read16(buf + pos + 10);
            info->sample_rate = read32(buf + pos + 12);
            info->byte_rate = read32(buf + pos + 16);
            info->block_align = read16(buf + pos + 20);
            info->bits_per_sample = read16(buf + pos + 22);
            have_fmt = 1;
        } else if (memcmp(buf + pos, "data", 4) == 0) {
            if (!have_fmt) return -5;
            info->data = buf + pos + 8;
            info->data_size = csz;
            if (pos + 8 + csz > len) info->data_size = len - pos - 8;
            return 0;
        }
        pos += 8 + csz;
        if (csz & 1) pos++;
    }
    return -6;
}

/* Fold one frame to a single 16-bit signed sample (mono, or L/R averaged). */
static int16_t fold_sample(const wav_info_t *w, uint32_t i) {
    const uint8_t *pcm = w->data + i * w->block_align;
    if (w->bits_per_sample == 16) {
        int16_t l = read_s16(pcm);
        int16_t r = (w->channels >= 2) ? read_s16(pcm + 2) : l;
        return (w->channels >= 2) ? (int16_t)(((int32_t)l + r) >> 1) : l;
    }
    /* 8-bit PCM is unsigned, biased at 128. */
    return (w->channels >= 2)
        ? (int16_t)((((int)pcm[0] + pcm[1]) / 2 - 128) << 8)
        : (int16_t)(((int)pcm[0] - 128) << 8);
}

/* ── Playback (handle + group API) ───────────────────────────────────── */

/* (Re)start the WAV on a MUSIC-group voice, optionally seeking to a sample
 * offset.  Returns a handle that survives voice stealing. */
static of_mixer_handle_t start_play(int from_sample) {
    of_mixer_handle_t h = of_mixer_alloc_for_group_h(
        OF_MIXER_GROUP_MUSIC, (const uint8_t *)sample_buf,
        total_samples, sample_rate, /*priority*/ 0, /*volume*/ 255);
    if (h != OF_MIXER_HANDLE_INVALID && from_sample > 0)
        of_mixer_set_position_h(h, from_sample);
    /* Anchor the wall-clock bar so it agrees with the seek position. */
    play_start_ms = of_time_ms() -
        (uint32_t)((uint64_t)from_sample * 1000 / sample_rate);
    return h;
}

/* Current playback position in samples.  Prefer the mixer's own counter;
 * fall back to wall-clock where the position service is absent (PC stub /
 * older firmware). */
static uint32_t play_position(void) {
    int pos = of_mixer_get_position_h(voice);
    if (pos >= 0)
        return (uint32_t)pos;
    uint32_t elapsed = of_time_ms() - play_start_ms;
    return (uint32_t)((uint64_t)elapsed * sample_rate / 1000);
}

static void draw_progress(void) {
    uint32_t pos = play_position();
    if (pos > total_samples) pos = total_samples;
    int pct = (int)((uint64_t)pos * 100 / total_samples);
    int filled = pct * 28 / 100;
    printf("\033[11;2H[");
    for (int i = 0; i < 28; i++)
        putchar(i < filled ? '#' : '-');
    printf("] %d%%  ", pct);

    /* Finished?  Trust the voice's own active flag when we have a real
     * position counter; otherwise (PC/old fw) decide by wall-clock. */
    int done = (of_mixer_get_position_h(voice) >= 0)
        ? (!of_mixer_handle_active(voice) && of_time_ms() - play_start_ms > 150)
        : (pos >= total_samples);
    if (done) {
        state = ST_DONE;
        printf("\033[9;3H[FINISHED]   \n");
    }
}

static void set_status(const char *s) {
    printf("\033[9;3H[%s]   \n", s);
}

/* ── main ────────────────────────────────────────────────────────────── */

static void park_msg(const char *msg) {
    printf("  %s\n", msg);
    for (;;) usleep(100 * 1000);
}

int main(void) {
    printf("\033[2J\033[H");
    printf("  WAV Player Demo\n\n");

    if (!of_has_feature(OF_HW_MIXER))
        park_msg("No HW mixer on this platform.");

    /* Bring the mixer up first; sample memory is plain malloc SDRAM. */
    of_mixer_init(OF_MIXER_MAX_VOICES, OF_MIXER_OUTPUT_RATE);
    of_mixer_set_master_volume(255);
    of_mixer_set_group_volume(OF_MIXER_GROUP_MUSIC, 255);

    /* This app's instance.json maps slot:4 to the WAV file.  We use the
     * literal `slot:4` path for portability — apps that prefer filename
     * opens can of_file_slot_register(4, "song.wav") first. */
    printf("  Loading WAV file...\n");
    FILE *f = fopen("slot:4", "rb");
    if (!f) park_msg("Error: cannot open slot:4");
    uint32_t file_size = (uint32_t)fread(wav_buf, 1, MAX_WAV_SIZE, f);
    fclose(f);
    printf("  Read %u bytes\n", (unsigned)file_size);

    wav_info_t wav;
    int rc = wav_parse(wav_buf, file_size, &wav);
    if (rc < 0) { printf("  WAV parse error: %d\n", rc); park_msg("bad WAV"); }
    if (wav.format != 1) { printf("  fmt=%d\n", wav.format); park_msg("not PCM"); }
    if (wav.block_align == 0) park_msg("bad block_align");

    total_samples = wav.data_size / wav.block_align;
    sample_rate   = wav.sample_rate;
    if (total_samples == 0 || sample_rate == 0) park_msg("empty WAV");

    printf("  %d-bit %s %uHz\n",
           wav.bits_per_sample, wav.channels == 1 ? "mono" : "stereo",
           (unsigned)sample_rate);
    printf("  %u samples (%.1fs)\n\n",
           (unsigned)total_samples, (float)total_samples / sample_rate);

    /* Convert to 16-bit signed mono in the mixer sample pool, which the HW
     * mixer's voice-fetch master reads and the OS keeps coherent. */
    sample_buf = (int16_t *)of_mixer_alloc_samples(total_samples * sizeof(int16_t));
    if (!sample_buf) park_msg("sample pool alloc failed");
    for (uint32_t i = 0; i < total_samples; i++)
        sample_buf[i] = fold_sample(&wav, i);

    printf("  Press START to play\n");
    set_status("READY");

    /* 30 Hz progress redraw, driven by the timer ISR's dirty flag. */
    of_timer_set_callback(timer_tick, 30);

    for (;;) {
        of_input_poll();

        if (of_btn_pressed(OF_BTN_START)) {
            switch (state) {
            case ST_PLAYING:                       /* pause: remember + stop */
                pause_pos = (int)play_position();
                of_mixer_stop_h(voice);
                state = ST_PAUSED;
                set_status("PAUSED");
                break;
            case ST_PAUSED:                        /* resume from pause_pos */
                voice = start_play(pause_pos);
                state = ST_PLAYING;
                set_status("PLAYING");
                break;
            case ST_DONE:                          /* play again from 0 */
                voice = start_play(0);
                pause_pos = 0;
                state = ST_PLAYING;
                set_status("PLAYING");
                break;
            }
        }

        if (of_btn_pressed(OF_BTN_SELECT)) {       /* restart */
            of_mixer_stop_h(voice);
            voice = start_play(0);
            pause_pos = 0;
            state = ST_PLAYING;
            set_status("PLAYING");
        }

        if (progress_dirty) {
            progress_dirty = 0;
            draw_progress();
        }

        usleep(16 * 1000);
    }
    return 0;
}
