#pragma once

// Independent SBC decoder (A2DP spec 12.6) used only to validate the encoder.

#include <OpenA2dp/SbcCodec.h>

#include <cstdint>
#include <vector>

namespace TestSupport {

class SbcReferenceDecoder {
public:
	struct FrameInfo {
		OpenA2dp::Sbc::FrameParams params;
		size_t length = 0;
		bool crcOk = false;
	};

	// Decodes one frame, appends interleaved PCM to `pcm`. Returns false on parse error.
	bool DecodeFrame(const uint8_t* data, size_t length, std::vector<int16_t>& pcm, FrameInfo& info);

	// Overall synthesis gain; the spec synthesis window is the analysis prototype scaled by this value.
	static double SynthesisScale(int subbands);

private:
	double v_[2][160] = {};
};

} // namespace TestSupport
