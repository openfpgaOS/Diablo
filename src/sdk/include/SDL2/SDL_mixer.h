/*
 * SDL_mixer shim for openfpgaOS
 *
 * Minimal SDL_mixer implementation wrapping of_mixer for SFX.
 * Music (OGG/MOD) is stubbed — not supported on FPGA.
 * On PC builds, this header is never used — the real SDL_mixer is linked.
 */

#ifndef _OF_SDL_MIXER_SHIM_H
#define _OF_SDL_MIXER_SHIM_H

#ifdef OF_PC
#include_next <SDL2/SDL_mixer.h>
#else

#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ======================================================================
 * Constants
 * ====================================================================== */

#define MIX_INIT_OGG     0x00000002
#define MIX_INIT_MP3     0x00000008
#define MIX_MAX_VOLUME   128
#define MIX_CHANNELS     32

#define SDL_MIXER_MAJOR_VERSION 2
#define SDL_MIXER_MINOR_VERSION 6
#define SDL_MIXER_PATCHLEVEL    0
#define SDL_MIXER_VERSION_ATLEAST(X, Y, Z) \
    (SDL_VERSIONNUM(SDL_MIXER_MAJOR_VERSION, SDL_MIXER_MINOR_VERSION, SDL_MIXER_PATCHLEVEL) >= SDL_VERSIONNUM((X), (Y), (Z)))

/* ======================================================================
 * Types
 * ====================================================================== */

typedef struct Mix_Chunk {
    int       allocated;
    Uint8    *abuf;
    Uint32    alen;
    int       volume;
    int16_t  *pcm_s16;
    uint32_t  sample_count;
    uint32_t  sample_rate;
} Mix_Chunk;

typedef struct Mix_Music {
    Uint8 *data;
    Uint32 len;
    int loop;
    int playing;
    int paused;
} Mix_Music;

/* ======================================================================
 * Internal state
 * ====================================================================== */

static int __mix_initialized;
static int __mix_max_channels = 8;
static of_mixer_handle_t __mix_voice_ids[32];
static int __mix_channel_group[32];
static int __mix_channel_volume[32];
static int __mix_freq = OF_AUDIO_RATE;
static Uint16 __mix_format = AUDIO_S16SYS;
static int __mix_channels = 2;
static Mix_Music *__mix_current_music;
static void (*__mix_music_finished)(void);
static void (*__mix_channel_finished)(int);
static void (*__mix_postmix)(void *, Uint8 *, int);
static void *__mix_postmix_udata;

/* ======================================================================
 * Init / Open / Close
 * ====================================================================== */

static inline int Mix_Init(int flags) { (void)flags; return flags; }
static inline void Mix_Quit(void) { __mix_initialized = 0; }

static inline int Mix_OpenAudio(int freq, uint16_t fmt, int ch, int sz) {
    (void)sz;
    __mix_freq = freq > 0 ? freq : OF_AUDIO_RATE;
    __mix_format = fmt ? fmt : AUDIO_S16SYS;
    __mix_channels = ch > 0 ? ch : 2;
    if (!__mix_initialized) {
        of_audio_init();
        of_mixer_init(MIX_CHANNELS, OF_AUDIO_RATE);
        for (int i = 0; i < MIX_CHANNELS; i++) {
            __mix_voice_ids[i] = OF_MIXER_HANDLE_INVALID;
            __mix_channel_group[i] = -1;
            __mix_channel_volume[i] = MIX_MAX_VOLUME;
        }
        __mix_max_channels = MIX_CHANNELS;
        __mix_initialized = 1;
    }
    return 0;
}

static inline void Mix_CloseAudio(void) {
    if (__mix_initialized) of_mixer_stop_all();
    __mix_initialized = 0;
}

static inline const char *Mix_GetError(void) { return ""; }
static inline int Mix_QuerySpec(int *freq, Uint16 *fmt, int *ch) {
    if (freq) *freq = __mix_freq;
    if (fmt) *fmt = __mix_format;
    if (ch) *ch = __mix_channels;
    return __mix_initialized ? 1 : 0;
}

static inline int16_t __mix_read_s16le(const uint8_t *p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* ======================================================================
 * WAV loading
 * ====================================================================== */

static inline Mix_Chunk *__mix_chunk_from_audio(const SDL_AudioSpec *spec,
                                                const Uint8 *audio,
                                                Uint32 audio_len) {
    if (!spec || !audio || audio_len == 0) return NULL;
    int channels = spec->channels > 0 ? spec->channels : 1;
    int bytes = (spec->format == AUDIO_U8) ? 1 : 2;
    uint32_t num_samples = audio_len / (uint32_t)(bytes * channels);
    if (num_samples == 0) return NULL;
    Mix_Chunk *chunk = (Mix_Chunk *)calloc(1, sizeof(Mix_Chunk));
    if (!chunk) return NULL;
    int16_t *pcm_s16 = (int16_t *)malloc(num_samples * sizeof(int16_t));
    if (!pcm_s16) { free(chunk); return NULL; }
    if (spec->format == AUDIO_U8) {
        int step = channels;
        for (uint32_t i = 0; i < num_samples; i++)
            pcm_s16[i] = (int16_t)(((int)audio[i * step] - 128) << 8);
    } else {
        int step = channels * 2;
        for (uint32_t i = 0; i < num_samples; i++)
            pcm_s16[i] = __mix_read_s16le(audio + (i * step));
    }
    chunk->allocated = 1;
    chunk->abuf = (Uint8 *)pcm_s16;
    chunk->alen = (Uint32)(num_samples * sizeof(int16_t));
    chunk->volume = MIX_MAX_VOLUME;
    chunk->pcm_s16 = pcm_s16;
    chunk->sample_count = num_samples;
    chunk->sample_rate = (uint32_t)(spec->freq > 0 ? spec->freq : OF_AUDIO_RATE);
    return chunk;
}

static inline Mix_Chunk *Mix_LoadWAV_RW(SDL_RWops *src, int freesrc) {
    SDL_AudioSpec spec;
    Uint8 *audio = NULL;
    Uint32 audio_len = 0;
    if (!SDL_LoadWAV_RW(src, freesrc, &spec, &audio, &audio_len)) return NULL;
    Mix_Chunk *chunk = __mix_chunk_from_audio(&spec, audio, audio_len);
    SDL_FreeWAV(audio);
    return chunk;
}

static inline Mix_Chunk *Mix_LoadWAV(const char *file) {
    SDL_AudioSpec spec;
    Uint8 *audio = NULL;
    Uint32 audio_len = 0;
    if (!SDL_LoadWAV(file, &spec, &audio, &audio_len)) return NULL;
    Mix_Chunk *chunk = __mix_chunk_from_audio(&spec, audio, audio_len);
    SDL_FreeWAV(audio);
    return chunk;
}

static inline void Mix_FreeChunk(Mix_Chunk *chunk) {
    if (!chunk) return;
    if (chunk->allocated && chunk->abuf)
        free(chunk->abuf);
    free(chunk);
}

/* ======================================================================
 * Channel playback (SFX)
 * ====================================================================== */

static inline int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops) {
    if (!chunk || !chunk->pcm_s16) return -1;
    (void)loops;

    if (!__mix_initialized) {
        if (Mix_OpenAudio(__mix_freq, __mix_format, __mix_channels, 1024) < 0)
            return -1;
    }

    if (channel < 0) {
        for (int i = 0; i < __mix_max_channels; i++) {
            if (__mix_voice_ids[i] == OF_MIXER_HANDLE_INVALID ||
                !of_mixer_handle_active(__mix_voice_ids[i])) {
                channel = i; break;
            }
        }
        if (channel < 0) channel = 0;
    }

    int mixvol = (channel < MIX_CHANNELS) ? __mix_channel_volume[channel] : MIX_MAX_VOLUME;
    int vol = ((chunk->volume * mixvol) / MIX_MAX_VOLUME) * 255 / 128;
    of_mixer_handle_t voice = of_mixer_play_h((const uint8_t *)chunk->pcm_s16,
                                              chunk->sample_count,
                                              chunk->sample_rate, 0, vol);
    if (voice == OF_MIXER_HANDLE_INVALID) return -1;

    if (channel < 32) __mix_voice_ids[channel] = voice;
    return channel;
}

static inline void Mix_HaltChannel(int channel) {
    if (!__mix_initialized) return;
    if (channel < 0) { of_mixer_stop_all(); return; }
    if (channel < 32 && __mix_voice_ids[channel] != OF_MIXER_HANDLE_INVALID)
        of_mixer_stop_h(__mix_voice_ids[channel]);
}

static inline void Mix_Pause(int ch)  { (void)ch; }
static inline void Mix_Resume(int ch) { (void)ch; }
static inline int Mix_ReserveChannels(int num) { (void)num; return 0; }
static inline int Mix_GroupChannels(int from, int to, int tag) {
    if (from < 0) from = 0;
    if (to >= MIX_CHANNELS) to = MIX_CHANNELS - 1;
    for (int i = from; i <= to; i++) __mix_channel_group[i] = tag;
    return (to >= from) ? (to - from + 1) : 0;
}
static inline int Mix_GroupAvailable(int tag) {
    for (int i = 0; i < MIX_CHANNELS; i++) {
        if (__mix_channel_group[i] == tag &&
            (__mix_voice_ids[i] == OF_MIXER_HANDLE_INVALID ||
             !of_mixer_handle_active(__mix_voice_ids[i]))) {
            return i;
        }
    }
    return -1;
}
static inline int Mix_GroupOldest(int tag) {
    for (int i = 0; i < MIX_CHANNELS; i++)
        if (__mix_channel_group[i] == tag) return i;
    return -1;
}
static inline int Mix_Volume(int channel, int volume) {
    if (channel < 0) {
        for (int i = 0; i < MIX_CHANNELS; i++) Mix_Volume(i, volume);
        return volume;
    }
    if (channel >= MIX_CHANNELS) return -1;
    int old = __mix_channel_volume[channel];
    if (volume >= 0) {
        if (volume > MIX_MAX_VOLUME) volume = MIX_MAX_VOLUME;
        __mix_channel_volume[channel] = volume;
        if (__mix_voice_ids[channel] != OF_MIXER_HANDLE_INVALID)
            of_mixer_set_volume_h(__mix_voice_ids[channel], (volume * 255) / MIX_MAX_VOLUME);
    }
    return old;
}
static inline int Mix_SetPanning(int channel, Uint8 left, Uint8 right) {
    if (channel < 0 || channel >= MIX_CHANNELS) return 0;
    if (__mix_voice_ids[channel] != OF_MIXER_HANDLE_INVALID)
        of_mixer_set_vol_lr_h(__mix_voice_ids[channel], left, right);
    return 1;
}
static inline void Mix_ChannelFinished(void (*cb)(int channel)) {
    __mix_channel_finished = cb;
    (void)__mix_channel_finished;
}
static inline void Mix_SetPostMix(void (*mix_func)(void *, Uint8 *, int), void *arg) {
    __mix_postmix = mix_func;
    __mix_postmix_udata = arg;
    (void)__mix_postmix;
    (void)__mix_postmix_udata;
}

/* ======================================================================
 * Music (stubs)
 * ====================================================================== */

static inline Mix_Music *Mix_LoadMUS_RW(SDL_RWops *src, int freesrc) {
    if (!src) return NULL;
    Sint64 size64 = src->size ? src->size(src) : -1;
    if (size64 <= 0 || size64 > 1024 * 1024) {
        if (freesrc && src->close) src->close(src);
        return NULL;
    }
    Uint8 *data = (Uint8 *)malloc((size_t)size64);
    if (!data) {
        if (freesrc && src->close) src->close(src);
        return NULL;
    }
    if (src->seek) src->seek(src, 0, RW_SEEK_SET);
    size_t got = src->read ? src->read(src, data, 1, (size_t)size64) : 0;
    if (freesrc && src->close) src->close(src);
    if (got != (size_t)size64 || size64 < 14 || memcmp(data, "MThd", 4) != 0) {
        free(data);
        return NULL;
    }
    Mix_Music *m = (Mix_Music *)calloc(1, sizeof(Mix_Music));
    if (!m) { free(data); return NULL; }
    m->data = data;
    m->len = (Uint32)size64;
    return m;
}
static inline Mix_Music *Mix_LoadMUS(const char *f) {
    FILE *fp = fopen(f, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    if (size <= 0 || size > 1024 * 1024) { fclose(fp); return NULL; }
    fseek(fp, 0, SEEK_SET);
    Uint8 *data = (Uint8 *)malloc((size_t)size);
    if (!data) { fclose(fp); return NULL; }
    fread(data, 1, (size_t)size, fp);
    fclose(fp);
    SDL_RWops *rw = SDL_RWFromMem(data, (int)size);
    Mix_Music *m = Mix_LoadMUS_RW(rw, 1);
    free(data);
    return m;
}
static inline void Mix_FreeMusic(Mix_Music *m) {
    if (!m) return;
    if (__mix_current_music == m) __mix_current_music = NULL;
    free(m->data);
    free(m);
}
static inline int Mix_PlayMusic(Mix_Music *m, int l) {
    if (!m || !m->data || !m->len) return -1;
    if (of_midi_init() < 0) return -1;
    if (of_midi_play(m->data, m->len, l != 0) < 0) return -1;
    m->loop = l;
    m->playing = 1;
    m->paused = 0;
    __mix_current_music = m;
    return 0;
}
static inline int Mix_FadeInMusic(Mix_Music *m, int l, int ms) { (void)m;(void)l;(void)ms; return -1; }
static inline int Mix_FadeOutMusic(int ms)           { (void)ms; return 0; }
static inline void Mix_HaltMusic(void) {
    of_midi_stop();
    if (__mix_current_music) __mix_current_music->playing = 0;
    __mix_current_music = NULL;
}
static inline void Mix_PauseMusic(void) {
    of_midi_pause();
    if (__mix_current_music) __mix_current_music->paused = 1;
}
static inline void Mix_ResumeMusic(void) {
    of_midi_resume();
    if (__mix_current_music) __mix_current_music->paused = 0;
}
static inline int Mix_PlayingMusic(void) { return of_midi_playing(); }
static inline int Mix_PausedMusic(void) { return of_midi_paused(); }
static inline double Mix_GetMusicPosition(Mix_Music *music) { (void)music; return 0.0; }
static inline int Mix_SetMusicPosition(double pos) { (void)pos; return 0; }
static inline int Mix_VolumeMusic(int volume) {
    if (volume >= 0) {
        if (volume > MIX_MAX_VOLUME) volume = MIX_MAX_VOLUME;
        of_midi_set_volume((volume * 255) / MIX_MAX_VOLUME);
    }
    return volume;
}
static inline void Mix_HookMusicFinished(void (*cb)(void)) {
    __mix_music_finished = cb;
    (void)__mix_music_finished;
}
static inline int Mix_SetSoundFonts(const char *paths) { (void)paths; return 1; }

#endif /* OF_PC */
#endif /* _OF_SDL_MIXER_SHIM_H */
