#ifndef OF_AULIB_SHIM_DECODER_H
#define OF_AULIB_SHIM_DECODER_H
/*
 * Aulib::Decoder -- abstract decoder. DevilutionX subclasses it
 * (PushAulibDecoder) and the shim provides DecoderDrwav / DecoderDrmp3.
 * doDecoding() yields interleaved float samples in [-1, 1].
 */
#include <chrono>
#include <SDL_rwops.h>

namespace Aulib {

class Decoder {
public:
	virtual ~Decoder() = default;

	virtual bool open(SDL_RWops *rwops) = 0;
	virtual int getChannels() const = 0;
	virtual int getRate() const = 0;
	virtual bool rewind() = 0;
	virtual std::chrono::microseconds duration() const = 0;
	virtual bool seekToTime(std::chrono::microseconds pos) = 0;

	// Writes up to `len` interleaved float samples; returns count written.
	virtual int doDecoding(float buf[], int len, bool &callAgain) = 0;
};

} // namespace Aulib

#endif
