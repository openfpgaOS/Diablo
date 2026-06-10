//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * test_audio.c — Mixer and audio tests
 *
 * Test codes:
 *   MX.xx = Hardware mixer tests
 *   AU.xx = Audio FIFO / streaming tests
 */

#include "test.h"
#include <time.h>

void test_mixer(void) {
    section_start("Mixer");

    of_mixer_init(4, 48000);
    of_mixer_stop_all();
    of_mixer_poll_ended();
    test_pass("MX.01 init");

    #define MIX_TONE_LEN 551
    static int16_t pcm_tone_src[MIX_TONE_LEN];
    for (int i = 0; i < MIX_TONE_LEN; i++) {
        int phase = (i * 440 / 11025) & 1;
        pcm_tone_src[i] = phase ? 8000 : -8000;
    }

    void *sample_buf = of_mixer_alloc_samples(MIX_TONE_LEN * sizeof(int16_t));
    ASSERT("MX.02 alloc", sample_buf != NULL);
    if (sample_buf) {
        memcpy(sample_buf, pcm_tone_src, MIX_TONE_LEN * sizeof(int16_t));
    }

    int voice = of_mixer_play((const uint8_t *)sample_buf, MIX_TONE_LEN, 11025, 0, 128);
    ASSERT("MX.03 play", voice >= 0);

    for (int i = 0; i < 10; i++) {
        of_mixer_pump();
        usleep(15 * 1000);
    }

    uint32_t ended = of_mixer_poll_ended();
    ASSERT("MX.04 done", voice >= 0 && (ended & (1u << voice)));

    int v2 = of_mixer_play((const uint8_t *)sample_buf, MIX_TONE_LEN, 11025, 0, 200);
    ASSERT("MX.05 replay", v2 >= 0);
    if (v2 >= 0) {
        of_mixer_set_vol_lr(v2, 255, 0);
        usleep(30 * 1000);
        of_mixer_set_vol_lr(v2, 0, 255);
        usleep(30 * 1000);
        of_mixer_set_vol_lr(v2, 128, 128);
        test_pass("MX.06 stereo");
    }

    int v3 = of_mixer_play((const uint8_t *)sample_buf, MIX_TONE_LEN, 11025, 0, 100);
    if (v3 >= 0) {
        of_mixer_set_loop(v3, 0, MIX_TONE_LEN);
        usleep(100 * 1000);
        ASSERT("MX.07 loop on", of_mixer_voice_active(v3));
        of_mixer_set_loop(v3, -1, 0);
        usleep(100 * 1000);
        of_mixer_poll_ended();
        ASSERT("MX.08 loop off", !of_mixer_voice_active(v3));
    }

    int v4 = of_mixer_play((const uint8_t *)sample_buf, MIX_TONE_LEN, 11025, 0, 100);
    if (v4 >= 0) {
        of_mixer_set_loop(v4, 0, MIX_TONE_LEN);
        usleep(20 * 1000);
        int pos = of_mixer_get_position(v4);
        /* After 20ms at 11025Hz playing into a 551-sample loop, pos
         * should be ~220 samples (well within [0, MIX_TONE_LEN)). */
        if (pos > 0 && pos < MIX_TONE_LEN) {
            test_pass("MX.09 pos rd");
        } else {
            snprintf(__buf, sizeof(__buf), "v=%d pos=%d", v4, pos);
            test_fail("MX.09 pos rd", __buf);
        }

        const int seek_pos = 8;
        of_mixer_set_position(v4, seek_pos);
        uint32_t seek_start = of_time_us();
        int pos2 = -1;
        do {
            pos2 = of_mixer_get_position(v4);
            if (pos2 >= 0 && pos2 < MIX_TONE_LEN / 4)
                break;
        } while ((uint32_t)(of_time_us() - seek_start) < 2000u);
        /* Current firmware uses 0 as a no-pending-seek sentinel; seek to
         * a small nonzero offset to verify the writable position path. */
        if (pos2 >= 0 && pos2 < MIX_TONE_LEN / 4) {
            test_pass("MX.10 pos wr");
        } else {
            snprintf(__buf, sizeof(__buf), "v=%d pos=%d", v4, pos2);
            test_fail("MX.10 pos wr", __buf);
        }
    }

    of_mixer_stop_all();
    test_pass("MX.11 stop");

    of_mixer_free_samples();
    test_pass("MX.12 free");

    section_end();
}

void test_mixer_adv(void) {
    section_start("Mixer Adv");

    of_mixer_init(32, 48000);
    of_mixer_stop_all();
    of_mixer_poll_ended();
    of_mixer_free_samples();

    /* Generate tone in the SDRAM mixer sample pool */
    #define ADV_TONE_LEN 551
    static int16_t adv_tone_src[ADV_TONE_LEN];
    for (int i = 0; i < ADV_TONE_LEN; i++)
        adv_tone_src[i] = (i & 1) ? 8000 : -8000;

    int16_t *s16_buf = (int16_t *)of_mixer_alloc_samples(ADV_TONE_LEN * 2);
    if (!s16_buf) { test_fail("MA.00 alloc", "null"); section_end(); return; }
    memcpy(s16_buf, adv_tone_src, ADV_TONE_LEN * 2);

    /* MA.01: 8-bit playback — allocate 8-bit signed, play, verify voice starts */
    {
        int8_t *s8_buf = (int8_t *)of_mixer_alloc_samples(ADV_TONE_LEN);
        ASSERT("MA.01a alloc8", s8_buf != NULL);
        if (s8_buf) {
            for (int i = 0; i < ADV_TONE_LEN; i++)
                s8_buf[i] = (i & 1) ? 31 : -31;
            int v = of_mixer_play_8bit((const uint8_t *)s8_buf, ADV_TONE_LEN, 11025, 0, 200);
            ASSERT("MA.01b play8", v >= 0);
            if (v >= 0) {
                usleep(10 * 1000);
                ASSERT("MA.01c actv8", of_mixer_voice_active(v));
                of_mixer_stop(v);
            }
        }
    }

    /* MA.02: priority-based voice stealing.
     * Fill all voices with looping low-priority sounds, then test stealing. */
    {
        of_mixer_stop_all();
        of_mixer_poll_ended();
        int filled = 0;
        for (int i = 0; i < OF_MIXER_MAX_VOICES; i++) {
            int v = of_mixer_play((const uint8_t *)s16_buf, ADV_TONE_LEN, 11025, 0, 50);
            if (v >= 0) {
                of_mixer_set_loop(v, 0, ADV_TONE_LEN);  /* keep alive */
                filled++;
            }
        }
        ASSERT("MA.02a fill", filled >= OF_MIXER_MAX_VOICES - 4);

        /* All voices busy — same priority play should fail (can't steal equal) */
        int vlo = of_mixer_play((const uint8_t *)s16_buf, ADV_TONE_LEN, 11025, 0, 50);
        ASSERT("MA.02b nofree", vlo < 0);

        /* Higher priority should steal a low-priority voice */
        int vhi = of_mixer_play((const uint8_t *)s16_buf, ADV_TONE_LEN, 11025, 10, 100);
        ASSERT("MA.02c steal", vhi >= 0);

        of_mixer_stop_all();
        of_mixer_poll_ended();
    }

    /* MA.03: set_pan at center preserves volume.
     * Play at vol=200, set pan to center (128), check voice is still active.
     * (The old bug made center pan = half volume.) */
    {
        int v = of_mixer_play((const uint8_t *)s16_buf, ADV_TONE_LEN, 11025, 0, 200);
        ASSERT("MA.03a play", v >= 0);
        if (v >= 0) {
            of_mixer_set_pan(v, 128);  /* center */
            usleep(10 * 1000);
            ASSERT("MA.03b pan", of_mixer_voice_active(v));
            of_mixer_stop(v);
        }
    }

    /* MA.06: retrigger — play, retrigger mid-playback, verify still active */
    {
        int v = of_mixer_play((const uint8_t *)s16_buf, ADV_TONE_LEN, 11025, 0, 150);
        ASSERT("MA.06a play", v >= 0);
        if (v >= 0) {
            usleep(20 * 1000);
            ASSERT("MA.06b pre", of_mixer_voice_active(v));
            of_mixer_retrigger(v, (const uint8_t *)s16_buf, ADV_TONE_LEN, 11025, 150);
            /* Sleep just long enough for the mixer to process the
             * retrigger and start advancing. ADV_TONE_LEN=551 at
             * 11025 Hz = 50 ms total playback, so the half-mark is
             * ~25 ms after retrigger — the old 10 ms sleep + the
             * inactive/get_position call overhead pushed pos right
             * to that boundary and randomly tipped over. 2 ms gives
             * ~22 samples played, well under ADV_TONE_LEN/2 = 275. */
            usleep(2 * 1000);
            ASSERT("MA.06c post", of_mixer_voice_active(v));
            /* After retrigger, position should be near start */
            int pos = of_mixer_get_position(v);
            ASSERT("MA.06d pos", pos < ADV_TONE_LEN / 2);
            of_mixer_stop(v);
        }
    }

    /* MA.07: bidi compatibility surface -- current firmware ignores bidi,
     * but the call must remain harmless while a forward loop stays active. */
    {
        int v = of_mixer_play((const uint8_t *)s16_buf, ADV_TONE_LEN, 11025, 0, 100);
        if (v >= 0) {
            of_mixer_set_loop(v, 0, ADV_TONE_LEN);
            of_mixer_set_bidi(v, 1);
            usleep(100 * 1000);  /* longer than one pass */
            ASSERT("MA.07 bidi", of_mixer_voice_active(v));
            of_mixer_stop(v);
        }
    }

    /* MA.08: volume ramp — set vol_rate, change volume, verify voice active */
    {
        int v = of_mixer_play((const uint8_t *)s16_buf, ADV_TONE_LEN, 11025, 0, 255);
        if (v >= 0) {
            of_mixer_set_loop(v, 0, ADV_TONE_LEN);
            of_mixer_set_volume_ramp(v, 4);  /* slow ramp */
            of_mixer_set_volume(v, 0);    /* ramp to 0 */
            usleep(10 * 1000);
            ASSERT("MA.08 ramp", of_mixer_voice_active(v));  /* still active during ramp */
            of_mixer_stop(v);
        }
    }

    /* MA.09: voice end callback via poll_ended bitmask */
    {
        of_mixer_stop_all();
        of_mixer_poll_ended();  /* clear stale bits */
        int v = of_mixer_play((const uint8_t *)s16_buf, ADV_TONE_LEN, 11025, 0, 100);
        ASSERT("MA.09a play", v >= 0);
        if (v >= 0) {
            /* Wait for voice to finish (~50ms) */
            for (int i = 0; i < 15; i++) usleep(10 * 1000);
            uint32_t ended = of_mixer_poll_ended();
            ASSERT("MA.09b ended", ended & (1u << v));
            ASSERT("MA.09c dead", !of_mixer_voice_active(v));
        }
    }

    /* MA.10: playback rate verification.
     * Play at 48000 Hz (1:1 with output). After 10ms, position should be
     * ~480 samples. Allow ±20% tolerance for timer + scheduling jitter. */
    {
        of_mixer_stop_all();
        of_mixer_poll_ended();
        /* Need a longer sample for rate test */
        #define RATE_TONE_LEN 4800  /* 100ms at 48kHz */
        int16_t *rate_buf = (int16_t *)of_mixer_alloc_samples(RATE_TONE_LEN * 2);
        if (rate_buf) {
            for (int i = 0; i < RATE_TONE_LEN; i++)
                rate_buf[i] = (i & 1) ? 4000 : -4000;

            int v = of_mixer_play((const uint8_t *)rate_buf, RATE_TONE_LEN, 48000, 0, 100);
            ASSERT("MA.10a play", v >= 0);
            if (v >= 0) {
                of_mixer_set_loop(v, 0, RATE_TONE_LEN);
                usleep(10 * 1000);  /* 10ms */
                int pos = of_mixer_get_position(v);
                /* At 48 kHz × 10 ms = 480 source samples.  Allow
                 * 384-576 (±20%) for timer + scheduler jitter. */
                if (pos >= 384 && pos <= 576) {
                    test_pass("MA.10b rate");
                } else {
                    snprintf(__buf, sizeof(__buf), "v=%d pos=%d (want 384..576)", v, pos);
                    test_fail("MA.10b rate", __buf);
                }
                of_mixer_stop(v);
            }
        }
    }

    /* MA.11: playback rate at half speed.
     * Play at 24000 Hz. After 10ms, position should be ~240 samples. */
    {
        of_mixer_stop_all();
        of_mixer_poll_ended();
        int16_t *rate_buf = (int16_t *)of_mixer_alloc_samples(RATE_TONE_LEN * 2);
        if (rate_buf) {
            for (int i = 0; i < RATE_TONE_LEN; i++)
                rate_buf[i] = (i & 1) ? 4000 : -4000;

            int v = of_mixer_play((const uint8_t *)rate_buf, RATE_TONE_LEN, 24000, 0, 100);
            if (v >= 0) {
                of_mixer_set_loop(v, 0, RATE_TONE_LEN);
                usleep(10 * 1000);
                int pos = of_mixer_get_position(v);
                /* At 24 kHz × 10 ms = 240 source samples.  Allow
                 * 192-288 (±20%).  If this returns ~480 the rate
                 * field is being ignored and the voice is playing
                 * at the output rate (= 48 kHz). */
                if (pos >= 192 && pos <= 288) {
                    test_pass("MA.11 half");
                } else {
                    snprintf(__buf, sizeof(__buf), "v=%d pos=%d (want 192..288)", v, pos);
                    test_fail("MA.11 half", __buf);
                }
                of_mixer_stop(v);
            }
        }
    }

    /* MA.12: multi-voice independence.
     * Play two voices at different rates, measure position DELTA over
     * a fixed interval. Delta is immune to loop wrap and start jitter.
     * v1 at 48kHz should advance ~4.35x faster than v2 at 11025Hz. */
    {
        of_mixer_stop_all();
        of_mixer_poll_ended();
        int16_t *buf1 = (int16_t *)of_mixer_alloc_samples(RATE_TONE_LEN * 2);
        int16_t *buf2 = (int16_t *)of_mixer_alloc_samples(RATE_TONE_LEN * 2);
        if (buf1 && buf2) {
            for (int i = 0; i < RATE_TONE_LEN; i++) {
                buf1[i] = (int16_t)(i & 0xFF);
                buf2[i] = (int16_t)(~i & 0xFF);
            }

            int v1 = of_mixer_play((const uint8_t *)buf1, RATE_TONE_LEN, 48000, 0, 100);
            int v2 = of_mixer_play((const uint8_t *)buf2, RATE_TONE_LEN, 11025, 0, 100);
            ASSERT("MA.12a v1", v1 >= 0);
            ASSERT("MA.12b v2", v2 >= 0);
            if (v1 >= 0 && v2 >= 0) {
                of_mixer_set_loop(v1, 0, RATE_TONE_LEN);
                of_mixer_set_loop(v2, 0, RATE_TONE_LEN);
                /* Let them run, then sample position twice */
                usleep(10 * 1000);
                int p1a = of_mixer_get_position(v1);
                int p2a = of_mixer_get_position(v2);
                usleep(10 * 1000);
                int p1b = of_mixer_get_position(v1);
                int p2b = of_mixer_get_position(v2);
                /* Compute deltas (handle loop wrap) */
                int d1 = p1b - p1a;
                if (d1 < 0) d1 += RATE_TONE_LEN;
                int d2 = p2b - p2a;
                if (d2 < 0) d2 += RATE_TONE_LEN;
                /* d1 should be ~2-5x larger than d2 (48000/11025 ≈ 4.35) */
                if (d1 > d2 * 2 && d2 > 0) {
                    test_pass("MA.12c rate");
                } else {
                    snprintf(__buf, sizeof(__buf),
                             "v%d/%d d %d/%d", v1, v2, d1, d2);
                    test_fail("MA.12c rate", __buf);
                }
                of_mixer_stop(v1);
                of_mixer_stop(v2);
            }
        }
    }

    /* MA.13: voice end timing — play at 48kHz, 2400 samples = 50ms.
     * Should end between 40-70ms (allow jitter). */
    {
        of_mixer_stop_all();
        of_mixer_poll_ended();
        #define END_TONE_LEN 2400
        int16_t *end_buf = (int16_t *)of_mixer_alloc_samples(END_TONE_LEN * 2);
        if (end_buf) {
            for (int i = 0; i < END_TONE_LEN; i++)
                end_buf[i] = (i & 1) ? 2000 : -2000;

            int v = of_mixer_play((const uint8_t *)end_buf, END_TONE_LEN, 48000, 0, 100);
            if (v >= 0) {
                /* Should still be active at 30ms */
                usleep(30 * 1000);
                ASSERT("MA.13a @30ms", of_mixer_voice_active(v));
                /* Should be done by 70ms */
                usleep(40 * 1000);
                of_mixer_poll_ended();
                ASSERT("MA.13b @70ms", !of_mixer_voice_active(v));
            }
        }
    }

    of_mixer_stop_all();
    of_mixer_free_samples();
    /* Restore group/master to defaults for subsequent tests */
    of_mixer_set_group_volume(OF_MIXER_GROUP_SFX, 255);
    of_mixer_set_group_volume(OF_MIXER_GROUP_MUSIC, 255);
    of_mixer_set_master_volume(255);

    section_end();
}

/* ================================================================
 * Stress test: many PCM voices simultaneously
 *
 * This simulates a worst-case game-audio scenario: many SFX playing
 * while music owns other mixer voices. Tests mixer FSM timing budget,
 * SDRAM sample bandwidth, and audio FIFO underrun.
 * ================================================================ */
void test_mixer_stress(void) {
    section_start("Mixer Strss");

    of_mixer_init(32, 48000);
    of_mixer_stop_all();
    of_mixer_poll_ended();
    of_mixer_free_samples();

    /* Allocate 31 different sample buffers with unique patterns. */
    #define STRESS_LEN 2400
    int16_t *bufs[31];
    int alloc_ok = 1;
    for (int i = 0; i < 31; i++) {
        bufs[i] = (int16_t *)of_mixer_alloc_samples(STRESS_LEN * 2);
        if (!bufs[i]) { alloc_ok = 0; break; }
        int half_period = 10 + i * 3;
        for (int j = 0; j < STRESS_LEN; j++)
            bufs[i][j] = ((j / half_period) & 1) ? 2000 : -2000;
    }
    ASSERT("ST.01 alloc31", alloc_ok);

    /* ST.02: Start all 31 voices simultaneously at different rates */
    int voices[31];
    int started = 0;
    for (int i = 0; i < 31 && bufs[i]; i++) {
        int rate = 11025 + i * 1200;  /* 11025 to 47025 Hz */
        voices[i] = of_mixer_play((const uint8_t *)bufs[i], STRESS_LEN,
                                  rate, i, 80);
        if (voices[i] >= 0) {
            of_mixer_set_loop(voices[i], 0, STRESS_LEN);
            /* Spread across stereo field */
            of_mixer_set_pan(voices[i], (i * 255) / 30);
            started++;
        }
    }
    ASSERT("ST.02 start31", started >= 29);

    /* ST.03: Let all 31 voices run for 200ms.
     * Check every 50ms that voices are still active (no underrun/crash). */
    {
        int alive_50 = 0, alive_100 = 0, alive_150 = 0, alive_200 = 0;
        usleep(50 * 1000);
        for (int i = 0; i < 31; i++)
            if (voices[i] >= 0 && of_mixer_voice_active(voices[i])) alive_50++;
        usleep(50 * 1000);
        for (int i = 0; i < 31; i++)
            if (voices[i] >= 0 && of_mixer_voice_active(voices[i])) alive_100++;
        usleep(50 * 1000);
        for (int i = 0; i < 31; i++)
            if (voices[i] >= 0 && of_mixer_voice_active(voices[i])) alive_150++;
        usleep(50 * 1000);
        for (int i = 0; i < 31; i++)
            if (voices[i] >= 0 && of_mixer_voice_active(voices[i])) alive_200++;

        /* All looping voices should remain active throughout */
        ASSERT("ST.03a @50ms", alive_50 >= started - 1);
        ASSERT("ST.03b @100ms", alive_100 >= started - 1);
        ASSERT("ST.03c @150ms", alive_150 >= started - 1);
        ASSERT("ST.03d @200ms", alive_200 >= started - 1);
    }

    /* ST.04: verify positions are advancing (not stuck) */
    {
        int advancing = 0;
        for (int i = 0; i < 31; i++) {
            if (voices[i] < 0) continue;
            int p1 = of_mixer_get_position(voices[i]);
            usleep(5 * 1000);
            int p2 = of_mixer_get_position(voices[i]);
            if (p2 != p1) advancing++;
        }
        ASSERT("ST.04 advance", advancing >= started - 2);
    }

    /* ST.05: stop all voices, verify clean shutdown */
    of_mixer_stop_all();
    usleep(10 * 1000);
    of_mixer_poll_ended();
    {
        int alive = 0;
        for (int i = 0; i < 31; i++)
            if (voices[i] >= 0 && of_mixer_voice_active(voices[i])) alive++;
        ASSERT("ST.05 stopped", alive == 0);
    }

    of_mixer_free_samples();

    section_end();
}

void test_audio_stream(void) {
    section_start("Audio Strm");

    /* AS.01: stream open */
    int rc = of_audio_stream_open(48000);
    ASSERT("AS.01 open", rc == 0);

    /* AS.02: stream ready (should be ready immediately — no data written yet) */
    ASSERT("AS.02 ready", of_audio_stream_ready());

    /* AS.03: stream write */
    {
        #define STRM_FRAMES 4096
        static int16_t strm_buf[STRM_FRAMES];
        for (int i = 0; i < STRM_FRAMES; i++) {
            int phase = ((uint32_t)i * 440 * 65536u / 48000) & 0xFFFF;
            strm_buf[i] = (int16_t)((phase < 32768 ? phase : 65536 - phase) - 16384);
        }
        int written = of_audio_stream_write(strm_buf, STRM_FRAMES);
        ASSERT("AS.03 write", written > 0);
    }

    /* AS.04: second write (ping-pong) */
    {
        static int16_t strm_buf2[STRM_FRAMES];
        for (int i = 0; i < STRM_FRAMES; i++)
            strm_buf2[i] = (int16_t)(i & 1 ? 4000 : -4000);
        int written = of_audio_stream_write(strm_buf2, STRM_FRAMES);
        ASSERT("AS.04 write2", written > 0);
    }

    /* AS.05: close */
    of_audio_stream_close();
    test_pass("AS.05 close");

    /* AS.06: re-open after close */
    rc = of_audio_stream_open(22050);
    ASSERT("AS.06 reopen", rc == 0);
    of_audio_stream_close();

    section_end();
}

void test_audio(void) {
    section_start("Audio");

    of_audio_init();
    test_pass("AU.01 init");

    int free = of_audio_free();
    ASSERT("AU.02 free", free > 0);

    {
        #define TONE_RATE 48000
        #define TONE_HZ 440
        #define TONE2_FRAMES 2400
        static int16_t tone2[TONE2_FRAMES * 2];

        for (int i = 0; i < TONE2_FRAMES; i++) {
            uint32_t phase = (uint32_t)i * TONE_HZ * 65536 / TONE_RATE;
            int32_t tri = (int32_t)(phase & 0xFFFF) - 32768;
            if (tri < 0) tri = -tri;
            tri = tri - 16384;
            int16_t sample = (int16_t)(tri * 2);
            sample >>= 1;
            tone2[i * 2]     = sample;
            tone2[i * 2 + 1] = sample;
        }

        int written = of_audio_write(tone2, TONE2_FRAMES);
        ASSERT("AU.03 write", written > 0);
        (void)written;
    }

    section_end();
}
