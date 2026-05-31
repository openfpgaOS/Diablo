#ifndef OF_AULIB_SHIM_STREAM_H
#define OF_AULIB_SHIM_STREAM_H
/*
 * Aulib::Stream -- a playable audio source. The shim implements a small
 * software mixer: active streams are summed each pump and written to the
 * openfpgaOS 48 kHz stereo output (see of_aulib.cpp). Each stream pulls
 * float samples from its Decoder and is nearest-neighbor resampled to the
 * hardware rate. DevilutionX's sound code uses this API unmodified.
 */
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <SDL_rwops.h>
#include <aulib.h>
#include <Aulib/Decoder.h>
#include <Aulib/Resampler.h>

namespace Aulib {

class Stream {
public:
	using Callback = std::function<void(Stream &)>;

	Stream(SDL_RWops *rwops, std::unique_ptr<Decoder> decoder,
	       std::unique_ptr<Resampler> resampler, bool closeRw);
	~Stream();

	bool open();
	bool play(int iterations = 1);
	void stop();
	void pause();
	void resume();
	bool isPlaying() const { return playing_; }
	bool isPaused() const { return paused_; }
	void setVolume(float volume) { volume_ = volume; }
	void setStereoPosition(float position);
	void setReadAheadFrames(int frames);
	void mute() { muted_ = true; }
	void unmute() { muted_ = false; }
	bool isMuted() const { return muted_; }
	void setFinishCallback(Callback callback) { finishCallback_ = std::move(callback); }
	std::chrono::microseconds duration();
	bool rewind();

	// --- Called by the mixer pump (of_aulib.cpp) ---
	// Sums this stream into the int32 stereo accumulator for `frames`
	// output frames; returns false when playback has finished.
	bool mixInto(int32_t *accum, int frames);
	void maintainReadAhead();
	void runFinishCallback();

private:
	bool nextFrame(float out[2]);
	bool decodeRawFrame(float out[2]);
	bool decodeLoopedFrame(float out[2]);
	void clearReadAhead();
	bool fillReadAhead(int maxFrames);
	bool popReadAhead(float out[2]);

	SDL_RWops *rwops_;
	std::unique_ptr<Decoder> decoder_;
	std::unique_ptr<Resampler> resampler_;
	bool closeRw_;

	// Channel count is fixed once the decoder is opened; cached at
	// construction so decodeRawFrame() (hot decode loop) doesn't make a
	// virtual Decoder::getChannels() call per source frame.
	int channels_ = 2;

	bool opened_ = false;
	bool playing_ = false;
	bool paused_ = false;
	bool muted_ = false;
	bool loopForever_ = false;
	int playsLeft_ = 0;

	float volume_ = 1.0F;
	float leftGain_ = 1.0F;
	float rightGain_ = 1.0F;

	Callback finishCallback_;

	// Nearest-neighbor resample state (decoder rate -> hardware rate).
	// float, not double: rv32imafc has hardware single-precision FP but NO
	// D extension, so double arithmetic falls back to libgcc soft-FP (tens
	// of cycles per op) -- this is per output frame, per stream, per pump,
	// directly causing audio underruns. Float keeps it on the FPU.
	float srcPos_ = 0.0F;
	long consumed_ = 0;
	float cur_[2] = { 0.0F, 0.0F };

	// Decoded-sample staging buffer.
	float ibuf_[1024 * 2];
	int ihave_ = 0; // frames available in ibuf_
	int ipos_ = 0;  // frames consumed from ibuf_

	// Optional decoded stereo source-frame ring. This is used for long music
	// streams on openfpgaOS so the mixer mostly consumes RAM instead of doing
	// MPQ/WAV reads directly in the audio pump.
	std::vector<float> readAhead_;
	int readAheadCapacity_ = 0; // stereo frames
	int readAheadRead_ = 0;
	int readAheadWrite_ = 0;
	int readAheadCount_ = 0;
};

} // namespace Aulib

#endif
