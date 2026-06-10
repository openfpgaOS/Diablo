/*
 * of_aulib.cpp -- implementation of the Aulib shim (see include/Aulib/*.h).
 *
 * DevilutionX feeds audio through Aulib (decode + resample + mix + output).
 * Here, Aulib::Stream is a small software mixer: every active stream pulls
 * float samples from its Decoder, is nearest-neighbor resampled to the
 * 48 kHz hardware rate, gain/pan-applied, and summed into a stereo block
 * that of_aulib_pump() writes to of_audio_write(). The pump is driven from
 * the main loop by the SDL shim (of_sdl2.cpp).
 *
 * WAV/MP3 decoding uses dr_wav/dr_mp3 (the same decoders upstream Aulib
 * uses), streaming from the SDL_RWops so music isn't fully held in RAM.
 */
#include <aulib.h>
#include <Aulib/Stream.h>
#include <Aulib/Decoder.h>
#include <Aulib/Resampler.h>
#include <Aulib/DecoderDrwav.h>
#include <Aulib/DecoderDrmp3.h>

#include <SDL.h>
#include "of.h"
#include "of_mixer.h"

#include <cstdint>
#include <cstring>
#include <unistd.h>
#include <vector>

/* Forward declaration; definition is further down once the static helpers
 * it uses (audio_lock, g_voice_open, OF_OUT_RATE) are in scope. */
extern "C" void of_aulib_ensure_hw_mixer_inited(void);

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include "dr_wav.h"
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"

#define OF_OUT_RATE 48000
#define OF_MIX_BLOCK 1024
/* Per-pump fill cap (stereo pairs). After a stall the (~2.7 s) OS ring is
 * nearly empty; refilling it all in one pump would decode over a second of
 * music in a single frame. ~85 ms per pump never binds in steady state
 * (per-frame drain is ~800-1600 pairs at 30-60 fps) and spreads a post-stall
 * refill over a few frames. The HW stream voice holds at the write pointer,
 * so a partially-refilled ring is always safe. */
#define OF_PUMP_FILL_CAP_PAIRS 4096
/* The OS streaming voice index (targets/pocket/audio.c AUDIO_VOICE): music is
 * software-mixed into this HW mixer voice's ring. */
#define OF_AUDIO_RING_VOICE 31
/* Music-stop fade: ramp the streaming voice's HW volume to 0 before it is
 * disabled, so its already-buffered output samples drain a smooth ramp instead
 * of a hard cut. The OS mixer itself documents that snapping VOL_LR=0 then
 * deactivating is "the click source" (hal/mixer.c); a short ramp removes it.
 * OF_AUDIO_FADE_RAMP is the HW volume-ramp step (smaller = slower fade);
 * OF_AUDIO_FADE_US is how long to wait for the ramp to reach 0 before the voice
 * is silenced. Tune on HW if the click persists (slower ramp / longer wait). */
#define OF_AUDIO_FADE_RAMP 4
#define OF_AUDIO_FADE_US   12000

static bool g_inited = false;
static int  g_frameSize = OF_MIX_BLOCK;
#ifdef OF_PERF_TRACE
/* Minimum buffered audio observed since the last perf print (stereo pairs;
 * ~0 means the ring ran dry = audible underrun). Sampled in the pump before
 * refill; printed+reset by of_sdl2.cpp's perf line (extern there). */
int g_perf_aud_min_pairs = -1;
#endif
/* OS ring capacity in stereo pairs, measured at init (of_audio_free()
 * reports the full depth while the stream voice is inactive). */
static int  g_ring_capacity = 0;
/* Max pairs the pump keeps buffered in the ring. Music wants the FULL
 * (~2.7 s) ring so it coasts through loads. Push-fed streams (SVid movie
 * audio) must NOT: their decoder zero-fills when its frame queue is empty,
 * so a deep ring buries each frame's audio ~2.7 s behind the video --
 * storm_svid caps this during playback for lip-sync. 0 = uncapped. */
static int  g_max_buffered_pairs = 0;
static bool g_output_suspended = false;
static unsigned g_output_suspend_depth = 0;
static bool g_hw_mixer_inited = false;
/* Track whether the 48 kHz PCM ring has been started through
 * of_audio_write(). Do not call of_audio_stream_open(48000): ScummVM hit
 * audible resonance/vibrato from that path, and the OS already configures
 * voice 31 for 1:1 48 kHz playback on the first of_audio_write(). */
static bool g_voice_open = false;
static bool g_voice_log_printed = false;
/* Forward declaration -- defined further down with the stream registry. */
static std::vector<Aulib::Stream *> &ActiveStreams();
static void ensure_voice_open(void) {
	if (g_output_suspended) return;
	if (g_voice_open) return;
	g_voice_open = true;
	if (!g_voice_log_printed) {
		std::printf("[of] audio stream using of_audio_write(%dHz)\n",
		    OF_OUT_RATE);
		g_voice_log_printed = true;
	}
}
static void silence_voice_locked(void) {
	if (!g_voice_open) return;
	/* Zero and deactivate only voice 31. The next of_audio_write()
	 * restarts it at the default 48 kHz rate without using
	 * of_audio_stream_open(). */
	of_audio_init();
	g_voice_open = false;
}
static void close_voice_if_idle(void) {
	if (!g_voice_open) return;
	if (!ActiveStreams().empty()) return;
	silence_voice_locked();
}
/* Ramp the streaming voice (31) to silence and wait for the HW ramp to finish,
 * so that when it is subsequently disabled there is no hard-cut click. Runs on
 * the main thread (music stop), so usleep is fine. of_mixer_* on voice 31 is
 * safe: the voice is active and the mixer HW is enabled; the next music play
 * reconfigures its volume via of_audio_write -> configure_stream_voice. */
static void fade_ring_voice_and_wait(void) {
	if (!g_inited || !g_voice_open || g_output_suspended) return;
	of_mixer_set_volume_ramp(OF_AUDIO_RING_VOICE, OF_AUDIO_FADE_RAMP);
	of_mixer_set_volume(OF_AUDIO_RING_VOICE, 0);
	usleep(OF_AUDIO_FADE_US);
}
/* Underrun handling lives in HARDWARE now: voice 31 runs in the mixer's
 * stream mode (audio_mixer.v ctrl[3] + MIX_VOICE_WPTR) -- the OS publishes
 * the ring write pointer on every of_audio_write, and the voice fades out,
 * holds position in silence when it catches up, and resumes by itself when
 * fresh PCM lands. A stalled main thread (SD load, level gen) can no longer
 * make the voice replay stale ring contents, so the old app-side band-aids
 * (free-threshold voice reset, IRQ silence tick, idle-hook ring feeding,
 * the pre-decoded readahead) are gone. */

/* Spinlock serializing pump / suspend / mixer-init MMIO sequences. All
 * callers run on the single main thread today (the IRQ/idle-hook feeders
 * are gone), so it never contends -- kept because it is cheap and keeps
 * the device-access sections explicit. */
static volatile uint32_t g_audio_lock = 0;
static inline bool audio_try_lock(void) {
	uint32_t expected = 0;
	return __atomic_compare_exchange_n(&g_audio_lock, &expected, 1u,
	                                   false /* strong */,
	                                   __ATOMIC_ACQUIRE,
	                                   __ATOMIC_RELAXED);
}
static inline void audio_lock(void) {
	while (!audio_try_lock())
		/* Never contends today: all callers are on the single main
		 * thread (the IRQ/idle-hook feeders are gone; see the
		 * g_audio_lock comment above). */ ;
}
static inline void audio_unlock(void) {
	__atomic_store_n(&g_audio_lock, 0, __ATOMIC_RELEASE);
}

/* ---- active stream registry (single-threaded; pump + play/stop all run
 *      from the main loop, so no locking needed) ---- */
static std::vector<Aulib::Stream *> &ActiveStreams()
{
	static std::vector<Aulib::Stream *> v;
	return v;
}
static void AddStream(Aulib::Stream *s)
{
	auto &v = ActiveStreams();
	for (auto *p : v)
		if (p == s) return;
	v.push_back(s);
}
static void RemoveStream(Aulib::Stream *s)
{
	auto &v = ActiveStreams();
	for (size_t i = 0; i < v.size(); ++i)
		if (v[i] == s) { v.erase(v.begin() + i); return; }
}

/* ====================================================================== */
/* SDL_RWops <-> dr_libs callbacks                                         */
/* ====================================================================== */
static size_t OfWavRead(void *u, void *buf, size_t n) { return SDL_RWread((SDL_RWops *)u, buf, 1, n); }
static drwav_bool32 OfWavSeek(void *u, int off, drwav_seek_origin o)
{
	int w = (o == DRWAV_SEEK_CUR) ? RW_SEEK_CUR : (o == DRWAV_SEEK_END) ? RW_SEEK_END : RW_SEEK_SET;
	return SDL_RWseek((SDL_RWops *)u, off, w) < 0 ? DRWAV_FALSE : DRWAV_TRUE;
}
static drwav_bool32 OfWavTell(void *u, drwav_int64 *cur)
{
	Sint64 p = SDL_RWtell((SDL_RWops *)u);
	if (p < 0) return DRWAV_FALSE;
	*cur = (drwav_int64)p;
	return DRWAV_TRUE;
}
static size_t OfMp3Read(void *u, void *buf, size_t n) { return SDL_RWread((SDL_RWops *)u, buf, 1, n); }
static drmp3_bool32 OfMp3Seek(void *u, int off, drmp3_seek_origin o)
{
	int w = (o == DRMP3_SEEK_CUR) ? RW_SEEK_CUR : (o == DRMP3_SEEK_END) ? RW_SEEK_END : RW_SEEK_SET;
	return SDL_RWseek((SDL_RWops *)u, off, w) < 0 ? DRMP3_FALSE : DRMP3_TRUE;
}
static drmp3_bool32 OfMp3Tell(void *u, drmp3_int64 *cur)
{
	Sint64 p = SDL_RWtell((SDL_RWops *)u);
	if (p < 0) return DRMP3_FALSE;
	*cur = (drmp3_int64)p;
	return DRMP3_TRUE;
}

namespace Aulib {

/* ====================================================================== */
/* DecoderDrwav                                                            */
/* ====================================================================== */
struct DecoderDrwav::Impl {
	drwav wav;
	bool ok = false;
};
DecoderDrwav::DecoderDrwav() : d_(new Impl) {}
DecoderDrwav::~DecoderDrwav() { if (d_->ok) drwav_uninit(&d_->wav); }
bool DecoderDrwav::open(SDL_RWops *rwops)
{
	if (d_->ok) return true;
	d_->ok = drwav_init(&d_->wav, OfWavRead, OfWavSeek, OfWavTell, rwops, nullptr);
	return d_->ok;
}
int DecoderDrwav::getChannels() const { return d_->ok ? (int)d_->wav.channels : 2; }
int DecoderDrwav::getRate() const { return d_->ok ? (int)d_->wav.sampleRate : OF_OUT_RATE; }
bool DecoderDrwav::rewind() { return d_->ok && drwav_seek_to_pcm_frame(&d_->wav, 0); }
std::chrono::microseconds DecoderDrwav::duration() const
{
	if (!d_->ok || d_->wav.sampleRate == 0) return std::chrono::microseconds(0);
	return std::chrono::microseconds(d_->wav.totalPCMFrameCount * 1000000ULL / d_->wav.sampleRate);
}
bool DecoderDrwav::seekToTime(std::chrono::microseconds pos)
{
	if (!d_->ok) return false;
	return drwav_seek_to_pcm_frame(&d_->wav, (drwav_uint64)pos.count() * d_->wav.sampleRate / 1000000ULL);
}
int DecoderDrwav::doDecoding(float buf[], int len, bool &callAgain)
{
	callAgain = false;
	if (!d_->ok) return 0;
	int ch = (int)d_->wav.channels;
	if (ch <= 0) return 0;
	drwav_uint64 got = drwav_read_pcm_frames_f32(&d_->wav, (drwav_uint64)(len / ch), buf);
	return (int)(got * ch);
}

/* ====================================================================== */
/* DecoderDrmp3                                                            */
/* ====================================================================== */
struct DecoderDrmp3::Impl {
	drmp3 mp3;
	bool ok = false;
};
DecoderDrmp3::DecoderDrmp3() : d_(new Impl) {}
DecoderDrmp3::~DecoderDrmp3() { if (d_->ok) drmp3_uninit(&d_->mp3); }
bool DecoderDrmp3::open(SDL_RWops *rwops)
{
	if (d_->ok) return true;
	d_->ok = drmp3_init(&d_->mp3, OfMp3Read, OfMp3Seek, OfMp3Tell, nullptr, rwops, nullptr);
	return d_->ok;
}
int DecoderDrmp3::getChannels() const { return d_->ok ? (int)d_->mp3.channels : 2; }
int DecoderDrmp3::getRate() const { return d_->ok ? (int)d_->mp3.sampleRate : OF_OUT_RATE; }
bool DecoderDrmp3::rewind() { return d_->ok && drmp3_seek_to_pcm_frame(&d_->mp3, 0); }
std::chrono::microseconds DecoderDrmp3::duration() const
{
	if (!d_->ok || d_->mp3.sampleRate == 0) return std::chrono::microseconds(0);
	drmp3_uint64 frames = drmp3_get_pcm_frame_count(&d_->mp3);
	return std::chrono::microseconds(frames * 1000000ULL / d_->mp3.sampleRate);
}
bool DecoderDrmp3::seekToTime(std::chrono::microseconds pos)
{
	if (!d_->ok) return false;
	return drmp3_seek_to_pcm_frame(&d_->mp3, (drmp3_uint64)pos.count() * d_->mp3.sampleRate / 1000000ULL);
}
int DecoderDrmp3::doDecoding(float buf[], int len, bool &callAgain)
{
	callAgain = false;
	if (!d_->ok) return 0;
	int ch = (int)d_->mp3.channels;
	if (ch <= 0) return 0;
	drmp3_uint64 got = drmp3_read_pcm_frames_f32(&d_->mp3, (drmp3_uint64)(len / ch), buf);
	return (int)(got * ch);
}

/* ====================================================================== */
/* Stream                                                                  */
/* ====================================================================== */
Stream::Stream(SDL_RWops *rwops, std::unique_ptr<Decoder> decoder,
               std::unique_ptr<Resampler> resampler, bool closeRw)
    : rwops_(rwops)
    , decoder_(std::move(decoder))
    , resampler_(std::move(resampler))
    , closeRw_(closeRw)
{
	channels_ = decoder_ ? decoder_->getChannels() : 2;
}
Stream::~Stream()
{
	RemoveStream(this);
	decoder_.reset();
	if (closeRw_ && rwops_ != nullptr)
		SDL_RWclose(rwops_);
}
bool Stream::open()
{
	if (!decoder_) return false;
	opened_ = true;
	return true;
}
bool Stream::rewind()
{
	if (!decoder_) return false;
	decoder_->rewind();
	ihave_ = ipos_ = 0;
	srcPos_ = 0.0;
	consumed_ = 0;
	cur_[0] = cur_[1] = 0.0F;
	return true;
}
bool Stream::play(int iterations)
{
	if (!decoder_) return false;
	loopForever_ = (iterations == 0);
	playsLeft_ = (iterations == 0) ? 1 : iterations;
	rewind();
	playing_ = true;
	paused_ = false;
	AddStream(this);
	/* Start the HW voice if this is the first active stream. */
	ensure_voice_open();
	return true;
}
void Stream::stop()
{
	/* Fade the HW voice down before it is silenced so its buffered output
	 * drains a ramp, not a click. Reached via SoundSample::Stop()/Release()
	 * (music_stop routes through Release) and TSnd teardown. Only when this
	 * is the last active stream (the voice is about to be silenced). */
	const bool fade = ActiveStreams().size() == 1 && ActiveStreams()[0] == this;
	playing_ = false;
	RemoveStream(this);
	if (fade)
		fade_ring_voice_and_wait();
	/* If that was the last active stream, silence the HW voice so the
	 * ring's stale samples don't keep looping during the next gap. */
	close_voice_if_idle();
}
void Stream::pause() { paused_ = true; }
void Stream::resume() { paused_ = false; }
void Stream::setStereoPosition(float p)
{
	if (p < -1.0F) p = -1.0F;
	if (p > 1.0F) p = 1.0F;
	leftGain_ = (p <= 0.0F) ? 1.0F : (1.0F - p);
	rightGain_ = (p >= 0.0F) ? 1.0F : (1.0F + p);
}
std::chrono::microseconds Stream::duration()
{
	return decoder_ ? decoder_->duration() : std::chrono::microseconds(0);
}
void Stream::runFinishCallback()
{
	/* The callback may destroy this Stream (DuplicateSound's callback
	 * erases the owning SoundSample, sound.cpp).  Move the function to a
	 * stack local so the executing closure is not freed mid-invocation,
	 * and touch no members after the call -- `this` may be dangling. */
	auto cb = std::move(finishCallback_);
	if (cb)
		cb(*this);
}

bool Stream::nextFrame(float out[2])
{
	return decodeLoopedFrame(out);
}

bool Stream::decodeRawFrame(float out[2])
{
	const int ch = channels_;
	if (ipos_ >= ihave_) {
		bool again = false;
		int want = (ch == 1) ? OF_MIX_BLOCK : OF_MIX_BLOCK * 2;
		int got = decoder_->doDecoding(ibuf_, want, again);
		ihave_ = (ch > 0) ? got / ch : 0;
		ipos_ = 0;
		if (ihave_ <= 0) return false;
	}
	if (ch == 1) {
		out[0] = out[1] = ibuf_[ipos_];
	} else {
		out[0] = ibuf_[ipos_ * 2];
		out[1] = ibuf_[ipos_ * 2 + 1];
	}
	++ipos_;
	return true;
}

bool Stream::decodeLoopedFrame(float out[2])
{
	if (decodeRawFrame(out))
		return true;

	if (loopForever_ || playsLeft_ > 1) {
		if (!loopForever_)
			--playsLeft_;
		decoder_->rewind();
		ihave_ = ipos_ = 0;
		if (decodeRawFrame(out))
			return true;
	}

	playing_ = false;
	return false;
}

bool Stream::mixInto(int32_t *accum, int frames)
{
	if (!playing_) return false;
	if (paused_) return true;
	const int rate = decoder_->getRate();
	if (rate <= 0) { playing_ = false; return false; }
	/* float, not double: see Stream.h srcPos_ comment -- rv32imafc lacks
	 * the D extension and soft-FP doubles in this hot loop were costing
	 * tens of cycles per output frame per stream. */
	const float step = (float)rate / (float)OF_OUT_RATE;
	/* Per-stream scale at 0.5 of full s16 so two simultaneous streams
	 * (music + a single SFX) sum without clipping. The pump still saturates
	 * as a safety net for many simultaneous sounds. Without this, multi-stream
	 * audio clipped into square-wave distortion that sounds like breaking up.
	 *
	 * volume_/leftGain_/rightGain_ only change from outside the pump
	 * (setVolume/setStereoPosition), so the scale is loop-invariant across
	 * this block: hoist it out of the per-frame loop. This drops the inner
	 * loop from 3 fmul.s per channel per output frame to 1 -- soft-FP-free
	 * already (rv32imafc has a single-precision FPU), but fewer ops per frame
	 * per stream still matters in the audio hot path. */
	const float lscale = volume_ * leftGain_ * 16384.0F;
	const float rscale = volume_ * rightGain_ * 16384.0F;

	/* Keep srcPos_ from outgrowing the float mantissa. srcPos_ is the absolute
	 * source-frame position; left unbounded it passes ~2^24 after a few minutes
	 * of continuous music, where srcPos_ += step loses precision so the position
	 * advances too slowly (or stalls) and the music progressively slows/drags,
	 * then snaps back when the track resets. Fold the whole part back out each
	 * block and subtract the same count from consumed_, preserving the
	 * consumed_ <= (long)srcPos_ relationship (and thus the exact decode/output
	 * sequence) -- consumed_ is a pure local resampler counter, used nowhere
	 * else. Clamp to consumed_ because with step > 1 (source rate above
	 * 48 kHz) srcPos_ can legitimately be ahead of consumed_ at a block
	 * boundary; the clamp keeps consumed_ from going transiently negative. */
	if (srcPos_ >= 1.0F) {
		long whole = (long)srcPos_;
		if (whole > consumed_) whole = consumed_;
		srcPos_ -= (float)whole;
		consumed_ -= whole;
	}

	for (int i = 0; i < frames; ++i) {
		long target = (long)srcPos_;
		while (consumed_ <= target) {
			if (!nextFrame(cur_))
				return false;
			++consumed_;
		}
		if (!muted_) {
			accum[i * 2] += (int32_t)(cur_[0] * lscale);
			accum[i * 2 + 1] += (int32_t)(cur_[1] * rscale);
		}
		srcPos_ += step;
	}
	return true;
}

/* ====================================================================== */
/* Global init / state                                                     */
/* ====================================================================== */
bool init(int freq, SDL_AudioFormat format, int channels, int frameSize, const std::string &device)
{
	(void)freq; (void)format; (void)channels; (void)device;
	g_frameSize = frameSize > 0 ? frameSize : OF_MIX_BLOCK;
	/* Initialize the 48 kHz PCM ring but don't explicitly open the stream
	 * voice. The OS configures voice 31 on the first of_audio_write() at
	 * the default 1:1 rate. of_mixer_init is deferred until the first HW
	 * SFX needs it; that call resets all mixer voices, so the deferred path
	 * then resets the audio service state with of_audio_init() and lets the
	 * next pump restart voice 31 through of_audio_write(). */
	of_audio_init();
	/* Voice inactive right after init, so free == full ring capacity. */
	g_ring_capacity = of_audio_free();
	/* Underrun handling is in HARDWARE: voice 31 runs in the mixer's stream
	 * mode, holding (in ramped silence) at the published write pointer when
	 * the main-loop pump stalls and resuming by itself -- no app-side stall
	 * detection, ring resets, or background feeders. The ~2.7 s OS ring
	 * (OF_TARGET_AUDIO_STREAM_SIZE) is purely how long music keeps PLAYING
	 * through a blocking load before the graceful fade. */
	g_voice_open = false;
	g_output_suspended = false;
	g_output_suspend_depth = 0;
	g_inited = true;
	/* Initialize the HW mixer NOW, before any music stream exists.
	 * of_mixer_init resets every voice (including 31); deferring it to the
	 * first HW SFX -- which always lands mid-music -- dumped the whole
	 * music buffer and restarted the voice: a guaranteed once-per-boot
	 * audible drop ("mixer_init done; voice31 will restart"). At this
	 * point nothing is playing, so the reset is free. */
	of_aulib_ensure_hw_mixer_inited();
	return true;
}
void quit()
{
	g_inited = false;
	g_output_suspended = false;
	g_output_suspend_depth = 0;
	g_voice_open = false;
	ActiveStreams().clear();
}
int sampleRate() { return OF_OUT_RATE; }
int channelCount() { return 2; }
int frameSize() { return g_frameSize; }
SDL_AudioFormat sampleFormat() { return AUDIO_S16SYS; }

} // namespace Aulib

/* Deferred HW PCM mixer init. Called from soundsample.cpp the first time
 * a HW-capable SFX is played; safe to call repeatedly (no-op after first
 * success). of_mixer_init zeroes every mixer voice, including the audio
 * ring voice 31. After that, of_audio_init() clears the OS-side stream
 * bookkeeping so the next of_audio_write() reconfigures voice 31 at 48 kHz
 * without going through of_audio_stream_open(). */
extern "C" void of_aulib_ensure_hw_mixer_inited(void)
{
	if (g_hw_mixer_inited) return;
	audio_lock();
	of_mixer_init(OF_MIXER_MAX_VOICES, OF_MIXER_OUTPUT_RATE);
	of_mixer_set_master_volume(g_output_suspended ? 0 : 255);
	of_mixer_set_group_volume(OF_MIXER_GROUP_SFX, 255);
	of_mixer_set_group_volume(OF_MIXER_GROUP_MUSIC, 255);
	of_mixer_set_group_volume(OF_MIXER_GROUP_VOICE, 255);
	of_mixer_set_group_volume(OF_MIXER_GROUP_AUX, 255);
	if (g_voice_open || !ActiveStreams().empty())
		of_audio_init();
	g_voice_open = false;
	audio_unlock();
	std::printf("[of] mixer_init done; voice31 will restart on next audio write\n");
	g_hw_mixer_inited = true;
}

extern "C" void of_aulib_suspend_output(void)
{
	if (!g_inited) return;
	audio_lock();
	++g_output_suspend_depth;
	g_output_suspended = true;
	silence_voice_locked();
	/* Hardware SFX are independent of the Aulib stream pump. Stop them
	 * during level transitions too, so loading screens are actually quiet. */
	if (g_hw_mixer_inited) {
		of_mixer_set_master_volume(0);
		of_mixer_stop_all();
	}
	audio_unlock();
}

extern "C" void of_aulib_resume_output(void)
{
	if (!g_inited) return;
	audio_lock();
	if (g_output_suspend_depth > 0)
		--g_output_suspend_depth;
	if (g_output_suspend_depth == 0) {
		g_output_suspended = false;
		if (g_hw_mixer_inited)
			of_mixer_set_master_volume(255);
	}
	audio_unlock();
}

/* Cap (or uncap, pairs=0) how much audio the pump keeps buffered ahead of
 * playback. 48 kHz stereo pairs; see g_max_buffered_pairs. */
extern "C" void of_aulib_set_max_buffered_pairs(int pairs)
{
	g_max_buffered_pairs = pairs;
}

/* Stop all HW SFX voices (0-30) WITHOUT touching the music stream voice 31.
 * Level transitions must tear down looping SFX so no voice keeps DMA-reading
 * level-owned PCM buffers that are about to be freed -- but music now plays
 * THROUGH the load (deep ring + HW stream-mode hold), so the old whole-output
 * suspend is gone. NOTE: of_mixer_stop_all() is NOT usable here -- it writes
 * CTRL=0 to every voice INCLUDING 31, killing music behind the OS audio
 * bookkeeping's back. */
extern "C" void of_aulib_stop_hw_sfx(void)
{
	if (!g_hw_mixer_inited)
		return;
	for (int v = 0; v < OF_AUDIO_RING_VOICE; ++v)
		of_mixer_stop(v);
}

/* ====================================================================== */
/* Pump -- called from the SDL shim's main-loop event/delay path.          */
/* ====================================================================== */
extern "C" void of_aulib_pump(void)
{
	if (!g_inited) return;
	audio_lock();
	if (g_output_suspended) {
		silence_voice_locked();
		audio_unlock();
		return;
	}
	/* If streams are active, mark the PCM path as started. The actual HW
	 * voice is configured by of_audio_write() when the ring is first fed. */
	{
		auto &v0 = ActiveStreams();
		if (v0.empty()) {
			close_voice_if_idle();
			audio_unlock();
			return;
		}
		ensure_voice_open();
	}
	static int32_t acc[OF_MIX_BLOCK * 2];
	static int16_t out[OF_MIX_BLOCK * 2];

	int freePairs = of_audio_free();
	if (g_ring_capacity > 0) {
		int buffered = g_ring_capacity - 1 - freePairs;
		/* OS voice inactive: of_audio_free() reports the full ring (N, not
		 * N-1), making this -1 -- the ring truly holds nothing then. */
		if (buffered < 0) buffered = 0;
#ifdef OF_PERF_TRACE
		if (g_perf_aud_min_pairs < 0 || buffered < g_perf_aud_min_pairs)
			g_perf_aud_min_pairs = buffered;
#endif
		if (g_max_buffered_pairs > 0) {
			/* Latency cap (movie audio): keep at most N pairs buffered. */
			int room = g_max_buffered_pairs - buffered;
			if (room < 0) room = 0;
			if (freePairs > room) freePairs = room;
		}
	}
	{
		/* Per-pump fill cap: amortizes refill bursts in steady state, but
		 * while the buffer is LOW (music just started / post-stall) fill
		 * 4x harder so the cushion banks before a hiccup can expose it. */
		int cap = OF_PUMP_FILL_CAP_PAIRS;
		if (g_ring_capacity > 0
		    && g_ring_capacity - 1 - freePairs < g_ring_capacity / 2)
			cap = OF_PUMP_FILL_CAP_PAIRS * 4;
		if (freePairs > cap)
			freePairs = cap;
	}
	while (freePairs > 0) {
		int n = freePairs < OF_MIX_BLOCK ? freePairs : OF_MIX_BLOCK;
		std::memset(acc, 0, sizeof(int32_t) * n * 2);

		auto &v = ActiveStreams();
		for (size_t i = 0; i < v.size();) {
			Aulib::Stream *s = v[i];
			if (!s->mixInto(acc, n)) {
				v.erase(v.begin() + i);
				/* Runs under g_audio_lock: finish callbacks must not
				 * call Stream::play()/stop(), suspend/resume, or
				 * of_aulib_ensure_hw_mixer_inited -- the audio lock is
				 * a non-reentrant spin and would hang forever. */
				s->runFinishCallback();
			} else {
				++i;
			}
		}
		for (int j = 0; j < n * 2; ++j) {
			int32_t x = acc[j];
			if (x > 32767) x = 32767; else if (x < -32768) x = -32768;
			out[j] = (int16_t)x;
		}
		int wrote = of_audio_write(out, n);
		if (wrote <= 0)
			break;
		freePairs -= wrote;
	}
	/* End-of-stream auto-removed the last active stream inside the loop?
	 * Close the voice so the ring isn't left looping its tail samples. */
	close_voice_if_idle();
	audio_unlock();
}
