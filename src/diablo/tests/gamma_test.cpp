#include <cstdio>
#include <cstdlib>

#include "engine/palette_gamma.hpp"

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::exit(1); } } while (0)

static unsigned powCalls;
extern "C" float __real_powf(float x, float y);
extern "C" float __wrap_powf(float x, float y)
{
	++powCalls;
	return __real_powf(x, y);
}

int main()
{
	devilution::PaletteGamma correction;
	std::array<SDL_Color, 256> src {}, dst {};
	for (unsigned i = 0; i < src.size(); ++i)
		src[i] = { static_cast<Uint8>(i), static_cast<Uint8>(255 - i), static_cast<Uint8>(i ^ 85), 37 };

	// Every supported gamma, both directions, including repeated palette loads.
	for (int pass = 0; pass < 2; ++pass) {
		for (int index = 0; index <= 14; ++index) {
			const int gamma = pass == 0 ? 30 + 5 * index : 100 - 5 * index;
			dst.fill({ 1, 2, 3, 42 });
			powCalls = 0;
			correction.Apply(dst, src, 256, gamma);
			CHECK(powCalls == (gamma == 100 ? 0 : 256));
			for (unsigned i = 0; i < src.size(); ++i) {
				CHECK(dst[i].r == static_cast<Uint8>(__real_powf(src[i].r / 256.0F, gamma / 100.0F) * 256.0F));
				CHECK(dst[i].g == static_cast<Uint8>(__real_powf(src[i].g / 256.0F, gamma / 100.0F) * 256.0F));
				CHECK(dst[i].b == static_cast<Uint8>(__real_powf(src[i].b / 256.0F, gamma / 100.0F) * 256.0F));
				CHECK(dst[i].a == 42);
			}
			powCalls = 0;
			correction.Apply(dst, src, 256, gamma);
			CHECK(powCalls == 0);
			auto partial = src;
			correction.Apply(partial, partial, 32, gamma);
			CHECK(powCalls == 0);
			for (unsigned i = 0; i < src.size(); ++i) {
				const auto expected = i < 32 ? dst[i] : src[i];
				CHECK(partial[i].r == expected.r && partial[i].g == expected.g && partial[i].b == expected.b);
				CHECK(partial[i].a == src[i].a);
			}
		}
	}
	std::puts("PASS: gamma matches all original RGB results; cached calls use zero powf evaluations");
}
