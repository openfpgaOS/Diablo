#ifndef OF_AULIB_SHIM_AULIB_H
#define OF_AULIB_SHIM_AULIB_H
/*
 * aulib.h -- global init/teardown of the Aulib shim (see Aulib/Stream.h).
 * Backed by the openfpgaOS 48 kHz stereo audio path; DevilutionX's sound
 * code is unmodified.
 */
#include <string>
#include <SDL_audio.h>

namespace Aulib {

bool init(int freq, SDL_AudioFormat format, int channels, int frameSize,
          const std::string &device = std::string());
void quit();

int sampleRate();      // actual output rate (hardware: 48000)
int channelCount();    // 2
int frameSize();       // frames per mix block
SDL_AudioFormat sampleFormat(); // AUDIO_S16SYS

} // namespace Aulib

#endif
