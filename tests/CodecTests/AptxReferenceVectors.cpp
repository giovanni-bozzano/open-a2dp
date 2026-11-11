// Reference codeword vectors for the vendored aptX encoders.
//
// The vendored AOSP encoders are the reference implementation, so a fixed PCM sequence has
// exactly one correct codeword sequence. Pinned here, it catches what the local invariants
// cannot: a change to the wrapper, the sample widening, the byte order or the encoder state
// that still produces "some" codewords but no longer the ones a reference sink decodes. The
// input is integer-only on purpose (no sin, no floating point), so the vector cannot drift
// with a CRT or a compiler's math library.

#include "TestFramework.h"

#include <OpenA2dp/AptxCodec.h>
#include <OpenA2dp/AptxHdCodec.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace OpenA2dp;
using namespace OpenA2dp::Aptx;

namespace {

constexpr uint32_t kFrames = 64;          // 256 samples per channel
constexpr uint32_t kPinnedFrames = 16;    // the part pinned byte by byte
constexpr uint32_t kSeed = 0x5EED1234u;

// Every frame of the sequence consumes the same number of random values in both codecs, so the
// two vectors describe the same signal at 16 and 24 bits.
uint32_t NextRandom(uint32_t& state)
{
	state = state * 1664525u + 1013904223u;
	return state;
}

int16_t ClassicSample(uint32_t& state)
{
	// The whole 16-bit range, extremes included: the quantiser has to see the top of it.
	return static_cast<int16_t>(NextRandom(state) >> 16);
}

int32_t HdSample(uint32_t& state)
{
	// A signed 24-bit value, left-justified as 24-in-32 carries it.
	uint32_t bits = NextRandom(state) & 0xFFFFFFu;
	if ((bits & 0x800000u) != 0) {
		bits |= 0xFF000000u;
	}
	const uint32_t wide = bits << 8;
	int32_t value = 0;
	std::memcpy(&value, &wide, sizeof(value));
	return value;
}

std::vector<uint8_t> ClassicPcm()
{
	std::vector<int16_t> samples(static_cast<size_t>(kFrames) * kSamplesPerFrame * 2);
	uint32_t state = kSeed;
	for (int16_t& sample : samples) {
		sample = ClassicSample(state);
	}
	std::vector<uint8_t> bytes(samples.size() * sizeof(int16_t));
	std::memcpy(bytes.data(), samples.data(), bytes.size());
	return bytes;
}

std::vector<uint8_t> HdPcm()
{
	std::vector<int32_t> samples(static_cast<size_t>(kFrames) * AptxHd::kSamplesPerFrame * 2);
	uint32_t state = kSeed;
	for (int32_t& sample : samples) {
		sample = HdSample(state);
	}
	std::vector<uint8_t> bytes(samples.size() * sizeof(int32_t));
	std::memcpy(bytes.data(), samples.data(), bytes.size());
	return bytes;
}

void BuildRemoteIe(uint32_t vendorId, uint16_t codecId, uint8_t capabilities, uint8_t* ie, size_t length)
{
	ie[0] = static_cast<uint8_t>(vendorId & 0xFF);
	ie[1] = static_cast<uint8_t>((vendorId >> 8) & 0xFF);
	ie[2] = static_cast<uint8_t>((vendorId >> 16) & 0xFF);
	ie[3] = static_cast<uint8_t>((vendorId >> 24) & 0xFF);
	ie[4] = static_cast<uint8_t>(codecId & 0xFF);
	ie[5] = static_cast<uint8_t>((codecId >> 8) & 0xFF);
	ie[6] = capabilities;
	std::memset(ie + 7, 0, length > 7 ? length - 7 : 0);
}

A2dpCodecConfig Config(uint32_t vendorId, uint16_t codecId, uint8_t capabilities, size_t length)
{
	A2dpCodecConfig config = {};
	config.codec = CodecType::Vendor;
	BuildRemoteIe(vendorId, codecId, capabilities, config.ie, length);
	config.ieLength = static_cast<uint8_t>(length);
	return config;
}

std::unique_ptr<IA2dpEncoder> ClassicEncoder(uint8_t capabilities)
{
	AptxCodec codec;
	std::unique_ptr<IA2dpEncoder> encoder;
	if (FAILED(codec.CreateEncoder(Config(kVendorId, kCodecId, capabilities, kIeLength), encoder))) {
		return nullptr;
	}
	return encoder;
}

std::unique_ptr<IA2dpEncoder> HdEncoder(uint8_t capabilities)
{
	AptxHdCodec codec;
	std::unique_ptr<IA2dpEncoder> encoder;
	if (FAILED(codec.CreateEncoder(Config(AptxHd::kVendorId, AptxHd::kCodecId, capabilities,
										 AptxHd::kIeLength),
				 encoder))) {
		return nullptr;
	}
	return encoder;
}

std::vector<uint8_t> HexBytes(const char* text)
{
	std::vector<uint8_t> bytes;
	uint8_t value = 0;
	int nibbles = 0;
	for (const char* cursor = text; *cursor != '\0'; ++cursor) {
		const char c = *cursor;
		int digit = -1;
		if (c >= '0' && c <= '9') {
			digit = c - '0';
		} else if (c >= 'A' && c <= 'F') {
			digit = c - 'A' + 10;
		} else if (c >= 'a' && c <= 'f') {
			digit = c - 'a' + 10;
		}
		if (digit < 0) {
			continue;
		}
		value = static_cast<uint8_t>((value << 4) | digit);
		if (++nibbles == 2) {
			bytes.push_back(value);
			value = 0;
			nibbles = 0;
		}
	}
	return bytes;
}

std::string AsHex(const std::vector<uint8_t>& bytes, size_t count)
{
	std::string text;
	char byte[4] = {};
	for (size_t i = 0; i < count && i < bytes.size(); ++i) {
		std::snprintf(byte, sizeof(byte), "%02X", bytes[i]);
		text += byte;
	}
	return text;
}

// A 64-bit FNV-1a over the whole sequence: the pinned prefix says what the codewords are, the
// hash covers every frame after it without a 400-character literal.
uint64_t Fnv1a(const std::vector<uint8_t>& bytes)
{
	uint64_t hash = 1469598103934665603ull;
	for (const uint8_t byte : bytes) {
		hash ^= byte;
		hash *= 1099511628211ull;
	}
	return hash;
}

void CheckPinned(const std::vector<uint8_t>& actual, size_t count, const char* expectedHex,
				 const char* label)
{
	const std::vector<uint8_t> expected = HexBytes(expectedHex);
	if (expected.size() != count) {
		// The codewords are printed either way: a broken literal is repaired from this report.
		TestFramework::ReportFailure(__FILE__, __LINE__,
				std::string(label) + ": the pinned vector has " + std::to_string(expected.size()) +
						" bytes, the check compares " + std::to_string(count) +
						"; codewords: " + AsHex(actual, count));
		return;
	}
	for (size_t i = 0; i < count; ++i) {
		if (actual[i] == expected[i]) {
			continue;
		}
		char position[96] = {};
		std::snprintf(position, sizeof(position), ": first difference at byte %zu, got %02X, expected %02X",
				i, actual[i], expected[i]);
		TestFramework::ReportFailure(__FILE__, __LINE__,
				std::string(label) + position + "; codewords: " + AsHex(actual, count) +
						"; expected: " + AsHex(expected, count));
		return;
	}
}

void CheckHash(const std::vector<uint8_t>& actual, const char* expectedHex, const char* label)
{
	const uint64_t expected = std::strtoull(expectedHex, nullptr, 16);
	const uint64_t computed = Fnv1a(actual);
	if (computed != expected) {
		char prefix[128] = {};
		std::snprintf(prefix, sizeof(prefix), ": FNV-1a is %016llX over %zu bytes, expected %016llX",
				computed, actual.size(), expected);
		TestFramework::ReportFailure(__FILE__, __LINE__,
				std::string(label) + prefix + "; codewords: " + AsHex(actual, actual.size()));
	}
}

// Encodes the fixed sequence with one encoder and returns the codewords back to back.
std::vector<uint8_t> EncodeSequence(IA2dpEncoder& encoder, const std::vector<uint8_t>& pcm)
{
	const size_t frameBytes = encoder.MaxFrameBytes();
	const size_t pcmFrameBytes = static_cast<size_t>(encoder.SamplesPerFrame()) * encoder.InputFormat().channels *
			(encoder.InputFormat().bitsPerSample > 16 ? 4u : 2u);
	std::vector<uint8_t> frames(frameBytes * kFrames, 0);
	for (uint32_t frame = 0; frame < kFrames; ++frame) {
		size_t written = 0;
		const HRESULT hr = encoder.Encode(pcm.data() + frame * pcmFrameBytes,
				frames.data() + frame * frameBytes, frameBytes, written);
		if (FAILED(hr) || written != frameBytes) {
			return {};
		}
	}
	return frames;
}

} // namespace

TEST(Aptx_ReferenceCodewordVector)
{
	auto encoder = ClassicEncoder(kSampleRate48000 | kChannelStereo);
	REQUIRE(encoder != nullptr);
	const std::vector<uint8_t> frames = EncodeSequence(*encoder, ClassicPcm());
	REQUIRE(frames.size() == kFrames * kFrameBytes);

	// The first sixteen codeword pairs, pinned byte by byte: what a reference sink decodes.
	CheckPinned(frames, kPinnedFrames * kFrameBytes,
			""
			"4BBF4BBF07FFD8006D6CF0064C4593B4AB0FB44493BD8C4C444A8E3F8ACB73C1"
			"74BFAC388BC053D293BD8C128B8950C0AA0EABBF43408FBE508C911069575361",
			"aptX");
	CheckHash(frames, "48135F75550F0A99", "aptX");
}

TEST(Aptx_ReferenceCodewordVectorIsRateIndependent)
{
	// aptX carries no depth or rate field and the encoder has no rate parameter: the same PCM
	// must produce the same codewords at either endpoint rate, or the configuration leaked into
	// the encoder.
	auto at48 = ClassicEncoder(kSampleRate48000 | kChannelStereo);
	auto at441 = ClassicEncoder(kSampleRate44100 | kChannelStereo);
	REQUIRE(at48 != nullptr);
	REQUIRE(at441 != nullptr);
	const std::vector<uint8_t> pcm = ClassicPcm();
	const std::vector<uint8_t> one = EncodeSequence(*at48, pcm);
	const std::vector<uint8_t> other = EncodeSequence(*at441, pcm);
	REQUIRE(one.size() == other.size());
	CHECK(std::memcmp(one.data(), other.data(), one.size()) == 0);
}

TEST(Aptx_ReferenceCodewordsDependOnHistory)
{
	// The vector is only meaningful because the encoder is stateful: a second pass over the same
	// frame from the same encoder must not repeat the first codewords.
	auto encoder = ClassicEncoder(kSampleRate48000 | kChannelStereo);
	REQUIRE(encoder != nullptr);
	const std::vector<uint8_t> pcm = ClassicPcm();

	uint8_t first[kFrameBytes] = {};
	uint8_t second[kFrameBytes] = {};
	size_t written = 0;
	REQUIRE(SUCCEEDED(encoder->Encode(pcm.data(), first, sizeof(first), written)));
	REQUIRE(SUCCEEDED(encoder->Encode(pcm.data(), second, sizeof(second), written)));
	CHECK(std::memcmp(first, second, kFrameBytes) != 0);
}

TEST(AptxHd_ReferenceCodewordVector)
{
	auto encoder = HdEncoder(AptxHd::kSampleRate48000 | AptxHd::kChannelStereo);
	REQUIRE(encoder != nullptr);
	const std::vector<uint8_t> frames = EncodeSequence(*encoder, HdPcm());
	REQUIRE(frames.size() == kFrames * AptxHd::kFrameBytes);

	// Three bytes per channel per frame: the 24-bit path's own codewords.
	CheckPinned(frames, kPinnedFrames * AptxHd::kFrameBytes,
			""
			"004FBF004FBF00D80000D80000F1BF00380000B7BF002FBF004E400057C00069"
			"C000B1C000913F00883F0057400077C000893F0088BF0074C000764000483F00"
			"8A3F00544000564000B54000B74000B7C000763F0074C0007440004F3F00AA3F",
			"aptX HD");
	CheckHash(frames, "78CE5C75E7875D7F", "aptX HD");
}

TEST(AptxHd_ReferenceCodewordsIgnoreTheLowByte)
{
	// The vector is for 24-in-32: setting the low byte (which is not sample data) must not move a
	// single codeword, or the widening would be reading bits the format does not carry.
	std::vector<uint8_t> pcm = HdPcm();
	REQUIRE(pcm.size() >= 4);
	for (size_t i = 3; i < pcm.size(); i += 4) {
		pcm[i] = 0xEF;
	}
	auto clean = HdEncoder(AptxHd::kSampleRate48000 | AptxHd::kChannelStereo);
	auto dirty = HdEncoder(AptxHd::kSampleRate48000 | AptxHd::kChannelStereo);
	REQUIRE(clean != nullptr);
	REQUIRE(dirty != nullptr);
	const std::vector<uint8_t> one = EncodeSequence(*clean, HdPcm());
	const std::vector<uint8_t> other = EncodeSequence(*dirty, pcm);
	REQUIRE(one.size() == other.size());
	CHECK(std::memcmp(one.data(), other.data(), one.size()) == 0);
}
