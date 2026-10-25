#include "SbcReferenceDecoder.h"

#include <OpenA2dp/BitWriter.h>

#include "sbc/SbcTables.h"

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace OpenA2dp;
using namespace OpenA2dp::Sbc;

namespace TestSupport {

double SbcReferenceDecoder::SynthesisScale(int subbands)
{
	// Synthesis filters are the negated time reverse of the analysis filters
	// (f_i(n) = -h_i(10M - n)), hence the negative gain.
	return -static_cast<double>(subbands);
}

bool SbcReferenceDecoder::DecodeFrame(const uint8_t* data, size_t length, std::vector<int16_t>& pcm, FrameInfo& info)
{
	if (length < 4 || data[0] != kSyncWord) {
		return false;
	}
	BitReader r(data, length);
	r.Read(8);
	FrameParams p;
	constexpr uint32_t rates[] = {16000, 32000, 44100, 48000};
	p.sampleRate = rates[r.Read(2)];
	p.blocks = static_cast<uint8_t>((r.Read(2) + 1) * 4);
	p.channelMode = static_cast<ChannelMode>(r.Read(2));
	p.allocation = static_cast<Allocation>(r.Read(1));
	p.subbands = r.Read(1) ? 8 : 4;
	p.bitpool = static_cast<uint8_t>(r.Read(8));
	const uint8_t crc = static_cast<uint8_t>(r.Read(8));

	const int nsb = p.subbands;
	const int nch = p.Channels();
	const int nblk = p.blocks;

	info.params = p;
	info.length = FrameLength(p);
	if (length < info.length) {
		return false;
	}

	bool join[8] = {};
	if (p.channelMode == ChannelMode::JointStereo) {
		for (int sb = 0; sb < nsb - 1; ++sb) {
			join[sb] = r.Read(1) != 0;
		}
		r.Read(1);
	}
	uint8_t scaleFactors[2][8] = {};
	for (int ch = 0; ch < nch; ++ch) {
		for (int sb = 0; sb < nsb; ++sb) {
			scaleFactors[ch][sb] = static_cast<uint8_t>(r.Read(4));
		}
	}

	const size_t crcBits = 16 + (p.channelMode == ChannelMode::JointStereo ? nsb : 0) + 4u * nsb * nch;
	uint8_t crcData[16] = {data[1], data[2]};
	std::copy(data + 4, data + 4 + (crcBits - 16 + 7) / 8, crcData + 2);
	info.crcOk = Crc8(crcData, crcBits) == crc;

	int bits[2][8] = {};
	CalculateBits(p, scaleFactors, bits);

	double samples[16][2][8] = {};
	for (int blk = 0; blk < nblk; ++blk) {
		for (int ch = 0; ch < nch; ++ch) {
			for (int sb = 0; sb < nsb; ++sb) {
				const int b = bits[ch][sb];
				if (b == 0) {
					samples[blk][ch][sb] = 0.0;
					continue;
				}
				const double levels = static_cast<double>((1u << b) - 1);
				const double scale = static_cast<double>(1u << (scaleFactors[ch][sb] + 1));
				const uint32_t q = r.Read(static_cast<unsigned>(b));
				samples[blk][ch][sb] = scale * ((2.0 * q + 1.0) / levels - 1.0);
			}
		}
		if (p.channelMode == ChannelMode::JointStereo) {
			for (int sb = 0; sb < nsb; ++sb) {
				if (join[sb]) {
					const double m = samples[blk][0][sb];
					const double s = samples[blk][1][sb];
					samples[blk][0][sb] = m + s;
					samples[blk][1][sb] = m - s;
				}
			}
		}
	}
	if (r.Overflow()) {
		return false;
	}

	const double* c = nsb == 4 ? Tables::kProto4 : Tables::kProto8;
	const double gain = SynthesisScale(nsb);
	const size_t base = pcm.size();
	pcm.resize(base + static_cast<size_t>(nblk) * nsb * nch);

	for (int blk = 0; blk < nblk; ++blk) {
		for (int ch = 0; ch < nch; ++ch) {
			// v_ holds the last 10 subband vectors, index 0 = current block.
			double* hist = v_[ch];
			for (int i = 10 * nsb - 1; i >= nsb; --i) {
				hist[i] = hist[i - nsb];
			}
			for (int i = 0; i < nsb; ++i) {
				hist[i] = samples[blk][ch][i];
			}
			for (int j = 0; j < nsb; ++j) {
				double sum = 0.0;
				for (int age = 0; age < 10; ++age) {
					const int n = age * nsb + j;
					// Plain prototype: the analysis table C carries (-1)^floor(n/2M) folding signs.
					const double prototype = ((n / (2 * nsb)) & 1) ? -c[n] : c[n];
					for (int i = 0; i < nsb; ++i) {
						const double f = prototype * std::cos((i + 0.5) * (n + nsb / 2.0) * std::numbers::pi / nsb);
						sum += hist[age * nsb + i] * f;
					}
				}
				sum *= gain;
				const double clamped = std::clamp(std::round(sum), -32768.0, 32767.0);
				pcm[base + (static_cast<size_t>(blk) * nsb + j) * nch + ch] = static_cast<int16_t>(clamped);
			}
		}
	}
	return true;
}

} // namespace TestSupport
