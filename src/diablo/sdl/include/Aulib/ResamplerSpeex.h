#ifndef OF_AULIB_SHIM_RESAMPLER_SPEEX_H
#define OF_AULIB_SHIM_RESAMPLER_SPEEX_H
#include <Aulib/Resampler.h>
namespace Aulib {
class ResamplerSpeex final : public Resampler {
public:
	explicit ResamplerSpeex(int quality = 5) { (void)quality; }
};
} // namespace Aulib
#endif
