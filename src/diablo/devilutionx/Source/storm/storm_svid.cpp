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
OfSVidFrame OfSVidQueue[OfSVidQueueCap];
int ofSVidQHead;
int ofSVidQCount;
bool ofSVidEof;
bool ofSVidSkippedPrev;

void OfSVidQueueReset()
{
	ofSVidQHead = 0;
	ofSVidQCount = 0;
	ofSVidEof = false;
	ofBlitSrc = nullptr;
	ofSVidSkippedPrev = false;
	ofSVidBandFills = 0;
}

void OfSVidQueueFree()
{
	OfSVidQueueReset();
	for (auto &f : OfSVidQueue)
		f.pixels = nullptr;
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

#ifdef OF_PERF_TRACE
	const Uint64 decodeStart = SDL_GetPerformanceCounter();
#endif
	Smacker_GetNextFrame(SVidHandle);
	Smacker_GetFrame(SVidHandle, SVidFrameBuffer.get());
#ifdef OF_PERF_TRACE
	of_perf_add_draw_us(static_cast<unsigned>(SDL_GetPerformanceCounter() - decodeStart));
#endif

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
	{
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
	while (ofSVidQCount < OfSVidQueueCap && OfSVidDecodeIntoQueue()) {
	}
#endif
	SVidFrameEnd = SDL_GetTicks() * 1000.0 + SVidFrameLength;

	return true;
}

bool SVidPlayContinue()
{
#ifdef OPENFPGAOS
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
