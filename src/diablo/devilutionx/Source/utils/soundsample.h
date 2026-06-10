#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <Aulib/Stream.h>

#include "engine/sound_defs.hpp"
#include "utils/stdcompat/shared_ptr_array.hpp"

#ifdef OPENFPGAOS
#include "of_mixer.h"
#endif

namespace devilution {

/* Two playback backends:
 *  - SW (Aulib::Stream): used for music (streaming) and MP3-format SFX.
 *    Mixed on the CPU by of_aulib_pump and pushed to HW voice 31.
 *  - HW (openfpgaOS mixer): used for in-memory WAV SFX. Decoded once at
 *    SetChunk time to s16 mono PCM and handed to
 *    of_mixer_alloc_for_group_h, which plays it on one of the 31 idle
 *    HW voices with hardware resampling and per-voice volume/pan.
 *    CPU cost per SFX play is now ~zero
 *    (kick off + occasional volume/pan write). */
class SoundSample final {
public:
	SoundSample() = default;
	SoundSample(SoundSample &&) noexcept = default;
	SoundSample &operator=(SoundSample &&) noexcept = default;

	[[nodiscard]] bool IsLoaded() const
	{
#ifdef OPENFPGAOS
		if (hw_pcm_ptr_ != nullptr) return true;
#endif
		return stream_ != nullptr;
	}

	void Release();
	bool IsPlaying();

	// Returns 0 on success.
	int SetChunkStream(std::string filePath, bool isMp3, bool logErrors = true);

	void SetFinishCallback(Aulib::Stream::Callback &&callback)
	{
		if (stream_)
			stream_->setFinishCallback(std::forward<Aulib::Stream::Callback>(callback));
		/* HW path: no per-voice finish callback (the mixer's
		 * end-callback API is global + bitmask-keyed). HW duplicates
		 * are instead reaped by ReapDuplicateSounds() polling
		 * IsPlaying() from sound_update(). */
	}

#ifdef OPENFPGAOS
	/* True when this sample plays only on a HW mixer voice: no Aulib
	 * stream exists, so no finish callback was installed (see above)
	 * and cleanup must come from ReapDuplicateSounds() polling. */
	[[nodiscard]] bool IsHwVoiceOnly() const
	{
		return stream_ == nullptr;
	}
#endif

	/**
	 * @brief Sets the sample's WAV, FLAC, or Ogg/Vorbis data.
	 * @param fileData Buffer containing the data
	 * @param dwBytes Length of buffer
	 * @param isMp3 Whether the data is an MP3
	 * @return 0 on success, -1 otherwise
	 */
	int SetChunk(ArraySharedPtr<std::uint8_t> fileData, std::size_t dwBytes, bool isMp3);

	[[nodiscard]] bool IsStreaming() const
	{
		return file_data_ == nullptr;
	}

	int DuplicateFrom(const SoundSample &other)
	{
		if (other.IsStreaming())
			return SetChunkStream(other.file_path_, other.isMp3_);
		return SetChunk(other.file_data_, other.file_data_size_, other.isMp3_);
	}

	/**
	 * @brief Start playing the sound for a given number of iterations (0 means loop).
	 */
	bool Play(int numIterations = 1);

	/**
	 * @brief Start playing the sound with the given sound and user volume, and a stereo position.
	 */
	bool PlayWithVolumeAndPan(int logSoundVolume, int logUserVolume, int logPan)
	{
		SetVolume(logSoundVolume + logUserVolume * (ATTENUATION_MIN / VOLUME_MIN), ATTENUATION_MIN, 0);
		SetStereoPosition(logPan);
		return Play();
	}

	/**
	 * @brief Stop playing the sound
	 */
	void Stop();

	void SetVolume(int logVolume, int logMin, int logMax);
	void SetStereoPosition(int logPan);

	void Mute();
	void Unmute();

	/**
	 * @return Audio duration in ms
	 */
	int GetLength() const;

private:
	// Non-streaming audio fields:
	ArraySharedPtr<std::uint8_t> file_data_;
	std::size_t file_data_size_ = 0;

	// Set for streaming audio to allow for duplicating it:
	std::string file_path_;

	bool isMp3_ = false;

	std::unique_ptr<Aulib::Stream> stream_;

#ifdef OPENFPGAOS
	/* HW-mixer SFX state. When hw_pcm_ptr_ != nullptr, this SoundSample
	 * plays through the openfpgaOS HW PCM mixer instead of Aulib.
	 * For 16-bit WAVs, hw_pcm_ptr_ points into file_data_ (shared
	 * lifetime via the ArraySharedPtr above). For 8-bit WAVs, the data
	 * is converted once from unsigned WAV PCM to signed s16 and stored
	 * in hw_pcm_s16_. PCM is mono because the HW mixer plays one voice
	 * per channel; stereo WAVs fall through to the Aulib SW path. */
	const uint8_t  *hw_pcm_ptr_   = nullptr;
	std::vector<int16_t> hw_pcm_s16_;
	uint32_t        hw_sample_count_ = 0;
	uint32_t        hw_rate_       = 0;
	of_mixer_handle_t hw_handle_ = OF_MIXER_HANDLE_INVALID;
	uint8_t hw_volume_       = 255; /* 0-255 linear */
	uint8_t hw_pan_          = 128; /* 0=left, 128=center, 255=right */
	uint8_t hw_saved_volume_ = 255; /* for Mute/Unmute */
	bool    hw_muted_        = false;
	bool    hw_loop_         = false;
#endif
};

} // namespace devilution
