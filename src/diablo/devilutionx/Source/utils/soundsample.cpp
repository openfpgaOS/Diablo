#include "utils/soundsample.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <utility>

#include <Aulib/DecoderDrmp3.h>
#include <Aulib/DecoderDrwav.h>
#include <SDL.h>
#ifdef USE_SDL1
#include "utils/sdl2_to_1_2_backports.h"
#else
#include "utils/sdl2_backports.h"
#endif

#include "engine/assets.hpp"
#include "options.h"
#include "utils/aulib.hpp"
#include "utils/log.hpp"
#include "utils/math.h"
#include "utils/stubs.h"

#ifdef OPENFPGAOS
#include "of_mixer.h"
extern "C" void of_aulib_ensure_hw_mixer_inited(void);

namespace {
/* HW SFX path enabled. WAVs that don't match the mono / 8-or-16-bit PCM
 * shape still fall through to the Aulib SW path inside SetChunk. */
constexpr bool kHwMixerSfxEnabled = true;
} // namespace
#endif

namespace devilution {

namespace {

constexpr float LogBase = 10.0;

/**
 * Scaling factor for attenuating volume.
 * Picked so that a volume change of -10 dB results in half perceived loudness.
 * VolumeScale = -1000 / log(0.5)
 */
constexpr float VolumeScale = 3321.9281;

/**
 * Min and max volume range, in millibel.
 * -100 dB (muted) to 0 dB (max. loudness).
 */
constexpr float MillibelMin = -10000.F;
constexpr float MillibelMax = 0.F;

/**
 * Stereo separation factor for left/right speaker panning. Lower values increase separation, moving
 * sounds further left/right, while higher values will pull sounds more towards the middle, reducing separation.
 * Current value is tuned to have ~2:1 mix for sounds that happen on the edge of a 640x480 screen.
 */
constexpr float StereoSeparation = 6000.F;

float PanLogToLinear(int logPan)
{
	if (logPan == 0)
		return 0;

	auto factor = std::pow(LogBase, static_cast<float>(-std::abs(logPan)) / StereoSeparation);

	return copysign(1.F - factor, static_cast<float>(logPan));
}

std::unique_ptr<Aulib::Decoder> CreateDecoder(bool isMp3)
{
	if (isMp3)
		return std::make_unique<Aulib::DecoderDrmp3>();
	return std::make_unique<Aulib::DecoderDrwav>();
}

std::unique_ptr<Aulib::Stream> CreateStream(SDL_RWops *handle, bool isMp3)
{
	auto decoder = CreateDecoder(isMp3);
	if (!decoder->open(handle)) // open for `getRate`
		return nullptr;
	auto resampler = CreateAulibResampler(decoder->getRate());
	return std::make_unique<Aulib::Stream>(handle, std::move(decoder), std::move(resampler), /*closeRw=*/true);
}

/**
 * @brief Converts log volume passed in into linear volume.
 * @param logVolume Logarithmic volume in the range [logMin..logMax]
 * @param logMin Volume range minimum (usually ATTENUATION_MIN for game sounds and VOLUME_MIN for volume sliders)
 * @param logMax Volume range maximum (usually 0)
 * @return Linear volume in the range [0..1]
 */
float VolumeLogToLinear(int logVolume, int logMin, int logMax)
{
	const auto logScaled = math::Remap(static_cast<float>(logMin), static_cast<float>(logMax), MillibelMin, MillibelMax, static_cast<float>(logVolume));
	return std::pow(LogBase, logScaled / VolumeScale); // linVolume
}

} // namespace

#ifdef OPENFPGAOS
namespace {

/* In-place WAV header parse, modeled on PocketDukeNukem's
 * d3d_parse_wav_local (src/duke3d/d3d_audio.c on that repo). Walks
 * the RIFF/WAVE chunk list looking for `fmt ` (format, rate, channels,
 * bits) and `data` (the raw PCM payload). Returns true on success and
 * stores a pointer that lives inside `data` (kept alive by file_data_'s
 * ArraySharedPtr). Only the shapes the HW mixer can play without a
 * decode pass: uncompressed PCM, mono, 8 or 16 bits/sample. Stereo,
 * ADPCM, IEEE float, etc. fall through to the Aulib SW path. */
static inline uint16_t rd16le(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t rd32le(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
	     | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
bool ParseWavForHwMixer(const uint8_t *data, uint32_t size,
                         const uint8_t *&pcm, uint32_t &sampleCount,
                         uint32_t &rate, uint8_t &bits)
{
	if (data == nullptr || size < 44) return false;
	if (data[0] != 'R' || data[1] != 'I' || data[2] != 'F' || data[3] != 'F') return false;
	if (data[8] != 'W' || data[9] != 'A' || data[10] != 'V' || data[11] != 'E') return false;
	const uint8_t *p = data + 12;
	const uint8_t *end = data + size;
	uint16_t channels = 0;
	bool foundFmt = false;
	while (p + 8 <= end) {
		const uint8_t *id = p;
		const uint32_t chunkSize = rd32le(p + 4);
		const uint8_t *chunkData = p + 8;
		if ((uint32_t)(end - chunkData) < chunkSize) return false;
		if (id[0] == 'f' && id[1] == 'm' && id[2] == 't' && id[3] == ' '
		    && chunkSize >= 16) {
			const uint16_t fmt = rd16le(chunkData + 0);
			if (fmt != 1) return false; /* PCM only */
			channels = rd16le(chunkData + 2);
			rate     = rd32le(chunkData + 4);
			bits     = (uint8_t)rd16le(chunkData + 14);
			foundFmt = true;
		} else if (id[0] == 'd' && id[1] == 'a' && id[2] == 't' && id[3] == 'a'
		    && foundFmt) {
			if (channels != 1) return false;       /* mono only */
			if (bits != 8 && bits != 16) return false;
			if (rate == 0) return false;
			const uint32_t bytesPerSample = bits / 8u;
			if (chunkSize < bytesPerSample) return false;
			pcm         = chunkData;
			sampleCount = chunkSize / bytesPerSample;
			return true;
		}
		/* Chunks are 2-byte aligned. */
		p = chunkData + chunkSize;
		if (chunkSize & 1u) p++;
	}
	return false;
}

} // namespace
#endif // OPENFPGAOS

///// SoundSample /////

void SoundSample::Release()
{
#ifdef OPENFPGAOS
	if (hw_handle_ != OF_MIXER_HANDLE_INVALID) {
		of_mixer_stop_h(hw_handle_);
		hw_handle_ = OF_MIXER_HANDLE_INVALID;
	}
	hw_pcm_ptr_      = nullptr;
	hw_pcm_s16_.clear();
	hw_sample_count_ = 0;
	hw_rate_         = 0;
	hw_loop_         = false;
	hw_muted_        = false;
#endif
	/* Stop (not just destroy) the stream: music_stop() reaches here without
	 * ever calling Stop(), and Stream::stop() is what fades the HW ring
	 * voice and closes it -- destroying the stream directly would hard-cut
	 * mid-waveform and leave the ring backlog to play ahead of the next
	 * track. Idempotent when Stop() already ran (TSnd teardown). */
	if (stream_)
		stream_->stop();
	stream_ = nullptr;
	file_data_ = nullptr;
	file_data_size_ = 0;
}

/**
 * @brief Check if a the sound is being played atm
 */
bool SoundSample::IsPlaying()
{
#ifdef OPENFPGAOS
	if (hw_pcm_ptr_ != nullptr) {
		return hw_handle_ != OF_MIXER_HANDLE_INVALID
		    && of_mixer_handle_active(hw_handle_) != 0;
	}
#endif
	return stream_ && stream_->isPlaying();
}

bool SoundSample::Play(int numIterations)
{
#ifdef OPENFPGAOS
	if (hw_pcm_ptr_ != nullptr) {
		/* Replace any in-flight voice for this SoundSample before
		 * (re)triggering -- DevilutionX expects Play() to restart a
		 * sample even if a previous play hasn't finished. */
		if (hw_handle_ != OF_MIXER_HANDLE_INVALID)
			of_mixer_stop_h(hw_handle_);
		/* Lazy init: of_mixer_init resets voice 31, so defer it to the
		 * first actual HW SFX play and let of_aulib reset the audio
		 * service state before the next music pump. */
		of_aulib_ensure_hw_mixer_inited();
		/* HW mixer loop is all-or-nothing: numIterations==0 means loop
		 * forever (matching Aulib's play(0) semantics). A finite N>1 isn't
		 * representable as a hardware loop count, so play it once rather
		 * than turning it into an infinite loop. */
		hw_loop_ = (numIterations == 0);
		const uint8_t volume = hw_muted_ ? 0 : hw_volume_;
		/* Signed 16-bit mono PCM. Group-aware allocator tags the
		 * voice as SFX before programming so group_vol[SFX] is
		 * composed against the right volume on the very first sample. */
		hw_handle_ = of_mixer_alloc_for_group_h(
		    OF_MIXER_GROUP_SFX, hw_pcm_ptr_, hw_sample_count_,
		    hw_rate_, /*priority=*/0, volume);
		if (hw_handle_ == OF_MIXER_HANDLE_INVALID)
			return false;
		of_mixer_set_pan_h(hw_handle_, hw_pan_);
		if (hw_loop_) {
			of_mixer_set_loop_h(hw_handle_, 0,
			    static_cast<int>(hw_sample_count_));
		}
		return true;
	}
#endif
	if (!stream_->play(numIterations)) {
		LogError(LogCategory::Audio, "Aulib::Stream::play (from SoundSample::Play): {}", SDL_GetError());
		return false;
	}
	return true;
}

void SoundSample::Stop()
{
#ifdef OPENFPGAOS
	if (hw_pcm_ptr_ != nullptr) {
		if (hw_handle_ != OF_MIXER_HANDLE_INVALID) {
			of_mixer_stop_h(hw_handle_);
			hw_handle_ = OF_MIXER_HANDLE_INVALID;
		}
		return;
	}
#endif
	if (stream_) stream_->stop();
}

void SoundSample::Mute()
{
#ifdef OPENFPGAOS
	if (hw_pcm_ptr_ != nullptr) {
		if (!hw_muted_) {
			hw_saved_volume_ = hw_volume_;
			hw_muted_ = true;
		}
		if (hw_handle_ != OF_MIXER_HANDLE_INVALID)
			of_mixer_set_volume_h(hw_handle_, 0);
		return;
	}
#endif
	if (stream_) stream_->mute();
}

void SoundSample::Unmute()
{
#ifdef OPENFPGAOS
	if (hw_pcm_ptr_ != nullptr) {
		if (hw_muted_) {
			hw_volume_ = hw_saved_volume_;
			hw_muted_ = false;
		}
		if (hw_handle_ != OF_MIXER_HANDLE_INVALID)
			of_mixer_set_volume_h(hw_handle_, hw_volume_);
		return;
	}
#endif
	if (stream_) stream_->unmute();
}

int SoundSample::SetChunkStream(std::string filePath, bool isMp3, bool logErrors)
{
#ifdef OPENFPGAOS
	if (hw_handle_ != OF_MIXER_HANDLE_INVALID) {
		of_mixer_stop_h(hw_handle_);
		hw_handle_ = OF_MIXER_HANDLE_INVALID;
	}
	hw_pcm_ptr_ = nullptr;
	hw_pcm_s16_.clear();
	hw_sample_count_ = 0;
	hw_rate_ = 0;
#endif
	SDL_RWops *handle = OpenAssetAsSdlRwOps(filePath.c_str(), /*threadsafe=*/true);
	if (handle == nullptr) {
		if (logErrors)
			LogError(LogCategory::Audio, "OpenAsset failed (from SoundSample::SetChunkStream) for {}: {}", filePath, SDL_GetError());
		return -1;
	}
	file_path_ = std::move(filePath);
	isMp3_ = isMp3;
	stream_ = CreateStream(handle, isMp3);
	if (!stream_->open()) {
		stream_ = nullptr;
		if (logErrors)
			LogError(LogCategory::Audio, "Aulib::Stream::open (from SoundSample::SetChunkStream) for {}: {}", file_path_, SDL_GetError());
		return -1;
	}
	return 0;
}

int SoundSample::SetChunk(ArraySharedPtr<std::uint8_t> fileData, std::size_t dwBytes, bool isMp3)
{
	isMp3_ = isMp3;
	file_data_ = std::move(fileData);
	file_data_size_ = dwBytes;

#ifdef OPENFPGAOS
	if (hw_handle_ != OF_MIXER_HANDLE_INVALID) {
		of_mixer_stop_h(hw_handle_);
		hw_handle_ = OF_MIXER_HANDLE_INVALID;
	}
	hw_pcm_ptr_ = nullptr;
	hw_pcm_s16_.clear();
	hw_sample_count_ = 0;
	hw_rate_ = 0;
	hw_loop_ = false;
	hw_muted_ = false;

	/* HW mixer SFX path: gated by kHwMixerSfxEnabled (see top of TU). */
	if (kHwMixerSfxEnabled && !isMp3) {
		const uint8_t *pcm = nullptr;
		uint32_t       sampleCount = 0;
		uint32_t       rate = 0;
		uint8_t        bits = 16;
		if (ParseWavForHwMixer(file_data_.get(),
		        static_cast<uint32_t>(file_data_size_),
		        pcm, sampleCount, rate, bits)) {
			if (bits == 8) {
				hw_pcm_s16_.resize(sampleCount);
				for (uint32_t i = 0; i < sampleCount; ++i)
					hw_pcm_s16_[i] = static_cast<int16_t>((static_cast<int>(pcm[i]) - 128) << 8);
				hw_pcm_ptr_ = reinterpret_cast<const uint8_t *>(hw_pcm_s16_.data());
			} else {
				hw_pcm_s16_.clear();
				hw_pcm_ptr_ = pcm;
			}
			hw_sample_count_ = sampleCount;
			hw_rate_         = rate;
			hw_handle_       = OF_MIXER_HANDLE_INVALID;
			hw_volume_       = 255;
			hw_pan_          = 128;
			hw_loop_         = false;
			hw_muted_        = false;
			return 0;
		}
	}
#endif

	SDL_RWops *buf = SDL_RWFromConstMem(file_data_.get(), dwBytes);
	if (buf == nullptr) {
		return -1;
	}

	stream_ = CreateStream(buf, isMp3_);
	if (!stream_->open()) {
		stream_ = nullptr;
		file_data_ = nullptr;
		LogError(LogCategory::Audio, "Aulib::Stream::open (from SoundSample::SetChunk): {}", SDL_GetError());
		return -1;
	}

	return 0;
}

void SoundSample::SetVolume(int logVolume, int logMin, int logMax)
{
	const float linear = VolumeLogToLinear(logVolume, logMin, logMax);
#ifdef OPENFPGAOS
	if (hw_pcm_ptr_ != nullptr) {
		const int v = static_cast<int>(linear * 255.0F + 0.5F);
		hw_volume_ = static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
		if (!hw_muted_) hw_saved_volume_ = hw_volume_;
		if (hw_handle_ != OF_MIXER_HANDLE_INVALID)
			of_mixer_set_volume_h(hw_handle_, hw_muted_ ? 0 : hw_volume_);
		return;
	}
#endif
	stream_->setVolume(linear);
}

void SoundSample::SetStereoPosition(int logPan)
{
	const float linear = PanLogToLinear(logPan);
#ifdef OPENFPGAOS
	if (hw_pcm_ptr_ != nullptr) {
		/* PanLogToLinear yields -1..+1. HW mixer wants 0..255 with
		 * 128 = center. */
		const int p = static_cast<int>((linear * 0.5F + 0.5F) * 255.0F + 0.5F);
		hw_pan_ = static_cast<uint8_t>(p < 0 ? 0 : p > 255 ? 255 : p);
		if (hw_handle_ != OF_MIXER_HANDLE_INVALID)
			of_mixer_set_pan_h(hw_handle_, hw_pan_);
		return;
	}
#endif
	stream_->setStereoPosition(linear);
}

int SoundSample::GetLength() const
{
#ifdef OPENFPGAOS
	if (hw_pcm_ptr_ != nullptr && hw_rate_ > 0) {
		return static_cast<int>((static_cast<uint64_t>(hw_sample_count_) * 1000ULL)
		    / hw_rate_);
	}
#endif
	if (!stream_)
		return 0;
	return std::chrono::duration_cast<std::chrono::milliseconds>(stream_->duration()).count();
}

} // namespace devilution
