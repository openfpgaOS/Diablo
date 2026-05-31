/*
 * SDL_mixer compatibility layer for openfpgaOS  (SDK-wide, reusable).
 *
 * Declares the SDL_mixer 2.x subset the common ports use; the bodies live
 * alongside the SDL2 shim in src/sdk/of_sdl2.c (one TU, shared state).
 * SFX play through the of_mixer 32-voice hardware mixer; music is Standard
 * MIDI played through of_midi (a SoundFont bank is preloaded by the OS).
 *
 * On PC builds (-DOF_PC) this forwards to the real system SDL_mixer.
 */
#ifndef OF_SDL2_SHIM_SDL_MIXER_H
#define OF_SDL2_SHIM_SDL_MIXER_H

#ifdef OF_PC
#include_next <SDL2/SDL_mixer.h>
#else

#include <SDL2/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIX_INIT_FLAC    0x00000001
#define MIX_INIT_MOD     0x00000002
#define MIX_INIT_MP3     0x00000008
#define MIX_INIT_OGG     0x00000010
#define MIX_INIT_MID     0x00000020
#define MIX_MAX_VOLUME   128
#define MIX_CHANNELS     32
#define MIX_DEFAULT_FREQUENCY 48000
#define MIX_DEFAULT_FORMAT    AUDIO_S16SYS
#define MIX_DEFAULT_CHANNELS  2

#define SDL_MIXER_MAJOR_VERSION 2
#define SDL_MIXER_MINOR_VERSION 6
#define SDL_MIXER_PATCHLEVEL    0
#define SDL_MIXER_VERSION_ATLEAST(X, Y, Z) \
	(SDL_VERSIONNUM(SDL_MIXER_MAJOR_VERSION, SDL_MIXER_MINOR_VERSION, SDL_MIXER_PATCHLEVEL) >= SDL_VERSIONNUM((X), (Y), (Z)))

typedef enum { MIX_NO_FADING, MIX_FADING_OUT, MIX_FADING_IN } Mix_Fading;
typedef enum { MUS_NONE, MUS_CMD, MUS_WAV, MUS_MOD, MUS_MID, MUS_OGG, MUS_MP3, MUS_FLAC } Mix_MusicType;

typedef struct Mix_Chunk {
	int       allocated;
	Uint8    *abuf;
	Uint32    alen;
	int       volume;
	int16_t  *pcm_s16;
	Uint32    sample_count;
	Uint32    sample_rate;
} Mix_Chunk;

typedef struct Mix_Music {
	Uint8 *data;
	Uint32 len;
	int loop;
	int playing;
	int paused;
} Mix_Music;

int  Mix_Init(int flags);
void Mix_Quit(void);
const char *Mix_GetError(void);
void Mix_SetError(const char *fmt, ...);
int  Mix_OpenAudio(int frequency, Uint16 format, int channels, int chunksize);
int  Mix_OpenAudioDevice(int frequency, Uint16 format, int channels, int chunksize, const char *device, int allowed_changes);
void Mix_CloseAudio(void);
int  Mix_QuerySpec(int *frequency, Uint16 *format, int *channels);
int  Mix_AllocateChannels(int numchans);

Mix_Chunk *Mix_LoadWAV_RW(SDL_RWops *src, int freesrc);
Mix_Chunk *Mix_LoadWAV(const char *file);
Mix_Chunk *Mix_QuickLoad_RAW(Uint8 *mem, Uint32 len);
void Mix_FreeChunk(Mix_Chunk *chunk);
int  Mix_VolumeChunk(Mix_Chunk *chunk, int volume);

int  Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops);
int  Mix_PlayChannelTimed(int channel, Mix_Chunk *chunk, int loops, int ticks);
void Mix_HaltChannel(int channel);
int  Mix_Playing(int channel);
void Mix_Pause(int channel);
void Mix_Resume(int channel);
int  Mix_ReserveChannels(int num);
int  Mix_GroupChannel(int which, int tag);
int  Mix_GroupChannels(int from, int to, int tag);
int  Mix_GroupAvailable(int tag);
int  Mix_GroupOldest(int tag);
int  Mix_Volume(int channel, int volume);
int  Mix_SetPanning(int channel, Uint8 left, Uint8 right);
void Mix_ChannelFinished(void (*channel_finished)(int channel));
void Mix_SetPostMix(void (*mix_func)(void *udata, Uint8 *stream, int len), void *arg);

Mix_Music *Mix_LoadMUS_RW(SDL_RWops *src, int freesrc);
Mix_Music *Mix_LoadMUS(const char *file);
void Mix_FreeMusic(Mix_Music *music);
int  Mix_PlayMusic(Mix_Music *music, int loops);
int  Mix_FadeInMusic(Mix_Music *music, int loops, int ms);
int  Mix_FadeOutMusic(int ms);
void Mix_HaltMusic(void);
void Mix_PauseMusic(void);
void Mix_ResumeMusic(void);
int  Mix_PlayingMusic(void);
int  Mix_PausedMusic(void);
int  Mix_VolumeMusic(int volume);
void Mix_HookMusicFinished(void (*music_finished)(void));
int  Mix_SetSoundFonts(const char *paths);
double Mix_GetMusicPosition(Mix_Music *music);
int  Mix_SetMusicPosition(double position);

#ifdef __cplusplus
}
#endif

#endif /* OF_PC */
#endif /* OF_SDL2_SHIM_SDL_MIXER_H */
