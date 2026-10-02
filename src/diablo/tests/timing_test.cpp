#include <SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <unistd.h>

#include "utils/frame_limiter.hpp"

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::exit(1); } } while (0)

namespace {
uint64_t clockUs;
unsigned pumpUs, oversleepUs, pumps, sleeps;
bool interruptSleep;

void Reset(uint64_t now = 0)
{
	clockUs = now;
	pumpUs = oversleepUs = pumps = sleeps = 0;
	interruptSleep = false;
}
} // namespace

extern "C" unsigned of_time_ms() { return static_cast<unsigned>(clockUs / 1000); }
extern "C" void of_aulib_pump()
{
	++pumps;
	clockUs += pumpUs;
}
extern "C" int __wrap_usleep(useconds_t us)
{
	++sleeps;
	if (interruptSleep) {
		interruptSleep = false;
		return -1;
	}
	clockUs += us + oversleepUs;
	return 0;
}

int main()
{
	Reset();
	SDL_Delay(0);
	CHECK(pumps == 1 && sleeps == 0);
	Reset();
	pumpUs = 1000;
	SDL_Delay(9);
	CHECK(clockUs == 9000 && sleeps == 4 && pumps == 5);
	Reset();
	pumpUs = 15000;
	SDL_Delay(9);
	CHECK(clockUs == 15000 && sleeps == 0);
	Reset();
	oversleepUs = 4000;
	SDL_Delay(10);
	CHECK(clockUs == 10000 && sleeps == 2);
	Reset();
	interruptSleep = true;
	SDL_Delay(3);
	CHECK(clockUs == 3000 && sleeps == 4);
	Reset((uint64_t { 1 } << 32) * 1000 - 2000);
	const uint64_t wrapStart = clockUs;
	SDL_Delay(5);
	CHECK(clockUs - wrapStart == 5000);

	// Sustain a fractional 60 Hz period, including SDL's audio work and timer
	// rounding. Repeat across both the old microsecond and millisecond wraps.
	for (const uint64_t startMs : { uint64_t { 0 }, uint64_t { 4294960 }, (uint64_t { 1 } << 32) - 100 }) {
		Reset(startMs * 1000);
		pumpUs = 500;
		devilution::FrameLimiter limiter;
		CHECK(limiter.GetDelay(SDL_GetTicks(), 16666) == 0);
		for (unsigned frame = 1; frame <= 600; ++frame) {
			clockUs += 4000; // render the next frame
			SDL_Delay(limiter.GetDelay(SDL_GetTicks(), 16666));
			const uint64_t idealUs = startMs * 1000 + frame * uint64_t { 16666 };
			CHECK(clockUs >= idealUs);
			CHECK(clockUs - idealUs < 2500); // bounded rounding/pump overhead, no drift
		}
	}

	devilution::FrameLimiter limiter;
	CHECK(limiter.GetDelay(0, 16666) == 0);
	CHECK(limiter.GetDelay(4, 16666) == 13);
	CHECK(limiter.GetDelay(1000, 16666) == 0); // no catch-up burst after a stall
	CHECK(limiter.GetDelay(1004, 16666) == 13);
	CHECK(limiter.GetDelay(1005, 0) == 0);
	CHECK(limiter.GetDelay(1006, 16666) == 0); // discard disabled schedule
	CHECK(limiter.GetDelay(1007, 33333) == 0); // refresh-rate change
	CHECK(limiter.GetDelay(1011, 33333) == 30);
	CHECK(limiter.GetDelay(3600000, 33333) == 0); // long suspension
	std::puts("PASS: elapsed SDL delays, fractional frame pacing, stalls, and clock wrap");
}
