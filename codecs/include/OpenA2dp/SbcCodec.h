#pragma once

#include <OpenA2dp/IA2dpCodec.h>
#include <OpenA2dp/SbcConfig.h>

namespace OpenA2dp {

namespace Sbc {

// The codec IE masks and bitpool bounds live in SbcConfig, so the selector that picks a
// configuration and the codec that encodes it read the same table.
using SbcConfig::kFs16000;
using SbcConfig::kFs32000;
using SbcConfig::kFs44100;
using SbcConfig::kFs48000;
using SbcConfig::kModeMono;
using SbcConfig::kModeDual;
using SbcConfig::kModeStereo;
using SbcConfig::kModeJoint;
using SbcConfig::kBlocks4;
using SbcConfig::kBlocks8;
using SbcConfig::kBlocks12;
using SbcConfig::kBlocks16;
using SbcConfig::kSubbands4;
using SbcConfig::kSubbands8;
using SbcConfig::kAllocationLoudness;
using SbcConfig::kAllocationSnr;
using SbcConfig::kIeLength;
using SbcConfig::kMinBitpool;
using SbcConfig::kMaxBitpool;
// The whole of each field, for parsing or writing an IE without spelling out the layout.
using SbcConfig::kAllRates;
using SbcConfig::kAllModes;
using SbcConfig::kAllBlocks;
using SbcConfig::kAllSubbands;
using SbcConfig::kAllAllocations;

constexpr uint8_t kSyncWord = 0x9C;
constexpr size_t kMaxFramesPerPacket = 15;

enum class ChannelMode : uint8_t { Mono = 0, DualChannel = 1, Stereo = 2, JointStereo = 3 };
enum class Allocation : uint8_t { Loudness = 0, Snr = 1 };

struct FrameParams {
	uint32_t sampleRate = 44100;
	uint8_t blocks = 16;
	uint8_t subbands = 8;
	ChannelMode channelMode = ChannelMode::JointStereo;
	Allocation allocation = Allocation::Loudness;
	uint8_t bitpool = 53;

	uint8_t Channels() const noexcept
	{
		return static_cast<uint8_t>(SbcConfig::ChannelCount(static_cast<uint8_t>(channelMode)));
	}

	// The same frame in the form the shared allocation solver takes.
	SbcConfig::Configuration ToConfiguration() const noexcept
	{
		return {sampleRate, static_cast<uint8_t>(channelMode), blocks, subbands,
				static_cast<uint8_t>(allocation), bitpool};
	}
};

// Index into the sampling-frequency dependent tables (0=16k, 1=32k, 2=44.1k, 3=48k), or -1.
int SampleRateIndex(uint32_t sampleRate) noexcept;

size_t FrameLength(const FrameParams& p) noexcept;
uint32_t Bitrate(const FrameParams& p) noexcept;

// Parses a fully configured IE (exactly one bit per field). Uses the max bitpool.
bool ParseConfiguredIe(const uint8_t* ie, size_t len, FrameParams& out) noexcept;

// CRC-8 (x^8+x^4+x^3+x^2+1, init 0x0F) over `bitLength` bits, MSB first.
uint8_t Crc8(const uint8_t* data, size_t bitLength) noexcept;

// Bit allocation (A2DP spec 12.6.3). scaleFactors/bits are [channel][subband].
void CalculateBits(const FrameParams& p, const uint8_t scaleFactors[2][8], int bits[2][8]) noexcept;

} // namespace Sbc

class SbcCodec final : public IA2dpCodec {
public:
	CodecType Type() const noexcept override { return CodecType::Sbc; }
	const char* Name() const noexcept override { return "SBC"; }

	size_t BuildLocalCapabilities(uint8_t* ie, size_t capacity) const noexcept override;
	bool SelectConfiguration(const uint8_t* remoteIe, size_t remoteIeLength,
							 const CodecPreference& preference, A2dpCodecConfig& out) const noexcept override;
	uint8_t ValidateConfiguration(const uint8_t* ie, size_t ieLength) const noexcept override;
	HRESULT CreateEncoder(const A2dpCodecConfig& config, std::unique_ptr<IA2dpEncoder>& encoder) const override;
};// The codec library's SBC encoder: the readable, floating-point form of what SbcFixed does
// in fixed point. It allocates bits with the same shared solver, and no production path
// selects it -- the driver encodes SBC in the kernel and the service encodes AAC only -- so
// the tests own it: they pin the frame layout and the allocation output against it. If that
// stops being worth it, this class and those tests go together.
class SbcEncoder final : public IA2dpEncoder
{
public:
	explicit SbcEncoder(const Sbc::FrameParams& params) noexcept;

	const Sbc::FrameParams& Params() const noexcept { return params_; }

	PcmFormat InputFormat() const noexcept override;
	uint32_t SamplesPerFrame() const noexcept override;
	size_t MaxFrameBytes() const noexcept override;
	HRESULT Encode(const int16_t* pcm, uint8_t* out, size_t capacity, size_t& written) noexcept override;
	size_t FramesPerPacket(size_t payloadCapacity) const noexcept override;
	size_t BuildMediaPayload(const uint8_t* frames, const size_t* frameSizes, size_t frameCount,
							 uint8_t* out, size_t capacity) noexcept override;

private:
	void Analyze(const int16_t* pcm, int channel, int block, double* subbandSamples) noexcept;

	Sbc::FrameParams params_;
	double history_[2][80] = {};
};

} // namespace OpenA2dp
