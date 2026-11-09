#include <OpenA2dp/SbcCodec.h>
#include <OpenA2dp/BitWriter.h>
#include <OpenA2dp/SbcFilter.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace OpenA2dp {

using namespace Sbc;

namespace {

struct Matrices {
	double m4[4][8];
	double m8[8][16];

	Matrices() noexcept
	{
		for (int i = 0; i < 4; ++i) {
			for (int k = 0; k < 8; ++k) {
				m4[i][k] = std::cos((i + 0.5) * (k - 2) * std::numbers::pi / 4);
			}
		}
		for (int i = 0; i < 8; ++i) {
			for (int k = 0; k < 16; ++k) {
				m8[i][k] = std::cos((i + 0.5) * (k - 4) * std::numbers::pi / 8);
			}
		}
	}
};

const Matrices& GetMatrices() noexcept
{
	static const Matrices matrices;
	return matrices;
}

uint8_t ScaleFactorFor(double maxAbs) noexcept
{
	uint8_t sf = 0;
	while (sf < 15 && maxAbs >= static_cast<double>(1u << (sf + 1))) {
		++sf;
	}
	return sf;
}

} // namespace

SbcEncoder::SbcEncoder(const FrameParams& params) noexcept : params_(params) {}

PcmFormat SbcEncoder::InputFormat() const noexcept
{
	return PcmFormat{params_.sampleRate, params_.Channels()};
}

uint32_t SbcEncoder::SamplesPerFrame() const noexcept
{
	return static_cast<uint32_t>(params_.blocks) * params_.subbands;
}

size_t SbcEncoder::MaxFrameBytes() const noexcept
{
	return FrameLength(params_);
}

void SbcEncoder::Analyze(const int16_t* pcm, int channel, int block, double* subbandSamples) noexcept
{
	const int m = params_.subbands;
	const int nch = params_.Channels();
	double* x = history_[channel];

	// Shift in M new samples: X[M-1] receives the oldest, X[0] the newest.
	for (int i = 10 * m - 1; i >= m; --i) {
		x[i] = x[i - m];
	}
	const int16_t* in = pcm + static_cast<size_t>(block) * m * nch + channel;
	for (int i = m - 1; i >= 0; --i) {
		x[i] = *in;
		in += nch;
	}

	// The same Q30 prototypes the kernel encoder multiplies; scaled here because this
	// filter accumulates in double.
	const int32_t* c = m == 4 ? SbcFilter::kProto4Q30 : SbcFilter::kProto8Q30;
	const double q30 = 1.0 / static_cast<double>(SbcFilter::kQ30One);
	double y[16] = {};
	for (int i = 0; i < 2 * m; ++i) {
		double sum = 0.0;
		for (int j = 0; j < 5; ++j) {
			const int idx = i + j * 2 * m;
			sum += static_cast<double>(c[idx]) * x[idx];
		}
		y[i] = sum * q30;
	}

	const auto& mats = GetMatrices();
	for (int i = 0; i < m; ++i) {
		double sum = 0.0;
		for (int k = 0; k < 2 * m; ++k) {
			sum += (m == 4 ? mats.m4[i][k] : mats.m8[i][k]) * y[k];
		}
		subbandSamples[i] = sum;
	}
}

HRESULT SbcEncoder::Encode(const int16_t* pcm, uint8_t* out, size_t capacity, size_t& written) noexcept
{
	written = 0;
	const size_t frameLength = FrameLength(params_);
	if (pcm == nullptr || out == nullptr || capacity < frameLength) {
		return E_INVALIDARG;
	}

	const int nsb = params_.subbands;
	const int nblk = params_.blocks;
	const int nch = params_.Channels();

	double samples[16][2][8] = {};
	for (int blk = 0; blk < nblk; ++blk) {
		for (int ch = 0; ch < nch; ++ch) {
			Analyze(pcm, ch, blk, samples[blk][ch]);
		}
	}

	uint8_t scaleFactors[2][8] = {};
	for (int ch = 0; ch < nch; ++ch) {
		for (int sb = 0; sb < nsb; ++sb) {
			double maxAbs = 0.0;
			for (int blk = 0; blk < nblk; ++blk) {
				maxAbs = std::max(maxAbs, std::abs(samples[blk][ch][sb]));
			}
			scaleFactors[ch][sb] = ScaleFactorFor(maxAbs);
		}
	}

	uint8_t join = 0; // bit (nsb-1-sb) set when subband sb is joined
	if (params_.channelMode == ChannelMode::JointStereo) {
		for (int sb = 0; sb < nsb - 1; ++sb) {
			double maxMid = 0.0;
			double maxSide = 0.0;
			for (int blk = 0; blk < nblk; ++blk) {
				const double l = samples[blk][0][sb];
				const double r = samples[blk][1][sb];
				maxMid = std::max(maxMid, std::abs((l + r) * 0.5));
				maxSide = std::max(maxSide, std::abs((l - r) * 0.5));
			}
			const uint8_t sfMid = ScaleFactorFor(maxMid);
			const uint8_t sfSide = ScaleFactorFor(maxSide);
			if (sfMid + sfSide < scaleFactors[0][sb] + scaleFactors[1][sb]) {
				join |= static_cast<uint8_t>(1u << (nsb - 1 - sb));
				scaleFactors[0][sb] = sfMid;
				scaleFactors[1][sb] = sfSide;
				for (int blk = 0; blk < nblk; ++blk) {
					const double l = samples[blk][0][sb];
					const double r = samples[blk][1][sb];
					samples[blk][0][sb] = (l + r) * 0.5;
					samples[blk][1][sb] = (l - r) * 0.5;
				}
			}
		}
	}

	int bits[2][8] = {};
	CalculateBits(params_, scaleFactors, bits);

	BitWriter w(out, capacity);
	const int fsIndex = SampleRateIndex(params_.sampleRate);
	w.Write(kSyncWord, 8);
	w.Write(static_cast<uint32_t>(fsIndex), 2);
	w.Write(static_cast<uint32_t>(nblk / 4 - 1), 2);
	w.Write(static_cast<uint32_t>(params_.channelMode), 2);
	w.Write(static_cast<uint32_t>(params_.allocation), 1);
	w.Write(nsb == 8 ? 1u : 0u, 1);
	w.Write(params_.bitpool, 8);
	w.Write(0, 8); // CRC placeholder

	if (params_.channelMode == ChannelMode::JointStereo) {
		w.Write(join, nsb); // join[0..nsb-2] followed by the RFA bit (always 0)
	}
	for (int ch = 0; ch < nch; ++ch) {
		for (int sb = 0; sb < nsb; ++sb) {
			w.Write(scaleFactors[ch][sb], 4);
		}
	}

	const size_t crcBits = 16 + (params_.channelMode == ChannelMode::JointStereo ? nsb : 0) + 4u * nsb * nch;
	uint8_t crcData[1 + 2 + 1 + 16] = {};
	// CRC covers header bytes 1..2 and then everything after the CRC byte up to the end of the scale factors.
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
				const double scale = static_cast<double>(1u << (scaleFactors[ch][sb] + 1));
				const uint32_t levels = (1u << b) - 1;
				double normalized = samples[blk][ch][sb] / scale;
				normalized = std::clamp(normalized, -1.0, 1.0);
				double q = std::floor((normalized + 1.0) * levels / 2.0);
				q = std::clamp(q, 0.0, static_cast<double>(levels));
				w.Write(static_cast<uint32_t>(q), static_cast<unsigned>(b));
			}
		}
	}
	w.AlignToByte();

	if (w.Overflow() || w.BytesUsed() > frameLength) {
		return E_UNEXPECTED;
	}
	// Pad to the nominal frame length (bit allocation may leave unused bits).
	for (size_t i = w.BytesUsed(); i < frameLength; ++i) {
		out[i] = 0;
	}
	written = frameLength;
	return S_OK;
}

size_t SbcEncoder::FramesPerPacket(size_t payloadCapacity) const noexcept
{
	return SbcFrame::FramesPerPacket(params_.ToConfiguration(), payloadCapacity);
}

size_t SbcEncoder::BuildMediaPayload(const uint8_t* frames, const size_t* frameSizes, size_t frameCount,
									 uint8_t* out, size_t capacity) noexcept
{
	if (frameCount == 0 || frameCount > kMaxFramesPerPacket || frames == nullptr || frameSizes == nullptr) {
		return 0;
	}
	size_t total = 1;
	for (size_t i = 0; i < frameCount; ++i) {
		total += frameSizes[i];
	}
	if (total > capacity) {
		return 0;
	}
	// Media payload header: F=0, S=0, L=0, RFA=0, number of frames.
	out[0] = SbcFrame::MediaPayloadHeader(frameCount);
	memcpy(out + 1, frames, total - 1);
	return total;
}

} // namespace OpenA2dp
