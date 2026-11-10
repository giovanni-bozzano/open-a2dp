#pragma once

// Conversions between the PCM depths the endpoint and the codecs use. Kernel-safe and
// user-mode safe: no STL, no exceptions, no allocation.

#include <OpenA2dp/Types.h>

namespace OpenA2dp::Pcm {

// 24-in-32 is left-justified: the 24 valid bits are the top three bytes and the low byte is
// zero. Narrowing to 16 rounds to nearest instead of truncating, and saturates, because the
// rounded value of the largest positive 24-bit sample is one past INT16_MAX.
constexpr int16_t Narrow24To16(int32_t sample) noexcept
{
	const int32_t rounded = (sample >> 8) + 0x80;
	const int32_t narrowed = rounded >> 8;
	if (narrowed > 32767)
	{
		return 32767;
	}
	if (narrowed < -32768)
	{
		return -32768;
	}
	return static_cast<int16_t>(narrowed);
}

} // namespace OpenA2dp::Pcm
