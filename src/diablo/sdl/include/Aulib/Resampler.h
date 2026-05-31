#ifndef OF_AULIB_SHIM_RESAMPLER_H
#define OF_AULIB_SHIM_RESAMPLER_H
/*
 * Aulib::Resampler -- abstract. In this shim the Stream resamples to the
 * hardware rate internally, so the resampler object is a no-op placeholder
 * (DevilutionX constructs one and hands it to the Stream).
 */
namespace Aulib {
class Resampler {
public:
	virtual ~Resampler() = default;
};
} // namespace Aulib
#endif
