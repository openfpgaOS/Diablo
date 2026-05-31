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
#define OF_READAHEAD_REFILL_FRAMES 1024
#ifndef OF_AUDIO_FIFO
#define OF_AUDIO_FIFO 1024
#endif
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
/* Reset voice 31 only when the OS PCM ring is within this many pairs of
 * completely empty (a real underrun), never on ordinary mid-frame drain. */
#define OF_UNDERRUN_FREE_GRACE 64

static bool g_inited = false;
static int  g_frameSize = OF_MIX_BLOCK;
/* Real OS PCM ring depth (pairs), measured once at init (Pocket: 2048 / ~42 ms;
 * the OS ring is NOT OF_AUDIO_FIFO=1024 -- that constant is the downstream HW
 * dcfifo). The underrun reset fires only when of_audio_free() is within
 * OF_UNDERRUN_FREE_GRACE of this depth. Fallback used until init measures it. */
static int  g_ring_capacity = OF_AUDIO_FIFO;
static int  g_underrun_reset_free = OF_AUDIO_FIFO;
static bool g_output_suspended = false;
static unsigned g_output_suspend_depth = 0;
static bool g_hw_mixer_inited = false;
static volatile uint32_t g_silence_idle_ticks = 0;
static const uint32_t SILENCE_STALE_TICKS     = 4; /* @60 Hz -> ~67 ms, > 42 ms ring */
/* Track whether the 48 kHz PCM ring has been started through
 * of_audio_write(). Do not call of_audio_stream_open(48000): ScummVM hit
 * audible resonance/vibrato from that path, and the OS already configures
 * voice 31 for 1:1 48 kHz playback on the first of_audio_write(). */
static bool g_voice_open = false;
static bool g_voice_has_written = false;
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
	g_voice_has_written = false;
	g_silence_idle_ticks = 0;
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
static void reset_stale_voice_if_underrun(int &freePairs)
{
	if (!g_voice_has_written) return;
	if (freePairs < g_underrun_reset_free) return;
	/* The OS ring is effectively empty (a true underrun): voice 31 has been
	 * replaying the tail of the last block while the main loop was blocked.
	 * Reset it before queuing fresh PCM so old samples cannot keep looping.
	 * NOTE this is a band-aid: of_audio_init() zeroes the ring (uncached
	 * stores), so it is NOT free -- the real fix is to not underrun (see the
	 * ring-size note in init()). */
	silence_voice_locked();
	ensure_voice_open();
	freePairs = of_audio_free();
}

/* Auto-silence-on-stall: the HW mixer voice loops the 2048-pair audio_ring
 * (~42 ms at 48 kHz) forever once started. When the main thread blocks for
 * longer than that (level loads, MPQ-sector decompression bursts, menu-
 * screen transitions where DevilutionX keeps the music stream alive), the
 * ring keeps replaying its last 42 ms. Fix: a periodic timer that runs in
 * IRQ context and silences the voice when the main thread hasn't pumped
 * in too many ticks. The next of_aulib_pump() from the main loop reopens
 * it via ensure_voice_open().
 *
 * IRQ SAFETY: only volatile uint32 atomic ops + a single function-pointer
 * call into of_audio_stream_close() (which writes MMIO + a couple of OS-
 * side statics). NO ecalls -- of_time_us() is an ecall and re-entering the
 * kernel from inside an interrupt is unsupported. We count ticks instead.
 *
 * RACE SAFETY: prior attempt broke music because main pump's of_audio_write
 * and IRQ's of_audio_stream_close both touch the HW mixer state (write idx,
 * voice control regs) and would interleave. Now serialized via the
 * try_lock/lock spin below: IRQ skips if main is mid-pump, main spins
 * briefly if IRQ is mid-silence. The lock uses RV32A LR/SC via __atomic. */

/* Spinlock between main-thread pump and IRQ-context silence_tick. Both
 * touch the same HW audio device; without serialization, of_audio_write
 * and of_audio_stream_close interleave their MMIO sequences. */
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
		/* Spin. IRQ holding the lock will release in microseconds,
		 * and IRQ can preempt this spin (its handler runs with the
		 * lock taken; it will finish and release before main resumes). */ ;
}
static inline void audio_unlock(void) {
	__atomic_store_n(&g_audio_lock, 0, __ATOMIC_RELEASE);
}

static void of_aulib_silence_tick(void) {
	if (!g_inited || !g_voice_open) return;
	/* __atomic_add_fetch rather than `++` to avoid the C++20 volatile-
	 * increment deprecation and to be explicit about IRQ visibility. */
	if (__atomic_add_fetch(&g_silence_idle_ticks, 1, __ATOMIC_RELAXED)
	    < SILENCE_STALE_TICKS) return;
	/* Main pump holds the lock? Don't fight it; it just refreshed audio.
	 * Resetting g_silence_idle_ticks here would mask a real stall, so we
	 * leave the counter and try again on the next tick. */
	if (!audio_try_lock()) return;
	/* Silence by WRITING ZEROS into the ring, not by close/reopen. The
	 * previous close-on-stall design (of_audio_stream_close + later
	 * of_audio_stream_open) killed music entirely: open re-zeros the ring
	 * AND resets audio_write_idx=0 AND sets MIX_VOICE_POS_WR=0, but by the
	 * time pump calls of_audio_free() the HW POS has already advanced past
	 * 0. write_idx ends up chasing read_pos around the ring, with every
	 * pump's samples written at positions HW just passed -- they never
	 * play. Verified against openfpgaOS audio.c / mixer.c.
	 *
	 * Writing silence under the same lock is safe: of_audio_write in the
	 * OS is pure MMIO (uncached SDRAM stores + write_idx update), no
	 * ecalls, and the spinlock guarantees pump's of_audio_write cannot
	 * interleave. Voice stays continuously active; HW reads silence past
	 * the last real sample instead of looping. */
	{
		static const int16_t silence[128 * 2] = {0};
		int room = of_audio_free();
		while (room > 0) {
			int n = room < 128 ? room : 128;
			int w = of_audio_write(silence, n);
			if (w <= 0) break;
			room -= w;
		}
	}
	g_silence_idle_ticks = 0;
	audio_unlock();
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
	clearReadAhead();
	return true;
}
void Stream::setReadAheadFrames(int frames)
{
	if (frames <= 0) {
		readAhead_.clear();
		readAheadCapacity_ = 0;
		clearReadAhead();
		return;
	}
	readAhead_.assign((size_t)frames * 2u, 0.0F);
	readAheadCapacity_ = frames;
	clearReadAhead();
}
bool Stream::play(int iterations)
{
	if (!decoder_) return false;
	loopForever_ = (iterations == 0);
	playsLeft_ = (iterations == 0) ? 1 : iterations;
	rewind();
	playing_ = true;
	paused_ = false;
	if (readAheadCapacity_ > 0) {
		fillReadAhead(readAheadCapacity_);
		if (readAheadCount_ == 0) {
			playing_ = false;
			return false;
		}
	}
	AddStream(this);
	/* Start the HW voice if this is the first active stream. */
	ensure_voice_open();
	return true;
}
void Stream::stop()
{
	/* Music (read-ahead) streams: fade the HW voice down before it is silenced
	 * so its buffered output drains a ramp, not a click ("plays one sample
	 * over" at e.g. the menu->game music stop). SFX have no read-ahead and are
	 * short, so they stop instantly. Only fade when this is the last active
	 * stream (the voice is about to be silenced) and output isn't suspended. */
	const bool fade = readAheadCapacity_ > 0
	    && ActiveStreams().size() == 1 && ActiveStreams()[0] == this;
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
	if (finishCallback_)
		finishCallback_(*this);
}

bool Stream::nextFrame(float out[2])
{
	if (readAheadCapacity_ > 0) {
#ifndef OF_AULIB_DEFER_DECODE
		/* Default: refill inline if the readahead drained. This can run a
		 * storage-backed MPQ/decoder read while the pump holds the audio lock
		 * (see of_aulib_pump). In practice play() fully primes the buffer and
		 * maintainReadAhead() keeps it topped up, so this rarely fires. */
		if (readAheadCount_ == 0)
			fillReadAhead(OF_READAHEAD_REFILL_FRAMES);
#endif
		/* With OF_AULIB_DEFER_DECODE, never decode here: just pop. An empty
		 * readahead returns false and mixInto() treats it as starvation. */
		return popReadAhead(out);
	}
	return decodeLoopedFrame(out);
}

void Stream::maintainReadAhead()
{
	if (readAheadCapacity_ <= 0)
		return;
	if (!playing_ || paused_)
		return;
	if (readAheadCount_ < readAheadCapacity_)
		fillReadAhead(OF_READAHEAD_REFILL_FRAMES);
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

void Stream::clearReadAhead()
{
	readAheadRead_ = 0;
	readAheadWrite_ = 0;
	readAheadCount_ = 0;
}

bool Stream::fillReadAhead(int maxFrames)
{
	if (readAheadCapacity_ <= 0)
		return true;

	int filled = 0;
	while (readAheadCount_ < readAheadCapacity_
	    && (maxFrames <= 0 || filled < maxFrames)) {
		float frame[2];
		if (!decodeLoopedFrame(frame))
			return false;
		const size_t write = (size_t)readAheadWrite_ * 2u;
		readAhead_[write] = frame[0];
		readAhead_[write + 1] = frame[1];
		readAheadWrite_++;
		if (readAheadWrite_ >= readAheadCapacity_)
			readAheadWrite_ = 0;
		readAheadCount_++;
		filled++;
	}
	return true;
}

bool Stream::popReadAhead(float out[2])
{
	if (readAheadCount_ <= 0)
		return false;
	const size_t read = (size_t)readAheadRead_ * 2u;
	out[0] = readAhead_[read];
	out[1] = readAhead_[read + 1];
	readAheadRead_++;
	if (readAheadRead_ >= readAheadCapacity_)
		readAheadRead_ = 0;
	readAheadCount_--;
	return true;
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

	for (int i = 0; i < frames; ++i) {
		long target = (long)srcPos_;
		while (consumed_ <= target) {
			if (!nextFrame(cur_)) {
#ifdef OF_AULIB_DEFER_DECODE
				/* Readahead (music) path decodes only in maintainReadAhead(),
				 * outside the audio lock. An empty readahead while playing_ is
				 * still set means the decoder hasn't caught up (a storage
				 * stall), NOT end-of-stream: leave the rest of this block as
				 * the silence the pump pre-zeroed, keep srcPos_/consumed_/cur_
				 * where they are so resampling resumes coherently next pump,
				 * and keep the stream alive. True EOS instead clears playing_
				 * inside fillReadAhead()/maintainReadAhead(), and the
				 * !playing_ guard at the top of mixInto erases the stream and
				 * fires the finish callback on the following pump. */
				if (readAheadCapacity_ > 0)
					return true;
#endif
				return false;
			}
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
	/* Measure the real OS ring depth. Right after of_audio_init() the stream
	 * voice is inactive, so of_audio_free() reports the full ring capacity
	 * (2048 pairs / ~42 ms on Pocket; OF_AUDIO_FIFO=1024 is a DIFFERENT thing,
	 * the downstream HW dcfifo). The underrun reset uses this to fire only when
	 * the ring is genuinely near-empty.
	 *
	 * ROOT CAUSE / REAL FIX: music is software-mixed on the MAIN thread into
	 * this ~42 ms ring; whenever the main thread is busy longer than that (a
	 * heavy render frame, an MPQ/bzip2 asset load, level gen) the autonomous HW
	 * mixer runs out of fresh samples and underruns. (SFX don't: they play on
	 * separate autonomous HW mixer voices, fed once.) No app-side reset can win
	 * here -- on the small ring every underrun costs either a replay artifact or
	 * a reset hitch. The durable fix is OS-side: enlarge the stream ring
	 * (targets/pocket: AUDIO_RING_PAIRS + OF_TARGET_AUDIO_STREAM_SIZE) so it
	 * spans the worst main-thread stall; ~170-340 ms removes audible underruns
	 * with no downside (music latency is irrelevant and SFX bypass the ring). */
	g_ring_capacity = of_audio_free();
	if (g_ring_capacity < 256)
		g_ring_capacity = OF_AUDIO_FIFO; /* implausible -> safe fallback */
	g_underrun_reset_free = (g_ring_capacity > OF_UNDERRUN_FREE_GRACE)
	    ? (g_ring_capacity - OF_UNDERRUN_FREE_GRACE)
	    : g_ring_capacity;
	g_voice_open = false;
	g_voice_has_written = false;
	g_output_suspended = false;
	g_output_suspend_depth = 0;
	g_silence_idle_ticks = 0;
	g_inited = true;
	/* IRQ-side silence remains disabled. ScummVM showed of_audio_write()
	 * from the timer ISR can produce audible modulation, so Diablo keeps
	 * all PCM writes on the main thread. */
	(void)of_aulib_silence_tick;
	return true;
}
void quit()
{
	of_timer_stop();
	g_inited = false;
	g_output_suspended = false;
	g_output_suspend_depth = 0;
	g_voice_open = false;
	g_voice_has_written = false;
	g_silence_idle_ticks = 0;
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
	g_voice_has_written = false;
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
	g_silence_idle_ticks = 0;
	audio_unlock();
}

/* ====================================================================== */
/* Pump -- called from the SDL shim's main-loop event/delay path.          */
/* ====================================================================== */
extern "C" void of_aulib_pump(void)
{
	if (!g_inited) return;
	/* Serialize against silence_tick (IRQ). Both touch HW mixer state. */
	audio_lock();
	/* Mark this pump for the silence-on-stall timer. Done before any
	 * early exit so even a no-op pump resets the stall counter. */
	g_silence_idle_ticks = 0;
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
	reset_stale_voice_if_underrun(freePairs);
	while (freePairs > 0) {
		int n = freePairs < OF_MIX_BLOCK ? freePairs : OF_MIX_BLOCK;
		std::memset(acc, 0, sizeof(int32_t) * n * 2);

		auto &v = ActiveStreams();
		for (size_t i = 0; i < v.size();) {
			Aulib::Stream *s = v[i];
			if (!s->mixInto(acc, n)) {
				v.erase(v.begin() + i);
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
		g_voice_has_written = true;
		freePairs -= wrote;
	}
	/* End-of-stream auto-removed the last active stream inside the loop?
	 * Close the voice so the ring isn't left looping its tail samples. */
	close_voice_if_idle();
	audio_unlock();

	/* Refill decoded music after the hardware FIFO has been topped up and
	 * outside the MMIO lock. If a storage read stalls, the HW ring already
	 * has the maximum available headroom instead of underrunning in the
	 * middle of mixInto(). */
	auto &v = ActiveStreams();
	for (Aulib::Stream *s : v)
		s->maintainReadAhead();
}
