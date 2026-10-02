#pragma once

#include <array>
#include <cmath>

#include <SDL.h>

namespace devilution {

/** Cache the component transform shared by every color in a palette. */
class PaletteGamma {
public:
	void Apply(std::array<SDL_Color, 256> &dst, const std::array<SDL_Color, 256> &src, int n, int gamma)
	{
		if (gamma != gamma_) {
			const float exponent = gamma / 100.0F;
			for (unsigned i = 0; i < table_.size(); ++i) {
				// Preserve the original float formula and rounding on RV32.
				table_[i] = gamma == 100 ? static_cast<Uint8>(i)
				                         : static_cast<Uint8>(powf(i / 256.0F, exponent) * 256.0F);
			}
			gamma_ = gamma;
		}
		for (int i = 0; i < n; ++i) {
			dst[i].r = table_[src[i].r];
			dst[i].g = table_[src[i].g];
			dst[i].b = table_[src[i].b];
		}
	}

private:
	int gamma_ = -1;
	std::array<Uint8, 256> table_ {};
};

} // namespace devilution
