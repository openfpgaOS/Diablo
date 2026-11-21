// Aulib shim pump: the 16-bit fast path (single-stream direct render and the
// integer-held accumulate path) must produce exactly the int16 output of the
// float nearest-neighbor mixer it replaced. The reference below is that float
// mixer (decode to s * 2^-15, float source position, (int32) of sample *
// volume * 32767, sum, clamp), driven over the same pump block sizes.
#include <Aulib/DecoderDrwav.h>
#include <Aulib/Stream.h>
#include <SDL.h>
#include <aulib.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::exit(1); } } while (0)

extern "C" void of_aulib_pump(void);

namespace {
std::vector<int16_t> written;
int ringFree = 0;
uint32_t rng = 0x1234567u;

int16_t Rand16()
{
	rng = rng * 1664525u + 1013904223u;
	return static_cast<int16_t>(rng >> 16);
}

std::vector<uint8_t> MakeWav(int channels, int rate, int frames)
{
	std::vector<uint8_t> w(44 + frames * channels * 2);
	auto put32 = [&](int at, uint32_t v) { std::memcpy(&w[at], &v, 4); };
	auto put16 = [&](int at, uint16_t v) { std::memcpy(&w[at], &v, 2); };
	std::memcpy(&w[0], "RIFF", 4);
	put32(4, static_cast<uint32_t>(w.size() - 8));
	std::memcpy(&w[8], "WAVEfmt ", 8);
	put32(16, 16);
	put16(20, 1);
	put16(22, static_cast<uint16_t>(channels));
	put32(24, static_cast<uint32_t>(rate));
	put32(28, static_cast<uint32_t>(rate * channels * 2));
	put16(32, static_cast<uint16_t>(channels * 2));
	put16(34, 16);
	std::memcpy(&w[36], "data", 4);
	put32(40, static_cast<uint32_t>(frames * channels * 2));
	for (int i = 0; i < frames * channels; ++i) {
		int16_t v = Rand16();
		if (i % 89 == 0)
			v = (i & 1) != 0 ? 32767 : -32768;
		std::memcpy(&w[44 + 2 * i], &v, 2);
	}
	return w;
}

// The replaced float mixer for one stream, as it ran in of_aulib.cpp.
struct RefStream {
	std::vector<int16_t> pcm;
	int channels, rate;
	bool loop;
	long frameCount, next = 0;
	float pos = 0.0F;
	long consumed = 0;
	float cur[2] = { 0.0F, 0.0F };
	float lscale, rscale;
	bool playing = true;

	RefStream(const std::vector<uint8_t> &wav, int ch, int r, bool lp, float volume, float pan)
	    : channels(ch)
	    , rate(r)
	    , loop(lp)
	{
		pcm.resize((wav.size() - 44) / 2);
		std::memcpy(pcm.data(), wav.data() + 44, pcm.size() * 2);
		frameCount = static_cast<long>(pcm.size()) / ch;
		const float left = pan <= 0.0F ? 1.0F : 1.0F - pan;
		const float right = pan >= 0.0F ? 1.0F : 1.0F + pan;
		lscale = volume * left * 32767.0F;
		rscale = volume * right * 32767.0F;
	}
	bool NextFrame()
	{
		if (next >= frameCount) {
			if (!loop)
				return false;
			next = 0;
		}
		cur[0] = pcm[next * channels] * 0.000030517578125f;
		cur[1] = pcm[next * channels + channels - 1] * 0.000030517578125f;
		++next;
		return true;
	}
	bool Mix(int32_t *acc, int frames)
	{
		if (!playing)
			return false;
		const float step = static_cast<float>(rate) / 48000.0F;
		if (pos >= 1.0F) {
			long whole = static_cast<long>(pos);
			if (whole > consumed) whole = consumed;
			pos -= static_cast<float>(whole);
			consumed -= whole;
		}
		for (int i = 0; i < frames; ++i) {
			const long target = static_cast<long>(pos);
			while (consumed <= target) {
				if (!NextFrame()) {
					playing = false;
					return false;
				}
				++consumed;
			}
			acc[i * 2] += static_cast<int32_t>(cur[0] * lscale);
			acc[i * 2 + 1] += static_cast<int32_t>(cur[1] * rscale);
			pos += step;
		}
		return true;
	}
};

std::unique_ptr<Aulib::Stream> MakeStream(const std::vector<uint8_t> &wav, float volume, float pan)
{
	SDL_RWops *rw = SDL_RWFromConstMem(wav.data(), static_cast<int>(wav.size()));
	auto decoder = std::make_unique<Aulib::DecoderDrwav>();
	CHECK(decoder->open(rw));
	auto stream = std::make_unique<Aulib::Stream>(rw, std::move(decoder), nullptr, true);
	CHECK(stream->s16());
	stream->setVolume(volume);
	stream->setStereoPosition(pan);
	return stream;
}

// Pumps `blocks` and checks every written pair against the reference streams.
void Compare(std::vector<RefStream> &refs, int blocks)
{
	static const int sizes[] = { 1600, 735, 4096, 1, 2, 1023, 1024, 1025, 3000, 17, 800 };
	for (int b = 0; b < blocks; ++b) {
		written.clear();
		ringFree = sizes[b % 11];
		of_aulib_pump();
		std::vector<int16_t> expect;
		const int left = static_cast<int>(written.size() / 2);
		for (int done = 0; done < left;) {
			const int n = left - done < 1024 ? left - done : 1024;
			std::vector<int32_t> acc(n * 2, 0);
			for (RefStream &r : refs)
				r.Mix(acc.data(), n);
			for (int32_t x : acc)
				expect.push_back(static_cast<int16_t>(x > 32767 ? 32767 : (x < -32768 ? -32768 : x)));
			done += n;
		}
		CHECK(expect.size() == written.size());
		CHECK(std::memcmp(expect.data(), written.data(), written.size() * 2) == 0);
	}
}
} // namespace

extern "C" void of_audio_init(void) {}
extern "C" int of_audio_write(const int16_t *samples, int count)
{
	if (count > ringFree)
		count = ringFree;
	written.insert(written.end(), samples, samples + 2 * count);
	ringFree -= count;
	return count;
}
extern "C" int of_audio_free(void) { return ringFree; }
// Clock for the optional PERF=1 pump timer (--perf); the mix does not read it.
extern "C" unsigned int of_time_us(void) { return 0; }

int main()
{
	Aulib::init(48000, AUDIO_S16SYS, 2, 1024, "");

	// Looping mono music at a fractional volume and pan (single-stream path).
	const auto mono = MakeWav(1, 22050, 30011);
	{
		auto stream = MakeStream(mono, 0.7071F, 0.3F);
		stream->play(0);
		std::vector<RefStream> refs { RefStream(mono, 1, 22050, true, 0.7071F, 0.3F) };
		Compare(refs, 120);
		stream->stop();
	}

	// Stereo playing once to the end (end-of-stream inside a block).
	const auto stereo = MakeWav(2, 22050, 17003);
	{
		auto stream = MakeStream(stereo, 0.33F, -0.6F);
		stream->play(1);
		std::vector<RefStream> refs { RefStream(stereo, 2, 22050, false, 0.33F, -0.6F) };
		Compare(refs, 60);
	}

	// Two streams summed (accumulate path, clamping), different rates.
	const auto slow = MakeWav(1, 11025, 9001);
	{
		auto a = MakeStream(stereo, 1.0F, 0.0F);
		auto b = MakeStream(slow, 0.9F, 0.5F);
		a->play(0);
		b->play(0);
		std::vector<RefStream> refs { RefStream(stereo, 2, 22050, true, 1.0F, 0.0F), RefStream(slow, 1, 11025, true, 0.9F, 0.5F) };
		Compare(refs, 90);
		b->stop();
		a->stop();
	}

	std::puts("PASS: 16-bit audio fast path matches the float nearest-neighbor mixer bit for bit");
	return 0;
}
