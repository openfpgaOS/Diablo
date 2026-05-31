#ifndef OF_AULIB_SHIM_DECODER_DRWAV_H
#define OF_AULIB_SHIM_DECODER_DRWAV_H
#include <memory>
#include <Aulib/Decoder.h>
namespace Aulib {
// WAV decoder backed by dr_wav (streaming from the SDL_RWops).
class DecoderDrwav final : public Decoder {
public:
	DecoderDrwav();
	~DecoderDrwav() override;
	bool open(SDL_RWops *rwops) override;
	int getChannels() const override;
	int getRate() const override;
	bool rewind() override;
	std::chrono::microseconds duration() const override;
	bool seekToTime(std::chrono::microseconds pos) override;
	int doDecoding(float buf[], int len, bool &callAgain) override;
private:
	struct Impl;
	std::unique_ptr<Impl> d_;
};
} // namespace Aulib
#endif
