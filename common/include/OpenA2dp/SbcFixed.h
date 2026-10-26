#pragma once

// Fixed-point SBC encoder (A2DP spec 12.5 / 12.6) and test-tone generator.
// Kernel-safe: no STL, no exceptions, no allocation, no floating point.

#include <OpenA2dp/BitWriter.h>
#include <OpenA2dp/SbcFixedTables.h>
#include <OpenA2dp/Types.h>

namespace OpenA2dp::SbcFixed {

constexpr uint8_t kSyncWord = 0x9C;
constexpr size_t kMaxFramesPerPacket = 15;
constexpr size_t kMaxFrameBytes = 4 + (4 * 8 * 2) / 8 + (8 + 16 * 250 + 7) / 8;

enum class ChannelMode : uint8_t { Mono = 0, DualChannel = 1, Stereo = 2, JointStereo = 3 };
enum class Allocation : uint8_t { Loudness = 0, Snr = 1 };

struct Params {
	uint32_t sampleRate = 44100;
	uint8_t blocks = 16;
	uint8_t subbands = 8;
	ChannelMode channelMode = ChannelMode::JointStereo;
	Allocation allocation = Allocation::Loudness;
	uint8_t bitpool = 53;

	uint8_t Channels() const noexcept { return channelMode == ChannelMode::Mono ? 1 : 2; }
	uint32_t SamplesPerFrame() const noexcept { return static_cast<uint32_t>(blocks) * subbands; }
};

inline int SampleRateIndex(uint32_t rate) noexcept
{
	return rate == 16000 ? 0 : rate == 32000 ? 1 : rate == 44100 ? 2 : rate == 48000 ? 3 : -1;
}

inline bool SingleBit(uint8_t v) noexcept { return v != 0 && (v & (v - 1)) == 0; }

// Parses a configured SBC IE (one bit per field); the max bitpool is used.
inline bool ParseIe(const uint8_t* ie, size_t len, Params& out) noexcept
{
	if (len < 4) {
		return false;
	}
	const uint8_t fs = ie[0] & 0xF0;
	const uint8_t mode = ie[0] & 0x0F;
	const uint8_t blocks = ie[1] & 0xF0;
	const uint8_t subbands = ie[1] & 0x0C;
	const uint8_t alloc = ie[1] & 0x03;
	if (!SingleBit(fs) || !SingleBit(mode) || !SingleBit(blocks) || !SingleBit(subbands) || !SingleBit(alloc)) {
		return false;
	}
	out.sampleRate = fs == 0x80 ? 16000 : fs == 0x40 ? 32000 : fs == 0x20 ? 44100 : 48000;
	out.channelMode = mode == 0x08 ? ChannelMode::Mono
					: mode == 0x04 ? ChannelMode::DualChannel
					: mode == 0x02 ? ChannelMode::Stereo
					: ChannelMode::JointStereo;
	out.blocks = blocks == 0x80 ? 4 : blocks == 0x40 ? 8 : blocks == 0x20 ? 12 : 16;
	out.subbands = subbands == 0x08 ? 4 : 8;
	out.allocation = alloc == 0x02 ? Allocation::Snr : Allocation::Loudness;
	out.bitpool = ie[3];
	return out.bitpool >= 2 && out.bitpool >= ie[2];
}

inline size_t FrameLength(const Params& p) noexcept
{
	const size_t nch = p.Channels();
	const size_t length = 4 + (4 * p.subbands * nch) / 8;
	size_t audioBits = 0;
	switch (p.channelMode) {
	case ChannelMode::Mono:
	case ChannelMode::DualChannel:
		audioBits = static_cast<size_t>(p.blocks) * nch * p.bitpool;
		break;
	case ChannelMode::Stereo:
		audioBits = static_cast<size_t>(p.blocks) * p.bitpool;
		break;
	case ChannelMode::JointStereo:
		audioBits = p.subbands + static_cast<size_t>(p.blocks) * p.bitpool;
		break;
	}
	return length + (audioBits + 7) / 8;
}

inline uint8_t Crc8(const uint8_t* data, size_t bitLength) noexcept
{
	uint8_t crc = 0x0F;
	for (size_t i = 0; i < bitLength; ++i) {
		const unsigned bit = (data[i >> 3] >> (7 - (i & 7))) & 1u;
		const unsigned feedback = ((crc >> 7) & 1u) ^ bit;
		crc = static_cast<uint8_t>(crc << 1);
		if (feedback) {
			crc ^= 0x1D;
		}
	}
	return crc;
}

namespace Detail {

constexpr int kOffset4[4][4] = {{-1, 0, 0, 0}, {-2, 0, 0, 1}, {-2, 0, 0, 1}, {-2, 0, 0, 1}};
constexpr int kOffset8[4][8] = {
	{-2, 0, 0, 0, 0, 0, 0, 1}, {-3, 0, 0, 0, 0, 0, 1, 2}, {-4, 0, 0, 0, 0, 0, 1, 2}, {-4, 0, 0, 0, 0, 0, 1, 2}};

inline void ComputeBitneed(const Params& p, const uint8_t sf[2][8], int ch, int bitneed[2][8]) noexcept
{
	const int fsIndex = SampleRateIndex(p.sampleRate);
	for (int sb = 0; sb < p.subbands; ++sb) {
		if (p.allocation == Allocation::Snr) {
			bitneed[ch][sb] = sf[ch][sb];
		} else if (sf[ch][sb] == 0) {
			bitneed[ch][sb] = -5;
		} else {
			const int offset = p.subbands == 4 ? kOffset4[fsIndex][sb] : kOffset8[fsIndex][sb];
			const int loudness = sf[ch][sb] - offset;
			bitneed[ch][sb] = loudness > 0 ? loudness / 2 : loudness;
		}
	}
}

inline int CountSlice(int bitneed, int bitslice) noexcept
{
	if (bitneed > bitslice + 1 && bitneed < bitslice + 16) {
		return 1;
	}
	return bitneed == bitslice + 1 ? 2 : 0;
}

inline void Allocate(const Params& p, const int* channels, int channelCount, int bitneed[2][8], int bits[2][8]) noexcept
{
	const int nsb = p.subbands;
	const int bitpool = p.bitpool;

	int maxBitneed = -1000;
	for (int c = 0; c < channelCount; ++c) {
		for (int sb = 0; sb < nsb; ++sb) {
			if (bitneed[channels[c]][sb] > maxBitneed) {
				maxBitneed = bitneed[channels[c]][sb];
			}
		}
	}

	int bitcount = 0;
	int slicecount = 0;
	int bitslice = maxBitneed + 1;
	do {
		--bitslice;
		bitcount += slicecount;
		slicecount = 0;
		for (int c = 0; c < channelCount; ++c) {
			for (int sb = 0; sb < nsb; ++sb) {
				slicecount += CountSlice(bitneed[channels[c]][sb], bitslice);
			}
		}
	} while (bitcount + slicecount < bitpool);

	if (bitcount + slicecount == bitpool) {
		bitcount += slicecount;
		--bitslice;
	}

	for (int c = 0; c < channelCount; ++c) {
		const int ch = channels[c];
		for (int sb = 0; sb < nsb; ++sb) {
			const int b = bitneed[ch][sb] - bitslice;
			bits[ch][sb] = bitneed[ch][sb] < bitslice + 2 ? 0 : (b < 16 ? b : 16);
		}
	}

	int idx = 0;
	const int total = nsb * channelCount;
	while (bitcount < bitpool && idx < total) {
		const int ch = channels[idx % channelCount];
		const int sb = idx / channelCount;
		if (bits[ch][sb] >= 2 && bits[ch][sb] < 16) {
			++bits[ch][sb];
			++bitcount;
		} else if (bitneed[ch][sb] == bitslice + 1 && bitpool > bitcount + 1) {
			bits[ch][sb] = 2;
			bitcount += 2;
		}
		++idx;
	}

	idx = 0;
	while (bitcount < bitpool && idx < total) {
		const int ch = channels[idx % channelCount];
		const int sb = idx / channelCount;
		if (bits[ch][sb] < 16) {
			++bits[ch][sb];
			++bitcount;
		}
		++idx;
	}
}

} // namespace Detail

inline void CalculateBits(const Params& p, const uint8_t sf[2][8], int bits[2][8]) noexcept
{
	int bitneed[2][8] = {};
	for (int ch = 0; ch < 2; ++ch) {
		for (int sb = 0; sb < 8; ++sb) {
			bits[ch][sb] = 0;
		}
	}
	if (p.channelMode == ChannelMode::Mono || p.channelMode == ChannelMode::DualChannel) {
		for (int ch = 0; ch < p.Channels(); ++ch) {
			Detail::ComputeBitneed(p, sf, ch, bitneed);
			const int channels[1] = {ch};
			Detail::Allocate(p, channels, 1, bitneed, bits);
		}
	} else {
		Detail::ComputeBitneed(p, sf, 0, bitneed);
		Detail::ComputeBitneed(p, sf, 1, bitneed);
		const int channels[2] = {0, 1};
		Detail::Allocate(p, channels, 2, bitneed, bits);
	}
}

// Subband samples are kept in Q12 relative to 16-bit PCM units (|x| < 2^16 fits in int32).
constexpr int kFracBits = 12;

inline uint8_t ScaleFactorFor(int64_t maxAbs) noexcept
{
	uint8_t sf = 0;
	while (sf < 15 && maxAbs >= (static_cast<int64_t>(1) << (sf + 1 + kFracBits))) {
		++sf;
	}
	return sf;
}

class Encoder {
public:
	Encoder() noexcept = default;
	explicit Encoder(const Params& params) noexcept { Reset(params); }

	void Reset(const Params& params) noexcept
	{
		params_ = params;
		for (int ch = 0; ch < 2; ++ch) {
			for (int i = 0; i < 80; ++i) {
				history_[ch][i] = 0;
			}
		}
	}

	const Params& GetParams() const noexcept { return params_; }

	// Encodes one frame of interleaved PCM (SamplesPerFrame() * Channels() samples).
	// Returns the frame length, or 0 on error.
	size_t Encode(const int16_t* pcm, uint8_t* out, size_t capacity) noexcept
	{
		const Params& p = params_;
		const size_t frameLength = FrameLength(p);
		if (pcm == nullptr || out == nullptr || capacity < frameLength || SampleRateIndex(p.sampleRate) < 0 ||
			(p.subbands != 4 && p.subbands != 8) || p.blocks == 0 || p.blocks > 16) {
			return 0;
		}

		const int nsb = p.subbands;
		const int nblk = p.blocks;
		const int nch = p.Channels();

		int32_t samples[16][2][8] = {};
		for (int blk = 0; blk < nblk; ++blk) {
			for (int ch = 0; ch < nch; ++ch) {
				Analyze(pcm, ch, blk, samples[blk][ch]);
			}
		}

		uint8_t sf[2][8] = {};
		for (int ch = 0; ch < nch; ++ch) {
			for (int sb = 0; sb < nsb; ++sb) {
				int64_t maxAbs = 0;
				for (int blk = 0; blk < nblk; ++blk) {
					const int64_t v = Abs(samples[blk][ch][sb]);
					maxAbs = v > maxAbs ? v : maxAbs;
				}
				sf[ch][sb] = ScaleFactorFor(maxAbs);
			}
		}

		uint8_t join = 0;
		if (p.channelMode == ChannelMode::JointStereo) {
			for (int sb = 0; sb < nsb - 1; ++sb) {
				int64_t maxMid = 0;
				int64_t maxSide = 0;
				for (int blk = 0; blk < nblk; ++blk) {
					const int64_t l = samples[blk][0][sb];
					const int64_t r = samples[blk][1][sb];
					const int64_t mid = Abs((l + r) / 2);
					const int64_t side = Abs((l - r) / 2);
					maxMid = mid > maxMid ? mid : maxMid;
					maxSide = side > maxSide ? side : maxSide;
				}
				const uint8_t sfMid = ScaleFactorFor(maxMid);
				const uint8_t sfSide = ScaleFactorFor(maxSide);
				if (sfMid + sfSide < sf[0][sb] + sf[1][sb]) {
					join |= static_cast<uint8_t>(1u << (nsb - 1 - sb));
					sf[0][sb] = sfMid;
					sf[1][sb] = sfSide;
					for (int blk = 0; blk < nblk; ++blk) {
						const int32_t l = samples[blk][0][sb];
						const int32_t r = samples[blk][1][sb];
						samples[blk][0][sb] = static_cast<int32_t>((static_cast<int64_t>(l) + r) / 2);
						samples[blk][1][sb] = static_cast<int32_t>((static_cast<int64_t>(l) - r) / 2);
					}
				}
			}
		}

		int bits[2][8] = {};
		CalculateBits(p, sf, bits);

		BitWriter w(out, capacity);
		w.Write(kSyncWord, 8);
		w.Write(static_cast<uint32_t>(SampleRateIndex(p.sampleRate)), 2);
		w.Write(static_cast<uint32_t>(nblk / 4 - 1), 2);
		w.Write(static_cast<uint32_t>(p.channelMode), 2);
		w.Write(static_cast<uint32_t>(p.allocation), 1);
		w.Write(nsb == 8 ? 1u : 0u, 1);
		w.Write(p.bitpool, 8);
		w.Write(0, 8); // CRC placeholder
		if (p.channelMode == ChannelMode::JointStereo) {
			w.Write(join, static_cast<unsigned>(nsb));
		}
		for (int ch = 0; ch < nch; ++ch) {
			for (int sb = 0; sb < nsb; ++sb) {
				w.Write(sf[ch][sb], 4);
			}
		}

		const size_t crcBits = 16 + (p.channelMode == ChannelMode::JointStereo ? nsb : 0) + 4u * nsb * nch;
		uint8_t crcData[2 + 18] = {};
		crcData[0] = out[1];
		crcData[1] = out[2];
		const size_t tailBytes = (crcBits - 16 + 7) / 8;
		for (size_t i = 0; i < tailBytes; ++i) {
			crcData[2 + i] = out[4 + i];
		}
		out[3] = Crc8(crcData, crcBits);

		for (int blk = 0; blk < nblk; ++blk) {
			for (int ch = 0; ch < nch; ++ch) {
				for (int sb = 0; sb < nsb; ++sb) {
					const int b = bits[ch][sb];
					if (b == 0) {
						continue;
					}
					const int shift = sf[ch][sb] + 1 + kFracBits; // scale = 2^shift
					const int64_t levels = (static_cast<int64_t>(1) << b) - 1;
					int64_t s = samples[blk][ch][sb];
					const int64_t limit = static_cast<int64_t>(1) << shift;
					s = s < -limit ? -limit : s > limit ? limit : s;
					// floor((s / scale + 1) * levels / 2)
					int64_t q = ((s + limit) * levels) >> (shift + 1);
					q = q < 0 ? 0 : q > levels ? levels : q;
					w.Write(static_cast<uint32_t>(q), static_cast<unsigned>(b));
				}
			}
		}
		w.AlignToByte();
		if (w.Overflow() || w.BytesUsed() > frameLength) {
			return 0;
		}
		for (size_t i = w.BytesUsed(); i < frameLength; ++i) {
			out[i] = 0;
		}
		return frameLength;
	}

private:
	static int64_t Abs(int64_t v) noexcept { return v < 0 ? -v : v; }

	void Analyze(const int16_t* pcm, int channel, int block, int32_t* subbandSamples) noexcept
	{
		const int m = params_.subbands;
		const int nch = params_.Channels();
		int32_t* x = history_[channel];

		for (int i = 10 * m - 1; i >= m; --i) {
			x[i] = x[i - m];
		}
		const int16_t* in = pcm + static_cast<size_t>(block) * m * nch + channel;
		for (int i = m - 1; i >= 0; --i) {
			x[i] = *in;
			in += nch;
		}

		const int32_t* c = m == 4 ? Tables::kProto4Q30 : Tables::kProto8Q30;
		int64_t y[16] = {};
		for (int i = 0; i < 2 * m; ++i) {
			int64_t sum = 0;
			for (int j = 0; j < 5; ++j) {
				const int idx = i + j * 2 * m;
				sum += static_cast<int64_t>(c[idx]) * x[idx];
			}
			y[i] = sum >> (30 - kFracBits);
		}

		for (int i = 0; i < m; ++i) {
			int64_t sum = 0;
			for (int k = 0; k < 2 * m; ++k) {
				const int16_t coeff = m == 4 ? Tables::kCos4Q14[i][k] : Tables::kCos8Q14[i][k];
				sum += coeff * y[k];
			}
			subbandSamples[i] = static_cast<int32_t>(sum >> 14);
		}
	}

	Params params_;
	int32_t history_[2][80] = {};
};

// Builds the SBC media payload header (A2DP 4.3.4): F=S=L=0, frame count.
inline uint8_t MediaPayloadHeader(size_t frameCount) noexcept { return static_cast<uint8_t>(frameCount & 0x0F); }

// How many frames fit in a media payload of `payloadCapacity` bytes (after RTP).
inline size_t FramesPerPacket(const Params& p, size_t payloadCapacity) noexcept
{
	if (payloadCapacity <= 1) {
		return 0;
	}
	const size_t n = (payloadCapacity - 1) / FrameLength(p);
	return n < kMaxFramesPerPacket ? n : kMaxFramesPerPacket;
}

// Interleaved sine generator using a 256-entry table and a 32-bit phase accumulator.
class ToneGenerator {
public:
	void Reset(uint32_t sampleRate, uint32_t frequency, int16_t amplitude) noexcept
	{
		step_ = static_cast<uint32_t>((static_cast<uint64_t>(frequency) << 32) / sampleRate);
		phase_ = 0;
		amplitude_ = amplitude;
	}

	void Fill(int16_t* out, size_t frames, int channels) noexcept
	{
		for (size_t n = 0; n < frames; ++n) {
			const int32_t s = (static_cast<int32_t>(Tables::kSine256Q15[phase_ >> 24]) * amplitude_) >> 15;
			for (int ch = 0; ch < channels; ++ch) {
				out[n * channels + ch] = static_cast<int16_t>(s);
			}
			phase_ += step_;
		}
	}

private:
	uint32_t step_ = 0;
	uint32_t phase_ = 0;
	int16_t amplitude_ = 0;
};

} // namespace OpenA2dp::SbcFixed
