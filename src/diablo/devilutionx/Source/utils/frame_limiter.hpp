#pragma once

#include <cstdint>

namespace devilution {

/** Keeps fractional milliseconds between frames without relying on an absolute
 * microsecond clock (which would wrap every 71 minutes). */
class FrameLimiter {
public:
	// Pass intervalUs == 0 when limiting is disabled to discard the old schedule.
	uint32_t GetDelay(uint32_t nowMs, uint32_t intervalUs)
	{
		const uint32_t elapsedMs = nowMs - lastTickMs_;
		lastTickMs_ = nowMs;
		if (intervalUs == 0 || intervalUs != intervalUs_ || elapsedMs >= CeilMilliseconds(remainingUs_)) {
			intervalUs_ = intervalUs;
			remainingUs_ = intervalUs;
			return 0;
		}

		// elapsedMs is bounded by remainingUs_, so this cannot overflow.
		const uint32_t waitUs = remainingUs_ - elapsedMs * 1000;
		remainingUs_ = waitUs + intervalUs;
		return CeilMilliseconds(waitUs);
	}

private:
	static uint32_t CeilMilliseconds(uint32_t us)
	{
		return us / 1000 + (us % 1000 != 0);
	}

	uint32_t lastTickMs_ = 0;
	uint32_t intervalUs_ = 0;
	uint32_t remainingUs_ = 0;
};

} // namespace devilution
