#include "TestFramework.h"

#include <OpenA2dp/FrameRecord.h>
#include <OpenA2dp/LdacCodec.h>
#include <OpenA2dp/LdacStreamEncoder.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace OpenA2dp;
using namespace OpenA2dp::Ldac;

namespace {

// The codec-specific IE a sink advertises: the vendor and codec IDs, then the rate and
// channel masks in their own octets.
void BuildRemoteIe(uint8_t sampleRates, uint8_t channelModes, uint8_t* ie)
{
	WriteIe(sampleRates, channelModes, ie);
}

A2dpCodecConfig ConfigWith(uint8_t sampleRates, uint8_t channelModes)
{
	A2dpCodecConfig config = {};
	config.codec = CodecType::Vendor;
	BuildRemoteIe(sampleRates, channelModes, config.ie);
	config.ieLength = static_cast<uint8_t>(kIeLength);
	return config;
}

std::unique_ptr<IA2dpEncoder> MakeEncoder(uint8_t sampleRates, uint8_t quality = kQualityHigh)
{
	LdacCodec codec(quality);
	std::unique_ptr<IA2dpEncoder> encoder;
	if (FAILED(codec.CreateEncoder(ConfigWith(sampleRates, kChannelStereo), encoder))) {
		return nullptr;
	}
	return encoder;
}

// Interleaved 16-bit stereo: a tone the encoder can actually transform.
std::vector<uint8_t> Tone(uint32_t sampleRate, uint16_t* first = nullptr)
{
	std::vector<int16_t> samples(static_cast<size_t>(kSamplesPerFrame) * 2);
	double phase = first != nullptr ? static_cast<double>(*first) : 0.0;
	for (uint32_t i = 0; i < kSamplesPerFrame; ++i) {
		const double value = std::sin(2.0 * 3.14159265358979 * 1000.0 * phase / sampleRate);
		samples[2 * i] = static_cast<int16_t>(value * 12000.0);
		samples[2 * i + 1] = static_cast<int16_t>(value * 9000.0);
		phase += 1.0;
	}
	std::vector<uint8_t> bytes(samples.size() * sizeof(int16_t));
	std::memcpy(bytes.data(), samples.data(), bytes.size());
	return bytes;
}

struct TestRing {
	alignas(64) uint8_t header[sizeof(RingHeader)];
	std::vector<uint8_t> data;
	RingBufferView view;

	explicit TestRing(uint32_t capacity) : data(capacity)
	{
		RingBufferView::Initialize(reinterpret_cast<RingHeader*>(header), capacity);
		view = RingBufferView(reinterpret_cast<RingHeader*>(header), data.data());
	}
};

} // namespace

TEST(Ldac_IeCarriesItsOwnMasks)
{
	LdacCodec codec;
	uint8_t ie[kIeLength] = {};
	REQUIRE(codec.BuildLocalCapabilities(ie, sizeof(ie)) == kIeLength);

	// Vendor and codec IDs are little endian; the two masks sit in separate octets, and only
	// the rates this pipeline can feed are offered.
	CHECK_EQ(ie[0], uint8_t{0x2D});
	CHECK_EQ(ie[1], uint8_t{0x01});
	CHECK_EQ(ie[2], uint8_t{0x00});
	CHECK_EQ(ie[3], uint8_t{0x00});
	CHECK_EQ(ie[4], uint8_t{0xAA});
	CHECK_EQ(ie[5], uint8_t{0x00});
	CHECK_EQ(ie[6], static_cast<uint8_t>(kSampleRate44100 | kSampleRate48000));
	CHECK_EQ(ie[7], kChannelStereo);

	uint8_t rates = 0;
	uint8_t modes = 0;
	REQUIRE(ParseIe(ie, sizeof(ie), rates, modes));
	CHECK_EQ(rates, static_cast<uint8_t>(kSampleRate44100 | kSampleRate48000));
	CHECK_EQ(modes, kChannelStereo);

	// A sink picks one rate and stereo from what we offered.
	uint8_t chosen[kIeLength] = {};
	WriteIe(kSampleRate48000, kChannelStereo, chosen);
	CHECK_EQ(ValidateIe(chosen, sizeof(chosen)), uint8_t{0});
}

TEST(Ldac_RejectsConfigurationsItCannotRun)
{
	uint8_t ie[kIeLength] = {};

	WriteIe(kSampleRate48000, kChannelStereo, ie);
	CHECK_EQ(ValidateIe(ie, sizeof(ie)), uint8_t{0});

	// Two rates at once is not a configuration.
	WriteIe(static_cast<uint8_t>(kSampleRate44100 | kSampleRate48000), kChannelStereo, ie);
	CHECK_EQ(ValidateIe(ie, sizeof(ie)),
			 static_cast<uint8_t>(Avdtp::ErrorCode::InvalidSamplingFrequency));

	// The higher rates are in the IE but nothing here feeds them.
	WriteIe(kSampleRate88200, kChannelStereo, ie);
	CHECK_EQ(ValidateIe(ie, sizeof(ie)),
			 static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedSamplingFrequency));

	// Mono and dual channel are legal LDAC modes, not modes this source runs.
	WriteIe(kSampleRate48000, kChannelMono, ie);
	CHECK_EQ(ValidateIe(ie, sizeof(ie)),
			 static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedChannelMode));
	WriteIe(kSampleRate48000, kChannelDual, ie);
	CHECK_EQ(ValidateIe(ie, sizeof(ie)),
			 static_cast<uint8_t>(Avdtp::ErrorCode::NotSupportedChannelMode));
	WriteIe(kSampleRate48000, static_cast<uint8_t>(kChannelMono | kChannelStereo), ie);
	CHECK_EQ(ValidateIe(ie, sizeof(ie)), static_cast<uint8_t>(Avdtp::ErrorCode::InvalidChannelMode));

	// A short IE, and one carrying another vendor's ID.
	CHECK_EQ(ValidateIe(ie, kIeLength - 1), static_cast<uint8_t>(Avdtp::ErrorCode::BadLength));
	WriteIe(kSampleRate48000, kChannelStereo, ie);
	ie[0] = 0x4F;
	CHECK_EQ(ValidateIe(ie, sizeof(ie)),
			 static_cast<uint8_t>(Avdtp::ErrorCode::InvalidCodecType));
}

TEST(Ldac_SelectsASinkCompatibleConfiguration)
{
	LdacCodec codec;
	CodecPreference preference;
	A2dpCodecConfig out = {};

	// 48 kHz is the default when the sink offers both.
	REQUIRE(codec.SelectConfiguration(ConfigWith(kSampleRate44100 | kSampleRate48000,
												kChannelStereo).ie,
									  kIeLength, preference, out));
	CHECK_EQ(out.ie[6], kSampleRate48000);
	CHECK_EQ(out.ie[7], kChannelStereo);
	CHECK_EQ(out.ieLength, static_cast<uint8_t>(kIeLength));

	// A stored preference wins when the sink supports it.
	preference.preferredSampleRate = 44100;
	REQUIRE(codec.SelectConfiguration(ConfigWith(kSampleRate44100 | kSampleRate48000,
												kChannelStereo).ie,
									  kIeLength, preference, out));
	CHECK_EQ(out.ie[6], kSampleRate44100);

	// A preference the sink cannot run falls back to what it does support.
	REQUIRE(codec.SelectConfiguration(ConfigWith(kSampleRate48000, kChannelStereo).ie, kIeLength,
									  preference, out));
	CHECK_EQ(out.ie[6], kSampleRate48000);

	// Only modes this source does not run, and another vendor's IE.
	preference = {};
	CHECK(!codec.SelectConfiguration(ConfigWith(kSampleRate48000, kChannelMono).ie, kIeLength,
									 preference, out));
	uint8_t foreign[kIeLength] = {};
	WriteIe(kSampleRate48000, kChannelStereo, foreign);
	foreign[4] = 0x01; // aptX's codec id
	CHECK(!codec.SelectConfiguration(foreign, sizeof(foreign), preference, out));
}

TEST(Ldac_FactoryBuildsTheVendorCodec)
{
	auto codec = CreateVendorCodec(kVendorId, kCodecId);
	REQUIRE(codec != nullptr);
	CHECK(std::string(codec->Name()) == std::string("LDAC"));
	CHECK(codec->Type() == CodecType::Vendor);

	CHECK(CreateVendorCodec(kVendorId, 0x0001) == nullptr);
	CHECK(CreateVendorCodec(0x0000004F, kCodecId) == nullptr);

	// The quality modes map to the bitrates the encoder reports for them.
	CHECK_EQ(BitrateBps(kQualityHigh, 48000), uint32_t{990000});
	CHECK_EQ(BitrateBps(kQualityHigh, 44100), uint32_t{909000});
	CHECK_EQ(BitrateBps(kQualityStandard, 48000), uint32_t{660000});
	CHECK_EQ(BitrateBps(kQualityMobile, 44100), uint32_t{303000});
}

TEST(Ldac_EncoderTakesOneFramePerPacket)
{
	auto encoder = MakeEncoder(kSampleRate48000);
	REQUIRE(encoder != nullptr);

	const PcmFormat format = encoder->InputFormat();
	CHECK_EQ(format.sampleRate, uint32_t{48000});
	CHECK_EQ(format.channels, uint16_t{2});
	CHECK_EQ(format.bitsPerSample, kBitsPerSample);
	CHECK_EQ(encoder->SamplesPerFrame(), kSamplesPerFrame);
	CHECK_EQ(encoder->MaxFrameBytes(), kMaxFrameBytes);

	// One transport frame per packet, and a link below LDAC's 2-DH5 MTU carries none, which is
	// what stops the stream encoder from starting on it.
	CHECK_EQ(encoder->FramesPerPacket(Ioctl::kMaxFramePayload), size_t{1});
	CHECK_EQ(encoder->FramesPerPacket(kMinMtuBytes), size_t{1});
	CHECK_EQ(encoder->FramesPerPacket(kMinMtuBytes - 1), size_t{0});
}

TEST(Ldac_EncodesTheSamePcmTheSameWay)
{
	auto first = MakeEncoder(kSampleRate48000);
	auto second = MakeEncoder(kSampleRate48000);
	REQUIRE(first != nullptr);
	REQUIRE(second != nullptr);

	std::vector<uint8_t> pcm = Tone(48000);
	std::vector<uint8_t> a(kMaxFrameBytes);
	std::vector<uint8_t> b(kMaxFrameBytes);

	// The encoder fills a whole media packet before returning anything, so the first calls
	// consume their samples and write nothing.
	size_t produced = 0;
	size_t packetBytes = 0;
	for (int call = 0; call < 4 && packetBytes == 0; ++call) {
		size_t writtenA = 0;
		size_t writtenB = 0;
		REQUIRE(SUCCEEDED(first->Encode(pcm.data(), a.data(), a.size(), writtenA)));
		REQUIRE(SUCCEEDED(second->Encode(pcm.data(), b.data(), b.size(), writtenB)));
		// Both encoders are fed the same input, so they return the same bytes.
		CHECK_EQ(writtenA, writtenB);
		if (writtenA != 0) {
			CHECK(writtenA <= kMaxFrameBytes);
			CHECK(std::memcmp(a.data(), b.data(), writtenA) == 0);
			packetBytes = writtenA;
		}
		++produced;
	}
	CHECK(produced > 0);
	CHECK(packetBytes > 0);

	// A buffer smaller than the packet the encoder fills is refused rather than overrun.
	size_t small = 0;
	CHECK(FAILED(second->Encode(pcm.data(), b.data(), kMaxFrameBytes - 1, small)));
}

TEST(Ldac_StreamPacesOneFrameRecords)
{
	TestRing pcm(64 * 1024);
	TestRing frames(64 * 1024);

	LdacStreamParams params;
	params.sampleRate = 48000;
	LdacStreamEncoder stream;
	REQUIRE(stream.Start(MakeEncoder(kSampleRate48000), params, pcm.view, 1000));

	// 60 ms of preroll is the whole target at the start.
	CHECK_EQ(stream.TargetSamples(1000), uint64_t{2880});

	// An empty PCM ring becomes silence instead of a stalled stream: the clock's samples are all
	// consumed, in whole encoder units. Records only appear once the encoder holds a full
	// packet, so they trail the consumption by whatever is still pending.
	const uint64_t consumed = ((2880 + kSamplesPerFrame - 1) / kSamplesPerFrame) * kSamplesPerFrame;
	REQUIRE(stream.Pump(1000, pcm.view, frames.view, 64));
	CHECK(stream.Stats().records > 0);
	CHECK_EQ(stream.Stats().silenceSamples, consumed);
	CHECK(stream.Stats().samples <= consumed);
	CHECK_EQ(stream.Stats().samples % kSamplesPerFrame, uint64_t{0});
	CHECK_EQ(stream.Stats().encodeErrors, uint64_t{0});
	CHECK_EQ(stream.Stats().pcmBytes, uint64_t{0});

	uint8_t storage[Ioctl::kMaxFrameRecordBytes];
	FrameRecord::Record record;
	REQUIRE(FrameRecord::Read(frames.view, storage, sizeof(storage), record) ==
			FrameRecord::ReadResult::Record);
	// A record covers whole frames and fits one media packet; the first is flagged after the
	// pre-start PCM was dropped.
	CHECK_EQ(record.samples % kSamplesPerFrame, uint32_t{0});
	CHECK(record.payloadLength > 0);
	CHECK(record.payloadLength <= kMinMtuBytes);
	CHECK(record.payloadLength <= Ioctl::kMaxFramePayload);
	CHECK_EQ(record.flags, Ioctl::kFrameFlagDiscontinuity);
	REQUIRE(FrameRecord::Read(frames.view, storage, sizeof(storage), record) ==
			FrameRecord::ReadResult::Record);
	CHECK_EQ(record.samples % kSamplesPerFrame, uint32_t{0});
	CHECK_EQ(record.flags, uint32_t{0});

	// The clock keeps its own total: 10 ms later, exactly one more millisecond of audio is
	// added to the target and the frames already produced are not counted twice.
	CHECK_EQ(stream.TargetSamples(11000), uint64_t{3360});
	stream.Stop();
	CHECK(!stream.Active());
}

TEST(Ldac_StreamReadsTheRingItIsGiven)
{
	TestRing pcm(64 * 1024);
	TestRing frames(64 * 1024);

	LdacStreamParams params;
	params.sampleRate = 44100;
	LdacStreamEncoder stream;
	REQUIRE(stream.Start(MakeEncoder(kSampleRate44100), params, pcm.view, 0));

	// Exactly enough PCM for the target, so nothing is zero-filled.
	for (uint32_t frame = 0; frame < 2944; ++frame) {
		const int16_t sweep = static_cast<int16_t>(frame);
		int16_t samples[2] = {sweep, static_cast<int16_t>(-static_cast<int32_t>(sweep))};
		REQUIRE(pcm.view.Write(samples, sizeof(samples)));
	}
	// 60 ms of 44.1 kHz is 2646 samples of preroll, consumed in whole encoder units.
	const uint64_t consumed = ((2646 + kSamplesPerFrame - 1) / kSamplesPerFrame) * kSamplesPerFrame;
	REQUIRE(stream.Pump(0, pcm.view, frames.view, 64));
	CHECK_EQ(stream.Stats().silenceSamples, uint64_t{0});
	CHECK(stream.Stats().records > 0);
	// Everything consumed came out of the ring, and what is left is the PCM still unread.
	CHECK_EQ(stream.Stats().pcmBytes, consumed * 4);
	CHECK_EQ(pcm.view.ReadableBytes(), uint32_t{2944 * 4} - static_cast<uint32_t>(consumed * 4));

	// A wipe-out ring is the same stream with silence, not a new one.
	LdacStreamParams wide;
	wide.sampleRate = 44100;
	wide.bitsPerSample = 24;
	REQUIRE(stream.Start(MakeEncoder(kSampleRate44100), wide, pcm.view, 0));
	REQUIRE(stream.Pump(0, pcm.view, frames.view, 64));
	CHECK(stream.Stats().records > 0);
	CHECK_EQ(stream.Stats().silenceSamples, consumed);
	CHECK(stream.Stats().samples <= consumed);
	CHECK_EQ(stream.Stats().encodeErrors, uint64_t{0});
}
