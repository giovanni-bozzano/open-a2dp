#pragma once

#include <stdint.h>
#include <stddef.h>

namespace OpenA2dp::Latm {

struct StreamConfig {
	uint8_t audioObjectType = 2; // AAC LC
	uint32_t sampleRate = 44100;
	uint8_t channels = 2;
};

// Writes one AudioMuxElement(muxConfigPresent = 1) carrying a single AAC access unit
// (RFC 3016 / ISO 14496-3 LATM, as used by A2DP AAC). Returns bytes written or 0.
size_t WriteAudioMuxElement(const StreamConfig& config, const uint8_t* accessUnit, size_t accessUnitLength,
							uint8_t* out, size_t capacity) noexcept;

// Upper bound of the LATM overhead for an access unit of the given size.
size_t MaxOverhead(size_t accessUnitLength) noexcept;

struct ParsedElement {
	StreamConfig config;
	// The payload is not byte aligned inside the element, so it is copied out.
	uint8_t accessUnit[8192] = {};
	size_t accessUnitLength = 0;
};

// Parses an element produced by WriteAudioMuxElement (used by tests and diagnostics).
bool ParseAudioMuxElement(const uint8_t* data, size_t length, ParsedElement& out) noexcept;

} // namespace OpenA2dp::Latm
