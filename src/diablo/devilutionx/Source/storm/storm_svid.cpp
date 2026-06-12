#include "storm/storm_svid.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <SmackerDecoder.h>

#ifndef NOSOUND
#include "utils/push_aulib_decoder.h"
#endif

#include "engine/assets.hpp"
#include "engine/dx.h"
#include "engine/palette.h"
#include "options.h"
#include "utils/aulib.hpp"
#include "utils/display.h"
#include "utils/log.hpp"
#include "utils/sdl_compat.h"
#include "utils/sdl_wrap.h"
#include "utils/stdcompat/optional.hpp"

#ifdef OPENFPGAOS
extern "C" void of_aulib_set_max_buffered_pairs(int pairs);
extern "C" int of_aulib_buffered_pairs(void);
#ifdef OF_PERF_TRACE
// Telemetry: feeds the shim's perf line. The 'draw' column is otherwise
// idle during movies, so it reports per-frame SVid decode time there.
extern "C" void of_perf_add_draw_us(unsigned us);
#endif
#endif

namespace devilution {
namespace {

#ifndef NOSOUND
std::optional<Aulib::Stream> SVidAudioStream;
PushAulibDecoder *SVidAudioDecoder;
std::uint8_t SVidAudioDepth;
std::unique_ptr<int16_t[]> SVidAudioBuffer;
#endif

uint32_t SVidWidth, SVidHeight;
double SVidFrameEnd;
double SVidFrameLength;
bool SVidLoop;
SmackerHandle SVidHandle;
std::unique_ptr<uint8_t[]> SVidFrameBuffer;
SDLPaletteUniquePtr SVidPalette;
SDLSurfaceUniquePtr SVidSurface;

bool IsLandscapeFit(unsigned long srcW, unsigned long srcH, unsigned long dstW, unsigned long dstH)
{
	return srcW * dstH > dstW * srcH;
}

#ifdef USE_SDL1
// Whether we've changed the video mode temporarily for SVid.
// If true, we must restore it once the video has finished playing.
bool IsSVidVideoMode = false;

// Set the video mode close to the SVid resolution while preserving aspect ratio.
void TrySetVideoModeToSVidForSDL1()
{
	const SDL_Surface *display = SDL_GetVideoSurface();
#if defined(SDL1_VIDEO_MODE_SVID_FLAGS)
	const int flags = SDL1_VIDEO_MODE_SVID_FLAGS;
#elif defined(SDL1_VIDEO_MODE_FLAGS)
	const int flags = SDL1_VIDEO_MODE_FLAGS;
#else
	const int flags = display->flags;
#endif
#ifdef SDL1_FORCE_SVID_VIDEO_MODE
	IsSVidVideoMode = true;
#else
	IsSVidVideoMode = (flags & (SDL_FULLSCREEN | SDL_NOFRAME)) != 0;
#endif
	if (!IsSVidVideoMode)
		return;

	int w;
	int h;
	if (IsLandscapeFit(SVidWidth, SVidHeight, display->w, display->h)) {
		w = SVidWidth;
		h = SVidWidth * display->h / display->w;
	} else {
		w = SVidHeight * display->w / display->h;
		h = SVidHeight;
	}

#ifndef SDL1_FORCE_SVID_VIDEO_MODE
	if (!SDL_VideoModeOK(w, h, /*bpp=*/display->format->BitsPerPixel, flags)) {
		IsSVidVideoMode = false;

		// Get available fullscreen/hardware modes
		SDL_Rect **modes = SDL_ListModes(nullptr, flags);

		// Check is there are any modes available.
		if (modes == reinterpret_cast<SDL_Rect **>(0)
		    || modes == reinterpret_cast<SDL_Rect **>(-1)) {
			return;
		}

		// Search for a usable video mode
		bool found = false;
		for (int i = 0; modes[i]; i++) {
			if (modes[i]->w == w || modes[i]->h == h) {
				found = true;
				break;
			}
		}
		if (!found)
			return;
		IsSVidVideoMode = true;
	}
#endif
	SetVideoMode(w, h, display->format->BitsPerPixel, flags);
}
#endif

#ifndef NOSOUND
bool HasAudio()
{
	return SVidAudioStream && SVidAudioStream->isPlaying();
}
#endif

bool SVidLoadNextFrame()
{
	if (Smacker_GetCurrentFrameNum(SVidHandle) >= Smacker_GetNumFrames(SVidHandle)) {
		if (!SVidLoop) {
			return false;
		}

		Smacker_Rewind(SVidHandle);
	}

	SVidFrameEnd += SVidFrameLength;

#if defined(OPENFPGAOS) && defined(OF_PERF_TRACE)
	const Uint64 svidDecodeStart = SDL_GetPerformanceCounter();
#endif
	Smacker_GetNextFrame(SVidHandle);
	Smacker_GetFrame(SVidHandle, SVidFrameBuffer.get());
#if defined(OPENFPGAOS) && defined(OF_PERF_TRACE)
	of_perf_add_draw_us((unsigned)(SDL_GetPerformanceCounter() - svidDecodeStart));
#endif

	return true;
}

void UpdatePalette()
{
	constexpr size_t NumColors = 256;
	uint8_t paletteData[NumColors * 3];
	Smacker_GetPalette(SVidHandle, paletteData);

	SDL_Color *colors = SVidPalette->colors;
	for (unsigned i = 0; i < NumColors; ++i) {
		colors[i].r = paletteData[i * 3];
		colors[i].g = paletteData[i * 3 + 1];
		colors[i].b = paletteData[i * 3 + 2];
#ifndef USE_SDL1
		colors[i].a = SDL_ALPHA_OPAQUE;
#endif
	}

#ifdef USE_SDL1
#if SDL1_VIDEO_MODE_BPP == 8
	// When the video surface is 8bit, we need to set the output palette.
	SDL_SetColors(SDL_GetVideoSurface(), colors, 0, NumColors);
#endif
	if (SDL_SetPalette(SVidSurface.get(), SDL_LOGPAL, colors, 0, NumColors) <= 0) {
		ErrSdl();
	}
#else
	if (SDL_SetSurfacePalette(SVidSurface.get(), SVidPalette.get()) <= -1) {
		ErrSdl();
	}
#endif
#ifdef OPENFPGAOS
	// Direct-to-framebuffer path: the 8-bit blit below copies PIXELS only,
	// and the shim's hardware-palette push is keyed to the OUTPUT surface
	// palette's version. Write the movie palette into it via
	// SDL_SetPaletteColors (which bumps the version) or movies render in
	// whatever palette the menu/game left in the hardware.
	SDL_SetPaletteColors(GetOutputSurface()->format->palette, colors, 0, NumColors);
#endif
}

#ifdef OPENFPGAOS
// When set, the OPENFPGAOS blit path below reads these raw pixels (pitch ==
// SVidWidth) instead of SVidSurface -- used by the decoded-frame queue.
const uint8_t *ofBlitSrc;
// Letterbox bands only need painting once per triple-buffer page (3 pages);
// they are static for the whole movie. Saves ~1-2 ms on every later present.
int ofSVidBandFills;
#endif

bool BlitFrame()
{
#ifndef USE_SDL1
	if (renderer != nullptr) {
		if (SDL_BlitSurface(SVidSurface.get(), nullptr, GetOutputSurface(), nullptr) <= -1) {
			Log("{}", SDL_GetError());
			return false;
		}
	} else
#endif
	{
		SDL_Surface *outputSurface = GetOutputSurface();
#ifdef OPENFPGAOS
		// 8-bit direct-to-framebuffer path. Two Pocket specifics:
		// (1) the output surface aliases a rotating OS triple-buffer page,
		//     so the letterbox area must be cleared EVERY frame or two of
		//     the three pages keep stale menu/game pixels around the video;
		// (2) upstream refuses to scale indexed output, but our surfaces are
		//     all 8-bit INDEX8, so a dedicated pixel-doubling loop upscales
		//     320x156 -> 640x312 (SDL_BlitScaled's per-pixel 64-bit math is
		//     far too slow here, and palette indices scale losslessly).
		if (outputSurface->w >= (int)(SVidWidth * 2) && outputSurface->h >= (int)(SVidHeight * 2)) {
			const int dw = (int)SVidWidth * 2, dh = (int)SVidHeight * 2;
			const int dx = (outputSurface->w - dw) / 2, dy = (outputSurface->h - dh) / 2;
			if (ofSVidBandFills < 3) {
				// One clear per triple-buffer page; static afterwards.
				ofSVidBandFills++;
				SDL_Rect top { 0, 0, outputSurface->w, dy };
				SDL_Rect bottom { 0, dy + dh, outputSurface->w, outputSurface->h - dy - dh };
				SDL_Rect left { 0, dy, dx, dh };
				SDL_Rect right { dx + dw, dy, outputSurface->w - dx - dw, dh };
				if (top.h > 0) SDL_FillRect(outputSurface, &top, 0);
				if (bottom.h > 0) SDL_FillRect(outputSurface, &bottom, 0);
				if (left.w > 0) SDL_FillRect(outputSurface, &left, 0);
				if (right.w > 0) SDL_FillRect(outputSurface, &right, 0);
			}
			const auto *src = static_cast<const uint8_t *>(ofBlitSrc != nullptr ? ofBlitSrc : SVidSurface->pixels);
			const int spitch = ofBlitSrc != nullptr ? static_cast<int>(SVidWidth) : SVidSurface->pitch;
			auto *dbase = static_cast<uint8_t *>(outputSurface->pixels)
			    + static_cast<size_t>(dy) * outputSurface->pitch + dx;
			for (uint32_t y = 0; y < SVidHeight; ++y) {
				uint8_t *d0 = dbase + static_cast<size_t>(y) * 2 * outputSurface->pitch;
				const uint8_t *srow = src + static_cast<size_t>(y) * spitch;
				for (uint32_t x = 0; x < SVidWidth; ++x) {
					const uint8_t px = srow[x];
					d0[x * 2] = px;
					d0[x * 2 + 1] = px;
				}
				std::memcpy(d0 + outputSurface->pitch, d0, static_cast<size_t>(dw));
			}
		} else {
			SDL_FillRect(outputSurface, nullptr, 0);
			SDL_Rect center;
			center.w = static_cast<int>(SVidWidth);
			center.h = static_cast<int>(SVidHeight);
			center.x = (outputSurface->w - center.w) / 2;
			center.y = (outputSurface->h - center.h) / 2;
			if (ofBlitSrc != nullptr) {
				// Queue path: SVidSurface wraps the LIVE decoder buffer
				// (up to 3 frames ahead of the presented one) -- copy the
				// queued snapshot directly instead.
				auto *dst = static_cast<uint8_t *>(outputSurface->pixels)
				    + static_cast<size_t>(center.y) * outputSurface->pitch + center.x;
				for (uint32_t y = 0; y < SVidHeight; ++y)
					std::memcpy(dst + static_cast<size_t>(y) * outputSurface->pitch,
					    ofBlitSrc + static_cast<size_t>(y) * SVidWidth, SVidWidth);
			} else if (SDL_BlitSurface(SVidSurface.get(), nullptr, outputSurface, &center) <= -1) {
				ErrSdl();
			}
		}
#else
#ifdef USE_SDL1
		const bool isIndexedOutputFormat = SDLBackport_IsPixelFormatIndexed(outputSurface->format);
#else
		const Uint32 wndFormat = SDL_GetWindowPixelFormat(ghMainWnd);
		const bool isIndexedOutputFormat = SDL_ISPIXELFORMAT_INDEXED(wndFormat);
#endif
		SDL_Rect outputRect;
		if (isIndexedOutputFormat) {
			// Cannot scale if the output format is indexed (8-bit palette).
			outputRect.w = static_cast<int>(SVidWidth);
			outputRect.h = static_cast<int>(SVidHeight);
		} else if (IsLandscapeFit(SVidWidth, SVidHeight, outputSurface->w, outputSurface->h)) {
			outputRect.w = outputSurface->w;
			outputRect.h = SVidHeight * outputSurface->w / SVidWidth;
		} else {
			outputRect.w = SVidWidth * outputSurface->h / SVidHeight;
			outputRect.h = outputSurface->h;
		}
		outputRect.x = (outputSurface->w - outputRect.w) / 2;
		outputRect.y = (outputSurface->h - outputRect.h) / 2;

		if (isIndexedOutputFormat
		    || outputSurface->w == static_cast<int>(SVidWidth)
		    || outputSurface->h == static_cast<int>(SVidHeight)) {
			if (SDL_BlitSurface(SVidSurface.get(), nullptr, outputSurface, &outputRect) <= -1) {
				ErrSdl();
			}
		} else {
			// The source surface is always 8-bit, and the output surface is never 8-bit in this branch.
			// We must convert to the output format before calling SDL_BlitScaled.
#ifdef USE_SDL1
			SDLSurfaceUniquePtr converted = SDLWrap::ConvertSurface(SVidSurface.get(), ghMainWnd->format, 0);
#else
			SDLSurfaceUniquePtr converted = SDLWrap::ConvertSurfaceFormat(SVidSurface.get(), wndFormat, 0);
#endif
			if (SDL_BlitScaled(converted.get(), nullptr, outputSurface, &outputRect) <= -1) {
				Log("{}", SDL_GetError());
				return false;
			}
		}
#endif /* !OPENFPGAOS */
	}

	RenderPresent();
	return true;
}

} // namespace

#ifdef OPENFPGAOS
/* Decoded-frame queue (single-threaded decode-ahead).
 *
 * Why: movie audio is pushed at DECODE time, one frame's worth per frame.
 * With the upstream loop (decode -> present -> sleep) the supply rate
 * exactly equals the 22050 Hz consumption rate, so the ring cushion never
 * grows past ~1 frame regardless of the buffering cap -- any stretch that
 * decodes slightly slower than realtime underruns audibly. Decoding AHEAD
 * during the pacing slack banks several frames of audio in the ring, so a
 * slow (keyframe) stretch stalls only the picture briefly while audio
 * rides the cushion. Presents still happen on schedule from the queue.
 * (This cannot smooth VIDEO -- present and decode share the single core --
 * it exists for the audio.) */
struct OfSVidFrame {
	std::unique_ptr<uint8_t[]> pixels; // SVidWidth * SVidHeight, pitch == width
	uint8_t palette[768];
	bool paletteChanged;
};
constexpr int OfSVidQueueCap = 6;
// Audio-priority playback for movies too heavy to decode at realtime:
// soundtrack decodes fully (~1 ms/frame), picture updates at keyframes
// only (the decoder byte-skips other video chunks). See SVidPlayBegin.
bool ofSVidAudioOnly;
bool ofSVidAudioOnlyPresentPending;
// Measured decode feasibility (EMA of per-frame decode us, alpha=1/4).
// Resolution alone cannot predict cost -- the Hellfire intro is 320x240
// (under any sane pixel gate) yet decodes at ~150 ms/frame because cost
// scales with SYMBOL density, not pixels. Measure and switch instead.
uint32_t ofSVidDecodeEmaUs;
uint32_t ofSVidDecodeCount;
// Audio-priority picture cadence: frames since the last forced video decode.
uint32_t ofSVidFramesSinceVideo;
OfSVidFrame OfSVidQueue[OfSVidQueueCap];
int ofSVidQHead;
int ofSVidQCount;
bool ofSVidEof;
bool ofSVidSkippedPrev;

void OfSVidQueueReset()
{
	/* NOTE: ofSVidAudioOnly is NOT reset here -- the feasibility gate in
	 * SVidPlayBegin sets it before this runs; it is cleared at the gate
	 * itself and in OfSVidQueueFree. */
	ofSVidQHead = 0;
	ofSVidQCount = 0;
	ofSVidEof = false;
	ofSVidDecodeEmaUs = 0;
	ofSVidDecodeCount = 0;
	ofSVidFramesSinceVideo = 0;
	ofBlitSrc = nullptr;
	ofSVidSkippedPrev = false;
	ofSVidBandFills = 0;
}

void OfSVidQueueFree()
{
	OfSVidQueueReset();
	ofSVidAudioOnly = false;
	ofSVidAudioOnlyPresentPending = false;
	for (auto &f : OfSVidQueue)
		f.pixels = nullptr;
}

// Switch to audio-priority playback (soundtrack + keyframes only). Safe
// mid-movie: skipping starts after a fully decoded frame, and the next
// decoded frame is a keyframe, so the held picture is never corrupt.
void OfSVidEngageAudioOnly(const char *why)
{
	if (ofSVidAudioOnly)
		return;
	Log("SVid: audio-priority (keyframe) playback engaged ({})", why);
	ofSVidAudioOnly = true;
	Smacker_SetVideoKeyframesOnly(SVidHandle, 1);
	of_aulib_set_max_buffered_pairs(16384);
	// Queued frames are left alone (the audio-only path ignores them and
	// OfSVidQueueFree releases them at movie end); clearing them here
	// mid-fill-loop would invalidate the caller's slot indexing.
}

// Decode one frame into the queue tail (audio pushed NOW = decode time).
// Returns false at end-of-stream (non-looping).
bool OfSVidDecodeIntoQueue()
{
	if (ofSVidEof || ofSVidQCount >= OfSVidQueueCap)
		return !ofSVidEof;
	if (Smacker_GetCurrentFrameNum(SVidHandle) >= Smacker_GetNumFrames(SVidHandle)) {
		if (!SVidLoop) {
			ofSVidEof = true;
			return false;
		}
		Smacker_Rewind(SVidHandle);
	}

	const Uint64 decodeStart = SDL_GetPerformanceCounter();
	Smacker_GetNextFrame(SVidHandle);
	Smacker_GetFrame(SVidHandle, SVidFrameBuffer.get());
	const uint32_t decodeUs = static_cast<uint32_t>(SDL_GetPerformanceCounter() - decodeStart);
#ifdef OF_PERF_TRACE
	of_perf_add_draw_us(decodeUs);
#endif
	ofSVidDecodeEmaUs = ofSVidDecodeCount == 0 ? decodeUs : (ofSVidDecodeEmaUs * 3 + decodeUs) / 4;
	ofSVidDecodeCount++;
	// Engage audio-priority on either of two signals (fall through after
	// engaging: this frame still queues and its audio still pushes):
	// (a) HOPELESS: decode >= 2x the frame budget -- no cushion survives
	//     that (Hellfire-class, ~150 ms vs 66).
	// (b) SYMPTOM: the audio cushion is actually draining (< ~60 ms)
	//     while decode runs over budget. Diablo's own movies burst to
	//     1.4-1.8x at scene cuts but their cushion never drops below
	//     ~99 ms -- a cost threshold alone false-triggered on them and
	//     stuck them in slideshow mode (one-way at the time).
	if (!ofSVidAudioOnly) {
		const uint32_t budgetUs = static_cast<uint32_t>(SVidFrameLength / 1000.0) * 1000u;
		if (ofSVidDecodeCount >= 4 && ofSVidDecodeEmaUs > budgetUs * 2u)
			OfSVidEngageAudioOnly("decode hopeless");
		else if (ofSVidDecodeCount >= 8
		    && ofSVidDecodeEmaUs > budgetUs + budgetUs / 8u
		    && of_aulib_buffered_pairs() < 2880 /* ~60 ms */)
			OfSVidEngageAudioOnly("audio cushion draining");
	}

	OfSVidFrame &slot = OfSVidQueue[(ofSVidQHead + ofSVidQCount) % OfSVidQueueCap];
	if (!slot.pixels)
		slot.pixels = std::unique_ptr<uint8_t[]> { new uint8_t[static_cast<size_t>(SVidWidth * SVidHeight)] };
	std::memcpy(slot.pixels.get(), SVidFrameBuffer.get(), static_cast<size_t>(SVidWidth * SVidHeight));
	slot.paletteChanged = Smacker_DidPaletteChange(SVidHandle);
	if (slot.paletteChanged)
		Smacker_GetPalette(SVidHandle, slot.palette);

#ifndef NOSOUND
	if (HasAudio()) {
		std::int16_t *buf = SVidAudioBuffer.get();
		const auto len = Smacker_GetAudioData(SVidHandle, 0, buf);
		if (SVidAudioDepth == 16) {
			SVidAudioDecoder->PushSamples(buf, len / 2);
		} else {
			SVidAudioDecoder->PushSamples(reinterpret_cast<const std::uint8_t *>(buf), len);
		}
	}
#endif

	ofSVidQCount++;
	return true;
}

#ifdef OPENFPGAOS
// One frame step in audio-priority mode: parse the frame, push its audio,
// decode pixels only if the decoder chose to (keyframe). Returns false at
// end of stream. Sets freshPicture when SVidFrameBuffer has new pixels.
bool OfSVidAudioOnlyStep(bool &freshPicture)
{
	if (Smacker_GetCurrentFrameNum(SVidHandle) >= Smacker_GetNumFrames(SVidHandle)) {
		if (!SVidLoop)
			return false;
		Smacker_Rewind(SVidHandle);
	}
	// These movies carry NO keyframe flags (measured: zero across the whole
	// corpus), so the picture would never refresh on its own. Force a video
	// decode every K frames; the delta lands on a slightly stale canvas --
	// static blocks were static anyway, moving blocks are re-coded fresh --
	// so the cost is bounded ghosting that self-heals at scene cuts.
	// AUDIO RULES: a forced decode stalls audio processing for ~ema us, so
	// it must (a) keep a <=40% duty cycle (K from ema/26 ms, min 3) and
	// (b) only happen when the ring has banked at least 1.5x the expected
	// stall -- the picture is strictly subordinate to the soundtrack.
	{
		uint32_t k = ofSVidDecodeEmaUs / 26000u;
		if (k < 3) k = 3;
		const uint32_t needPairs = ofSVidDecodeEmaUs * 48u / 1000u * 3u / 2u;
		if (ofSVidFramesSinceVideo >= k
		    && static_cast<uint32_t>(of_aulib_buffered_pairs()) >= needPairs)
			Smacker_ForceNextVideoDecode(SVidHandle);
	}
	const Uint64 ofStepStart = SDL_GetPerformanceCounter();
	Smacker_GetNextFrame(SVidHandle);
	if (Smacker_DidDecodeVideo(SVidHandle)) {
		// Keep the cost estimate live: the ema was learned on the movie's
		// first frames (often cheap fade-ins) and would otherwise stay
		// frozen while real frames cost 2x more.
		const uint32_t decodeUs = static_cast<uint32_t>(SDL_GetPerformanceCounter() - ofStepStart);
		ofSVidDecodeEmaUs = (ofSVidDecodeEmaUs * 3 + decodeUs) / 4;
#ifdef OF_PERF_TRACE
		of_perf_add_draw_us(decodeUs);
#endif
		Smacker_GetFrame(SVidHandle, SVidFrameBuffer.get());
		freshPicture = true;
		ofSVidFramesSinceVideo = 0;
		// Self-correct a false engagement: if forced decodes now run well
		// under budget and the cushion is healthy, return to full-rate
		// playback (the next deltas land on the held canvas and self-heal
		// exactly like the forced-decode path). Hysteresis vs the engage
		// thresholds prevents flapping.
		if (ofSVidDecodeEmaUs < static_cast<uint32_t>(SVidFrameLength * 0.85 / 1000.0) * 1000u
		    && of_aulib_buffered_pairs() > 7200 /* ~150 ms */) {
			Log("SVid: decode recovered; resuming full-rate playback");
			ofSVidAudioOnly = false;
			Smacker_SetVideoKeyframesOnly(SVidHandle, 0);
			of_aulib_set_max_buffered_pairs(12288);
			ofSVidQHead = 0;
			ofSVidQCount = 0;
			ofSVidAudioOnlyPresentPending = false;
			SVidFrameEnd = SDL_GetTicks() * 1000.0 + SVidFrameLength;
		}
	} else {
		ofSVidFramesSinceVideo++;
	}
#ifndef NOSOUND
	if (HasAudio()) {
		std::int16_t *buf = SVidAudioBuffer.get();
		const auto len = Smacker_GetAudioData(SVidHandle, 0, buf);
		if (SVidAudioDepth == 16)
			SVidAudioDecoder->PushSamples(buf, len / 2);
		else
			SVidAudioDecoder->PushSamples(reinterpret_cast<const std::uint8_t *>(buf), len);
	}
#endif
	return true;
}
#endif

// Apply a queued frame's palette snapshot to the output surface so the
// present pushes it to the hardware WITH the matching pixels. (The live
// decoder palette belongs to the newest DECODED frame, which is ahead of
// the presented one -- UpdatePalette() must not be used here.)
void OfSVidApplyQueuedPalette(const OfSVidFrame &frame)
{
	SDL_Color colors[256];
	for (unsigned i = 0; i < 256; ++i) {
		colors[i].r = frame.palette[i * 3];
		colors[i].g = frame.palette[i * 3 + 1];
		colors[i].b = frame.palette[i * 3 + 2];
		colors[i].a = SDL_ALPHA_OPAQUE;
	}
	SDL_SetPaletteColors(GetOutputSurface()->format->palette, colors, 0, 256);
}
#endif

bool SVidPlayBegin(const char *filename, int flags)
{
	if ((flags & 0x10000) != 0 || (flags & 0x20000000) != 0) {
		return false;
	}

	SVidLoop = false;
	if ((flags & 0x40000) != 0)
		SVidLoop = true;
	// 0x8 // Non-interlaced
	// 0x200, 0x800 // Upscale video
	// 0x80000 // Center horizontally
	// 0x100000 // Disable video
	// 0x800000 // Edge detection
	// 0x200800 // Clear FB

	SDL_RWops *videoStream = OpenAssetAsSdlRwOps(filename);
	SVidHandle = Smacker_Open(videoStream);
	if (!SVidHandle.isValid) {
		return false;
	}

#ifndef NOSOUND
	const bool enableAudio = (flags & 0x1000000) == 0;

	auto audioInfo = Smacker_GetAudioTrackDetails(SVidHandle, 0);
	LogVerbose(LogCategory::Audio, "SVid audio depth={} channels={} rate={}", audioInfo.bitsPerSample, audioInfo.nChannels, audioInfo.sampleRate);

	if (enableAudio && audioInfo.bitsPerSample != 0) {
		sound_stop(); // Stop in-progress music and sound effects

		SVidAudioDepth = audioInfo.bitsPerSample;
		SVidAudioBuffer = std::unique_ptr<int16_t[]> { new int16_t[audioInfo.idealBufferSize] };
		auto decoder = std::make_unique<PushAulibDecoder>(audioInfo.nChannels, audioInfo.sampleRate);
		SVidAudioDecoder = decoder.get();
		SVidAudioStream.emplace(/*rwops=*/nullptr, std::move(decoder), CreateAulibResampler(audioInfo.sampleRate), /*closeRw=*/false);
		const float volume = static_cast<float>(*sgOptions.Audio.soundVolume - VOLUME_MIN) / -VOLUME_MIN;
		SVidAudioStream->setVolume(volume);
		if (!diablo_is_focused())
			SVidMute();
		if (!SVidAudioStream->open()) {
			LogError(LogCategory::Audio, "Aulib::Stream::open (from SVidPlayBegin): {}", SDL_GetError());
			SVidAudioStream = std::nullopt;
			SVidAudioDecoder = nullptr;
		}
		// (guarded: operator-> on the disengaged optional after an open()
		// failure is UB, and this build has no exceptions to catch it)
		if (SVidAudioStream && !SVidAudioStream->play()) {
			LogError(LogCategory::Audio, "Aulib::Stream::play (from SVidPlayBegin): {}", SDL_GetError());
			SVidAudioStream = std::nullopt;
			SVidAudioDecoder = nullptr;
		}
#ifdef OPENFPGAOS
		if (SVidAudioStream) {
			// Push-fed stream: the decoder zero-fills when its queue is
			// empty, so an uncapped pump fills the whole ~2.7 s OS ring
			// with silence and every frame's audio lands that far behind
			// the video. ~256 ms balances lip-sync against underruns when
			// a heavy (keyframe) stretch briefly decodes slower than
			// realtime and the per-frame audio supply lags.
			of_aulib_set_max_buffered_pairs(12288);
		}
#endif
	}
#endif

	SVidFrameLength = 1000000.0 / Smacker_GetFrameRate(SVidHandle);
	Smacker_GetFrameSize(SVidHandle, SVidWidth, SVidHeight);

#ifdef OPENFPGAOS
	// Decode-feasibility gate. Smacker is delta-coded (every frame must be
	// decoded -- frame skipping is impossible) and its audio is interleaved
	// per frame, so a movie that cannot decode video at realtime cannot
	// play normally at 100 MHz. Diablo's movies are all 320x156 (measured
	// 18-54 ms/frame); Hellfire ships higher-resolution videos that
	// measured ~130 ms/frame on HW. Those switch to AUDIO-PRIORITY mode:
	// the soundtrack plays perfectly (audio chunks are self-contained and
	// cost ~1 ms/frame) while the picture updates only at keyframes --
	// scene cuts -- with non-keyframe video chunks byte-skipped unread.
	ofSVidAudioOnly = false;
	if (static_cast<uint32_t>(SVidWidth) * SVidHeight > 80000u) {
		// Clearly infeasible by size alone; smaller-but-dense movies are
		// caught by the measured-decode-rate switch in OfSVidDecodeIntoQueue.
		Log("SVid: {}x{} movie", SVidWidth, SVidHeight);
		OfSVidEngageAudioOnly("frame size");
	}
#endif

#ifndef USE_SDL1
	if (renderer != nullptr) {
		int renderWidth = static_cast<int>(SVidWidth);
		int renderHeight = static_cast<int>(SVidHeight);
		texture = SDLWrap::CreateTexture(renderer, DEVILUTIONX_DISPLAY_TEXTURE_FORMAT, SDL_TEXTUREACCESS_STREAMING, renderWidth, renderHeight);
		if (SDL_RenderSetLogicalSize(renderer, renderWidth, renderHeight) <= -1) {
			ErrSdl();
		}
	}
#else
	TrySetVideoModeToSVidForSDL1();
#endif

	// Set the background to black.
	SDL_FillRect(GetOutputSurface(), nullptr, 0x000000);

	// The buffer for the frame. It is not the same as the SDL surface because the SDL surface also has pitch padding.
	SVidFrameBuffer = std::unique_ptr<uint8_t[]> { new uint8_t[static_cast<size_t>(SVidWidth * SVidHeight)] };

	// Decode first frame.
	Smacker_GetNextFrame(SVidHandle);
	Smacker_GetFrame(SVidHandle, SVidFrameBuffer.get());

	// Create the surface from the frame buffer data.
	// It will be rendered in `SVidPlayContinue`, called immediately after this function.
	// Subsequents frames will also be copied to this surface.
	SVidSurface = SDLWrap::CreateRGBSurfaceWithFormatFrom(
	    reinterpret_cast<void *>(SVidFrameBuffer.get()),
	    static_cast<int>(SVidWidth),
	    static_cast<int>(SVidHeight),
	    8,
	    static_cast<int>(SVidWidth),
	    SDL_PIXELFORMAT_INDEX8);

	SVidPalette = SDLWrap::AllocPalette();
	UpdatePalette();

#ifdef OPENFPGAOS
	OfSVidQueueReset();
	// Seed the queue with the frame decoded just above; otherwise the first
	// OfSVidDecodeIntoQueue advances to frame 2 before snapshotting and
	// frame 1's pixels AND audio (overwritten by the decoder's next
	// ReadPacket) are silently dropped.
	if (!ofSVidAudioOnly) {
		OfSVidFrame &slot = OfSVidQueue[0];
		if (!slot.pixels)
			slot.pixels = std::unique_ptr<uint8_t[]> { new uint8_t[static_cast<size_t>(SVidWidth * SVidHeight)] };
		std::memcpy(slot.pixels.get(), SVidFrameBuffer.get(), static_cast<size_t>(SVidWidth * SVidHeight));
		// Consume the decoder's destructive flag so frame 2's slot doesn't
		// inherit frame 1's palette-change.
		slot.paletteChanged = Smacker_DidPaletteChange(SVidHandle);
		if (slot.paletteChanged)
			Smacker_GetPalette(SVidHandle, slot.palette);
#ifndef NOSOUND
		if (HasAudio()) {
			std::int16_t *buf = SVidAudioBuffer.get();
			const auto len = Smacker_GetAudioData(SVidHandle, 0, buf);
			if (SVidAudioDepth == 16)
				SVidAudioDecoder->PushSamples(buf, len / 2);
			else
				SVidAudioDecoder->PushSamples(reinterpret_cast<const std::uint8_t *>(buf), len);
		}
#endif
		ofSVidQCount = 1;
	}
	// Prefill the rest of the queue BEFORE starting the present clock: this
	// banks the audio cushion (the whole point of the queue) as ~150 ms of
	// movie-start latency instead of late presents during the first frames.
	while (!ofSVidAudioOnly && ofSVidQCount < OfSVidQueueCap && OfSVidDecodeIntoQueue()) {
	}
	if (ofSVidAudioOnly) {
		// Audio-priority setup -- reached either via the size gate (queue
		// untouched) or via mid-prefill engagement (some frames queued;
		// their audio is already pushed and their pictures are ignored).
		// The latest decoded picture presents on the first Continue; then
		// bank ~0.5 s of soundtrack (skipped frames cost ~1-2 ms each)
		// with a deeper ring cushion so a 150-300 ms forced decode cannot
		// drain it mid-scene.
		of_aulib_set_max_buffered_pairs(16384);
		ofSVidAudioOnlyPresentPending = true;
#ifndef NOSOUND
		// Only the size-gate path skipped the queue seed; its frame-1
		// audio was never pushed. Mid-prefill engagement already pushed
		// every processed frame's audio -- pushing again would duplicate.
		if (ofSVidQCount == 0 && HasAudio()) {
			std::int16_t *buf = SVidAudioBuffer.get();
			const auto len = Smacker_GetAudioData(SVidHandle, 0, buf);
			if (SVidAudioDepth == 16)
				SVidAudioDecoder->PushSamples(buf, len / 2);
			else
				SVidAudioDecoder->PushSamples(reinterpret_cast<const std::uint8_t *>(buf), len);
		}
#endif
		bool fresh = false;
		for (int i = 0; i < 8; ++i) {
			if (!OfSVidAudioOnlyStep(fresh))
				break;
		}
	}
#endif
	SVidFrameEnd = SDL_GetTicks() * 1000.0 + SVidFrameLength;

	return true;
}

bool SVidPlayContinue()
{
#ifdef OPENFPGAOS
	if (ofSVidAudioOnly) {
		// Pace by the audio clock: process every frame due by now (audio
		// parse is ~1-2 ms; only keyframes cost a real decode). Present
		// only when fresh pixels arrived; otherwise just keep pumping.
		bool freshPicture = ofSVidAudioOnlyPresentPending;
		ofSVidAudioOnlyPresentPending = false;
		double now = SDL_GetTicks() * 1000.0;
		while (now >= SVidFrameEnd) {
			if (!OfSVidAudioOnlyStep(freshPicture))
				return false; // end of movie
			SVidFrameEnd += SVidFrameLength;
			now = SDL_GetTicks() * 1000.0;
		}
		if (freshPicture) {
			if (Smacker_DidPaletteChange(SVidHandle))
				UpdatePalette();
			if (!BlitFrame())
				return false;
		} else {
			SDL_Delay(1); // audio pump tick
		}
		return true;
	}

	// Keep the queue topped up: always at least one frame, and use the
	// schedule slack (4 ms margin) to run ahead -- this is what banks the
	// audio cushion. A slow decode here makes the next present late; the
	// resync below recovers, exactly as before.
	while (ofSVidQCount < OfSVidQueueCap && !ofSVidEof) {
		if (ofSVidQCount > 0 && SDL_GetTicks() * 1000.0 + 4000.0 >= SVidFrameEnd)
			break;
		if (!OfSVidDecodeIntoQueue())
			break;
	}
	if (ofSVidQCount == 0)
		return false; // end of movie

	double now = SDL_GetTicks() * 1000.0;
	if (now < SVidFrameEnd)
		SDL_Delay(static_cast<Uint32>((SVidFrameEnd - now) / 1000.0));

	OfSVidFrame &frame = OfSVidQueue[ofSVidQHead];
	// Sustained decode slightly over the 66.7 ms budget starves the AUDIO
	// (its supply is locked to frames decoded; the push decoder zero-fills
	// the deficit = audible stutter while the ring metric reads full).
	// When late, drop this frame's PICTURE -- its audio was already pushed
	// at decode time -- so the saved blit/flip time lets decode catch up.
	// Bounded to alternate frames; the palette must still apply so later
	// frames don't present under a stale one. Resync only when even
	// skipping cannot keep up (> 2 frames behind).
	const bool skipLate = !ofSVidSkippedPrev
	    && now - SVidFrameEnd > SVidFrameLength * 0.75;
	if (!skipLate && now - SVidFrameEnd > 2 * SVidFrameLength)
		SVidFrameEnd = now;
	if (frame.paletteChanged)
		OfSVidApplyQueuedPalette(frame);
	bool blitOk = true;
	if (skipLate) {
		ofSVidSkippedPrev = true;
	} else {
		ofSVidSkippedPrev = false;
		ofBlitSrc = frame.pixels.get();
		blitOk = BlitFrame();
		ofBlitSrc = nullptr;
	}
	ofSVidQHead = (ofSVidQHead + 1) % OfSVidQueueCap;
	ofSVidQCount--;
	SVidFrameEnd += SVidFrameLength;
	return blitOk;
#else
	if (Smacker_DidPaletteChange(SVidHandle)) {
		UpdatePalette();
	}

	if (SDL_GetTicks() * 1000.0 >= SVidFrameEnd) {
		return SVidLoadNextFrame(); // Skip video and audio if the system is to slow
	}

#ifndef NOSOUND
	if (HasAudio()) {
		std::int16_t *buf = SVidAudioBuffer.get();
		const auto len = Smacker_GetAudioData(SVidHandle, 0, buf);
		if (SVidAudioDepth == 16) {
			SVidAudioDecoder->PushSamples(buf, len / 2);
		} else {
			SVidAudioDecoder->PushSamples(reinterpret_cast<const std::uint8_t *>(buf), len);
		}
	}
#endif

	if (SDL_GetTicks() * 1000.0 >= SVidFrameEnd) {
		return SVidLoadNextFrame(); // Skip video if the system is to slow
	}

	if (!BlitFrame())
		return false;

	double now = SDL_GetTicks() * 1000.0;
	if (now < SVidFrameEnd) {
		SDL_Delay(static_cast<Uint32>((SVidFrameEnd - now) / 1000.0)); // wait with next frame if the system is too fast
	}

	return SVidLoadNextFrame();
#endif
}

void SVidPlayEnd()
{
#ifndef NOSOUND
	if (HasAudio()) {
		SVidAudioStream = std::nullopt;
		SVidAudioDecoder = nullptr;
		SVidAudioBuffer = nullptr;
	}
#endif
#ifdef OPENFPGAOS
	// Outside the NOSOUND region: the queue is VIDEO state (pixel buffers
	// sized per movie -- a NOSOUND build reusing stale smaller buffers
	// would overflow). Cap reset unconditional: a leaked cap would
	// silently strip music's deep load-coast buffering for the session.
	of_aulib_set_max_buffered_pairs(0);
	OfSVidQueueFree();
#endif

	if (SVidHandle.isValid)
		Smacker_Close(SVidHandle);

	SVidPalette = nullptr;
	SVidSurface = nullptr;
	SVidFrameBuffer = nullptr;

#ifndef USE_SDL1
	if (renderer != nullptr) {
		texture = SDLWrap::CreateTexture(renderer, DEVILUTIONX_DISPLAY_TEXTURE_FORMAT, SDL_TEXTUREACCESS_STREAMING, gnScreenWidth, gnScreenHeight);
		if (renderer != nullptr && SDL_RenderSetLogicalSize(renderer, gnScreenWidth, gnScreenHeight) <= -1) {
			ErrSdl();
		}
	}
#else
	if (IsSVidVideoMode) {
		SetVideoModeToPrimary(IsFullScreen(), gnScreenWidth, gnScreenHeight);
		IsSVidVideoMode = false;
	}
#endif
}

void SVidMute()
{
#ifndef NOSOUND
	if (SVidAudioStream)
		SVidAudioStream->mute();
#endif
}

void SVidUnmute()
{
#ifndef NOSOUND
	if (SVidAudioStream)
		SVidAudioStream->unmute();
#endif
}

} // namespace devilution
