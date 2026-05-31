#ifndef OF_AULIB_SHIM_RESAMPLER_SDL_H
#define OF_AULIB_SHIM_RESAMPLER_SDL_H
#include <Aulib/Resampler.h>
namespace Aulib {
class ResamplerSdl final : public Resampler {
public:
	ResamplerSdl() = default;
};
} // namespace Aulib
#endif
