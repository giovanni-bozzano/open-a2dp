#include "TestFramework.h"

#include <OpenA2dp/Avdtp.h>
#include <OpenA2dp/IA2dpCodec.h>
#include <OpenA2dp/Ioctl.h>
#include <OpenA2dp/LatencyBudget.h>
#include <OpenA2dp/RingBuffer.h>
#include <OpenA2dp/Rtp.h>
#include <OpenA2dp/SbcCodec.h>

#include <atomic>
#include <thread>
#include <vector>

using namespace OpenA2dp;

// The driver/service ABI must also compile in user mode (Ioctl.h is shared with the
// driver) and a maximum-size frame must fit in one ring record.
static_assert(Ioctl::kMaxFramePayload + sizeof(Ioctl::FrameRecordHeader) <= Ioctl::kMaxFrameRecordBytes);

// The PCM ring is sized for the longest lead a preference allows at the widest format the
// endpoint offers, so a long lead is buffered rather than trimmed or dropped.
TEST(Ioctl_PcmRingHoldsTheLongestLead)
{
	const uint64_t bytesPerSecond =
		static_cast<uint64_t>(LatencyBudget::kPcmRingMaxRate) * LatencyBudget::kPcmRingMaxBytesPerFrame;
	const uint64_t trimLimit = bytesPerSecond *
		LatencyBudget::RingLimitMs(Ioctl::kMaxPrerollMs, LatencyBudget::kPcmSlackMs) / 1000;
	CHECK(trimLimit <= Ioctl::kPcmRingBytes);
	// Past the trim limit a record still fits, so a full ring is never on the edge of the limit.
	CHECK(static_cast<uint64_t>(Ioctl::kPcmRingBytes) - trimLimit >= bytesPerSecond / 100);
}

// The endpoint rates are exactly the ones the encoders take; anything else is not a format it
// can offer, whichever codec asks for it.
TEST(Ioctl_EndpointRates)
{
	CHECK_EQ(Ioctl::kEndpointRateCount, uint32_t{6});
	for (const uint32_t rate : Ioctl::kEndpointRates)
	{
		Ioctl::EndpointFormat format = {};
		format.abiVersion = Ioctl::kAbiVersion;
		format.sampleRate[Ioctl::kCodecSbc] = rate;
		format.channels[Ioctl::kCodecSbc] = 2;
		CHECK(Ioctl::ValidEndpointFormat(format));
	}
	const uint32_t refused[] = {0u, 8000, 22050, 64000, 192000};
	for (const uint32_t rate : refused)
	{
		Ioctl::EndpointFormat format = {};
		format.abiVersion = Ioctl::kAbiVersion;
		format.sampleRate[Ioctl::kCodecSbc] = rate;
		format.channels[Ioctl::kCodecSbc] = 2;
		CHECK(!Ioctl::ValidEndpointFormat(format));
	}
}

TEST(Rtp_WriteParse)
{
	Rtp::Header h{0xBEEF, 0x01020304, 0xCAFEBABE, Rtp::kDynamicPayloadType, false};
	uint8_t buf[12];
	REQUIRE(Rtp::Write(h, buf, sizeof(buf)) == Rtp::kHeaderSize);
	CHECK_EQ(buf[0], uint8_t{0x80});
	CHECK_EQ(buf[1], uint8_t{96});
	Rtp::Header parsed{};
	REQUIRE(Rtp::Parse(buf, sizeof(buf), parsed));
	CHECK_EQ(parsed.sequence, h.sequence);
	CHECK_EQ(parsed.timestamp, h.timestamp);
	CHECK_EQ(parsed.ssrc, h.ssrc);
	CHECK_EQ(Rtp::Write(h, buf, 11), size_t{0});
}

TEST(Avdtp_SignalHeader)
{
	Avdtp::SignalHeader h{5, Avdtp::PacketType::Single, Avdtp::MessageType::ResponseAccept, Avdtp::Signal::GetAllCapabilities};
	uint8_t buf[2];
	REQUIRE(Avdtp::WriteSignalHeader(h, buf, 2) == 2);
	CHECK_EQ(buf[0], uint8_t{0x52});
	CHECK_EQ(buf[1], uint8_t{0x0C});
	Avdtp::SignalHeader parsed{};
	REQUIRE(Avdtp::ParseSignalHeader(buf, 2, parsed));
	CHECK_EQ(parsed.transactionLabel, uint8_t{5});
	CHECK(parsed.signal == Avdtp::Signal::GetAllCapabilities);
	CHECK(parsed.messageType == Avdtp::MessageType::ResponseAccept);
}

TEST(Avdtp_SepInfo)
{
	const uint8_t raw[2] = {0x04, 0x08}; // SEID 1, not in use, audio, sink
	const auto sep = Avdtp::ParseSepInfo(raw);
	CHECK_EQ(sep.seid, uint8_t{1});
	CHECK(!sep.inUse);
	CHECK(sep.mediaType == Avdtp::MediaType::Audio);
	CHECK(sep.sepType == Avdtp::SepType::Sink);
	uint8_t out[2];
	Avdtp::WriteSepInfo(sep, out);
	CHECK_EQ(out[0], raw[0]);
	CHECK_EQ(out[1], raw[1]);
}

TEST(Avdtp_FindMediaCodec)
{
	// Media Transport (empty) + Media Codec (audio, SBC, 4-byte IE) + Delay Reporting.
	const uint8_t caps[] = {0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0xFF, 0xFF, 0x02, 0x35, 0x08, 0x00};
	Avdtp::CodecType codec{};
	size_t ieLen = 0;
	const uint8_t* ie = Avdtp::FindMediaCodec(caps, sizeof(caps), codec, ieLen);
	REQUIRE(ie != nullptr);
	CHECK(codec == Avdtp::CodecType::Sbc);
	CHECK_EQ(ieLen, size_t{4});
	CHECK_EQ(ie[3], uint8_t{0x35});

	const uint8_t truncated[] = {0x07, 0x06, 0x00, 0x00};
	CHECK(Avdtp::FindMediaCodec(truncated, sizeof(truncated), codec, ieLen) == nullptr);
}

TEST(Negotiation_PrefersAacThenSbc)
{
	A2dpCodecConfig remote[2];
	remote[0].codec = CodecType::Sbc;
	remote[0].ieLength = 4;
	const uint8_t sbc[4] = {0xFF, 0xFF, 2, 53};
	std::copy(sbc, sbc + 4, remote[0].ie);
	remote[1].codec = CodecType::Aac;
	remote[1].ieLength = 6;
	const uint8_t aac[6] = {0x80, 0x01, 0x8C, 0x84, 0xE2, 0x00};
	std::copy(aac, aac + 6, remote[1].ie);

	A2dpCodecConfig selected;
	REQUIRE(NegotiateConfiguration(remote, 2, CodecPreference{}, selected));
	CHECK(selected.codec == CodecType::Aac);

	CodecPreference forceSbc;
	forceSbc.hasForcedCodec = true;
	forceSbc.forcedCodec = CodecType::Sbc;
	REQUIRE(NegotiateConfiguration(remote, 2, forceSbc, selected));
	CHECK(selected.codec == CodecType::Sbc);

	REQUIRE(NegotiateConfiguration(remote, 1, CodecPreference{}, selected));
	CHECK(selected.codec == CodecType::Sbc);
}

TEST(Negotiation_ForcedCodecUnavailableFallsBack)
{
	A2dpCodecConfig remote[1];
	remote[0].codec = CodecType::Sbc;
	remote[0].ieLength = 4;
	const uint8_t sbc[4] = {0xFF, 0xFF, 2, 53};
	std::copy(sbc, sbc + 4, remote[0].ie);
	CodecPreference forceAac;
	forceAac.hasForcedCodec = true;
	forceAac.forcedCodec = CodecType::Aac;
	A2dpCodecConfig selected;
	REQUIRE(NegotiateConfiguration(remote, 1, forceAac, selected));
	CHECK(selected.codec == CodecType::Sbc);
}

// The driver and the CLI share this check: a stale or hand-edited preference must never
// reach the render format list or the AVDTP selector.
TEST(Ioctl_PreferenceValidation)
{
	Ioctl::Preference preference = {};
	CHECK(Ioctl::ValidPreference(preference)); // every field at its default

	// kCodecSbc is 0, so a forced preference that leaves the codec at its default means SBC.
	preference.flags = Ioctl::kPreferenceForceCodec;
	CHECK(Ioctl::ValidPreference(preference));
	preference.codec = Ioctl::kCodecAac;
	CHECK(Ioctl::ValidPreference(preference));
	preference.codec = Ioctl::kCodecAptx;
	CHECK(Ioctl::ValidPreference(preference));
	preference.codec = Ioctl::kCodecAptxHd;
	CHECK(Ioctl::ValidPreference(preference));
	preference.codec = 7;
	CHECK(!Ioctl::ValidPreference(preference));
	preference.flags = 0x1u << 1;
	CHECK(!Ioctl::ValidPreference(preference)); // unknown flag
	preference.flags = 0;

	preference.sbcBitpool = 1;
	CHECK(!Ioctl::ValidPreference(preference));
	preference.sbcBitpool = Ioctl::kSbcMaxBitpool;
	CHECK(Ioctl::ValidPreference(preference));
	preference.sbcBitpool = 0;
	preference.sbcBlocks = 6;
	CHECK(!Ioctl::ValidPreference(preference));
	preference.sbcBlocks = 4;
	preference.sbcSubbands = 4;
	preference.sbcAllocation = Ioctl::kSbcAllocationSnr;
	CHECK(Ioctl::ValidPreference(preference));

	// The pacing lead is stored in the same preference: 0 keeps the default, and anything
	// outside the range the sink could survive would be a glitch profile.
	preference.prerollMs = Ioctl::kMinPrerollMs;
	CHECK(Ioctl::ValidPreference(preference));
	CHECK_EQ(Ioctl::EffectivePrerollMs(preference.prerollMs), Ioctl::kMinPrerollMs);
	preference.prerollMs = Ioctl::kMaxPrerollMs;
	CHECK(Ioctl::ValidPreference(preference));
	preference.prerollMs = Ioctl::kMinPrerollMs - 1;
	CHECK(!Ioctl::ValidPreference(preference));
	preference.prerollMs = Ioctl::kMaxPrerollMs + 1;
	CHECK(!Ioctl::ValidPreference(preference));
	preference.prerollMs = 0;
	CHECK(Ioctl::ValidPreference(preference));
	CHECK_EQ(Ioctl::EffectivePrerollMs(0), Ioctl::kPrerollMs);

	// The SBC packet cap: 0 fills the packet, otherwise the range SbcFrame accepts.
	preference.sbcFramesPerPacket = Ioctl::kSbcMaxFramesPerPacket;
	CHECK(Ioctl::ValidPreference(preference));
	preference.sbcFramesPerPacket = Ioctl::kSbcMaxFramesPerPacket + 1;
	CHECK(!Ioctl::ValidPreference(preference));
	preference.sbcFramesPerPacket = 0;
	CHECK(Ioctl::ValidPreference(preference));

}

// The ring limits are the pacing lead plus a slack, so they can never trim the audio the pacer
// is streaming ahead of the clock: a fixed limit silently capped every lead above the 60 ms
// default, on both the PCM and the encoded ring. Both limits also have to hold at least one
// record or packet at the shortest lead the preference allows, or the trim would fire on a
// stream that has nothing wrong with it.
TEST(LatencyBudget_TrimLimitsNeverCapTheLead)
{
	using namespace LatencyBudget;
	constexpr uint32_t kAacRecordMs = 1000 * Ioctl::kAacSamplesPerFrame / 44100;
	for (uint32_t lead = Ioctl::kMinPrerollMs; lead <= Ioctl::kMaxPrerollMs; lead += 10)
	{
		const uint32_t pcm = RingLimitMs(lead, kPcmSlackMs);
		const uint32_t frames = RingLimitMs(lead, kFrameSlackMs);
		CHECK(pcm > lead);
		CHECK(frames > lead);
		// One SBC packet (seven frames of 128 samples) and one AAC record both fit in the
		// shortest limit, so no trim is caused by packetization alone.
		CHECK(pcm >= 1000 * (7 * 128) / 44100);
		CHECK(frames >= kAacRecordMs);
		// The longest limit still fits the rings at 48 kHz stereo, so a long lead is not
		// trimmed back by capacity either.
		CHECK(pcm * 48000 / 1000 * 4 <= Ioctl::kPcmRingBytes);
		CHECK(frames * 320000 / 8000 <= Ioctl::kFrameRingBytes);
	}
}

// A backlog has to persist before audio is dropped for it. One observation over the limit is a
// wobble - the pacer between packets, a record written as the ring emptied - and the gap a trim
// costs is worse than the few milliseconds of latency that would have absorbed it.
TEST(LatencyBudget_TrimNeedsAPersistentBacklog)
{
	using namespace LatencyBudget;
	uint32_t strikes = 0;
	// The first excess is left alone.
	CHECK(!TrimNow(true, strikes));
	// One observation under the limit clears the count, so the next excess starts over.
	CHECK(!TrimNow(false, strikes));
	CHECK_EQ(strikes, uint32_t{0});
	CHECK(!TrimNow(true, strikes));
	// Only the excess that survived an interval is dropped, and a backlog that stays is.
	CHECK(TrimNow(true, strikes));
	CHECK(TrimNow(true, strikes));
}

// What a trim drops back to: the lead itself, so the gap it costs leaves an interval of headroom
// instead of the ring sitting on the edge. Without a lead the limit the caller computed is kept.
TEST(LatencyBudget_TrimTargetIsTheLead)
{
	using namespace LatencyBudget;
	for (uint32_t lead = Ioctl::kMinPrerollMs; lead <= Ioctl::kMaxPrerollMs; lead += 10)
	{
		CHECK_EQ(TrimTargetMs(lead, kPcmSlackMs) + kPcmSlackMs, RingLimitMs(lead, kPcmSlackMs));
		CHECK(TrimTargetMs(lead, kPcmSlackMs) <= RingLimitMs(lead, kPcmSlackMs));
	}
	CHECK_EQ(TrimTargetMs(0, kFrameSlackMs), kFrameSlackMs);
}

// A lead below one access unit can never be served: the pacer would ask for a packet before the
// producer had the interval to fill it, so the floor is one unit plus the copy tick that has to
// produce it. Above the floor the stored value stands, which is what makes it a floor and not a
// target the driver imposes on the setting.
TEST(LatencyBudget_LeadFloorCoversOneAccessUnit)
{
	using namespace LatencyBudget;
	// One SBC packet of seven 128-sample frames is 20.3 ms at 44.1 kHz; it rounds up to 21, so
	// the floor is 41 and not 40.
	CHECK_EQ(LeadFloorMs(7 * 128, 44100), uint32_t{21 + kPcmSlackMs});
	// A whole AAC frame is 23 ms at 44.1 kHz and 21 at 48 kHz: both round up, never down.
	CHECK_EQ(LeadFloorMs(Ioctl::kAacSamplesPerFrame, 44100), uint32_t{24 + kPcmSlackMs});
	CHECK_EQ(LeadFloorMs(Ioctl::kAacSamplesPerFrame, 48000), uint32_t{22 + kPcmSlackMs});
	// One service record at 48 kHz is 480 samples: 10 ms.
	CHECK_EQ(LeadFloorMs(48000 * Ioctl::kServiceRecordMs / 1000, 48000), uint32_t{10 + kPcmSlackMs});
	// Nothing known, nothing raised.
	CHECK_EQ(LeadFloorMs(0, 44100), uint32_t{0});
	CHECK_EQ(LeadFloorMs(1024, 0), uint32_t{0});
	// The shortest lead the setting allows is below every floor, which is why the clamp exists.
	CHECK(LeadFloorMs(Ioctl::kAacSamplesPerFrame, 44100) > Ioctl::kMinPrerollMs);
}

// An underrun is audio the media clock asked for that the ring did not have. The tail of a
// packet is built whole and reaches past the clock into audio the sink is meant to buffer, so
// silence there must not count, or the number the panel shows would fire on a healthy stream.
TEST(LatencyBudget_UnderrunOnlyCountsDueAudio)
{
	using namespace LatencyBudget;
	constexpr uint32_t kPacket = 7 * 128;

	// Nothing at all in the ring: the whole packet is a gap.
	CHECK_EQ(UnderrunSamples(kPacket, kPacket), kPacket);
	// Only 128 samples are due, so 300 missing is still a 128-sample gap: the rest is the tail
	// the sink buffers.
	CHECK_EQ(UnderrunSamples(300, 128), uint32_t{128});
	// A short tail inside the window counts exactly what is missing.
	CHECK_EQ(UnderrunSamples(40, 128), uint32_t{40});
	// Nothing missing, or nothing due, is not an underrun.
	CHECK_EQ(UnderrunSamples(0, 128), uint32_t{0});
	CHECK_EQ(UnderrunSamples(64, 0), uint32_t{0});
}

// A stream (re)starts its lead as silence in front of the buffered audio, so that audio runs
// straight into live audio instead of being followed by padding, and never owes the sink more
// than the lead or the buffered audio: what the clock asks for beyond that is skipped.
TEST(LatencyBudget_RestartPutsTheLeadInFrontOfBufferedAudio)
{
	using namespace LatencyBudget;
	constexpr uint64_t kLead = 2646;
	// At a start the clock asks for exactly the lead.
	CHECK_EQ(RestartAt(kLead, 0, kLead).skipSamples, uint64_t{0});
	CHECK_EQ(RestartAt(kLead, 0, kLead).leadSilenceSamples, kLead);
	CHECK_EQ(RestartAt(kLead, 1000, kLead).leadSilenceSamples, uint64_t{1646});
	// More buffered than the lead: no silence at all, the audio is simply sent.
	CHECK_EQ(RestartAt(kLead, 4000, kLead).leadSilenceSamples, uint64_t{0});
	CHECK_EQ(RestartAt(kLead, 4000, kLead).skipSamples, uint64_t{0});
	// Half a second behind with the ring trimmed to the lead: skip to the lead, owe nothing.
	CHECK_EQ(RestartAt(22050 + kLead, kLead, kLead).skipSamples, uint64_t{22050});
	CHECK_EQ(RestartAt(22050 + kLead, kLead, kLead).leadSilenceSamples, uint64_t{0});
	// Behind with more audio than the lead: the audio is kept and only the rest is skipped.
	CHECK_EQ(RestartAt(22050, 4000, kLead).skipSamples, uint64_t{22050 - 4000});
}

// A stall is a clock that ran further ahead than the sink can buffer with the audio for it gone.
// Running one unit short is not one - the unit is padded and the sink keeps its buffer - and
// neither is a long way behind with the audio still there: that is simply sent.
TEST(LatencyBudget_StalledNeedsTheAudioToBeGone)
{
	using namespace LatencyBudget;
	constexpr uint32_t kRate = 44100;
	constexpr uint32_t kUnit = 1024;
	constexpr uint64_t kLead = 2646;
	constexpr uint64_t kSlack = kRate * kPcmSlackMs / 1000;
	CHECK(Stalled(22050 + kLead, kLead, kLead, kUnit, kRate));
	// Within the lead and its slack: not behind enough to matter.
	CHECK(!Stalled(kLead + kSlack, 0, kLead, kUnit, kRate));
	// Far behind, but every sample is buffered.
	CHECK(!Stalled(22050, 22050, kLead, kUnit, kRate));
	CHECK(!Stalled(22050, 22050 - kUnit - kSlack, kLead, kUnit, kRate));
	CHECK(Stalled(22050, 22050 - kUnit - kSlack - 1, kLead, kUnit, kRate));
}

// A unit waits for its PCM while that PCM is only a copy tick late, and is padded only once the
// clock is a unit plus the copy slack ahead - the point where waiting longer would starve the sink.
TEST(LatencyBudget_UnitWaitsForItsPcmUntilItIsMissing)
{
	using namespace LatencyBudget;
	constexpr uint32_t kRate = 44100;
	constexpr uint32_t kUnit = 1024;
	constexpr uint64_t kSlack = kRate * kPcmSlackMs / 1000;
	// The PCM is there (lead silence counts as PCM): encode.
	CHECK(UnitReady(kUnit, kUnit, 1, kRate));
	// One sample due but the block's PCM is still arriving: wait instead of zero-filling it.
	CHECK(!UnitReady(kUnit - 1, kUnit, 1, kRate));
	CHECK(!UnitReady(0, kUnit, kUnit + kSlack - 1, kRate));
	// A unit plus the slack due and still no PCM: it is missing, so pad it.
	CHECK(UnitReady(0, kUnit, kUnit + kSlack, kRate));
}

TEST(RingBuffer_WrapAround)
{
	RingHeader header;
	RingBufferView::Initialize(&header, 16);
	uint8_t data[16];
	RingBufferView ring(&header, data);

	const uint8_t a[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
	uint8_t out[10] = {};
	CHECK(ring.Write(a, 10));
	CHECK(ring.Read(out, 10));
	CHECK(ring.Write(a, 10)); // wraps
	CHECK(!ring.Write(a, 7));  // full: 10 used, 6 free
	CHECK_EQ(ring.ReadableBytes(), uint32_t{10});
	CHECK(ring.Read(out, 10));
	CHECK(std::equal(a, a + 10, out));
	CHECK_EQ(ring.ReadableBytes(), uint32_t{0});
}

TEST(RingBuffer_ConcurrentStress)
{
	RingHeader header;
	RingBufferView::Initialize(&header, 4096);
	std::vector<uint8_t> data(4096);
	RingBufferView ring(&header, data.data());

	constexpr uint32_t kTotal = 4'000'000;
	bool ok = true;
	std::thread producer([&] {
		uint32_t next = 0;
		uint8_t chunk[97];
		while (next < kTotal) {
			const uint32_t n = std::min<uint32_t>(sizeof(chunk), kTotal - next);
			for (uint32_t i = 0; i < n; ++i) {
				chunk[i] = static_cast<uint8_t>((next + i) * 31);
			}
			if (ring.Write(chunk, n)) {
				next += n;
			} else {
				std::this_thread::yield();
			}
		}
	});
	uint32_t received = 0;
	uint8_t chunk[61];
	while (received < kTotal) {
		const uint32_t n = std::min<uint32_t>(sizeof(chunk), ring.ReadableBytes());
		if (n == 0) {
			std::this_thread::yield();
			continue;
		}
		ring.Read(chunk, n);
		for (uint32_t i = 0; i < n; ++i) {
			ok = ok && chunk[i] == static_cast<uint8_t>((received + i) * 31);
		}
		received += n;
	}
	producer.join();
	CHECK(ok);
}

// The producer drops the oldest frames whenever the ring holds more than a quarter of it - the
// PCM backlog trim under a stalled consumer - while the consumer keeps reading. Every value must
// come out exactly once, read or counted as discarded, and every read must be one contiguous run:
// a torn copy or a tail moved backwards breaks one or the other.
TEST(RingBuffer_ProducerDiscardsUnderAConcurrentReader)
{
	RingHeader header;
	RingBufferView::Initialize(&header, 4096);
	std::vector<uint8_t> data(4096);
	RingBufferView ring(&header, data.data());

	constexpr uint32_t kTotal = 1'000'000;
	std::atomic<bool> done{false};
	uint64_t discarded = 0;
	std::thread producer([&] {
		uint32_t next = 0;
		uint32_t chunk[25];
		while (next < kTotal) {
			const uint32_t n = std::min<uint32_t>(25, kTotal - next);
			for (uint32_t i = 0; i < n; ++i) {
				chunk[i] = next + i;
			}
			if (ring.Write(chunk, n * 4)) {
				next += n;
			} else {
				std::this_thread::yield();
			}
			const uint32_t readable = ring.ReadableBytes();
			if (readable > 1024) {
				discarded += ring.Discard((readable - 512) & ~3u);
			}
		}
		done = true;
	});

	uint64_t read = 0;
	bool contiguous = true;
	bool increasing = true;
	bool started = false;
	uint32_t last = 0;
	uint32_t chunk[16];
	while (!(done && ring.ReadableBytes() == 0)) {
		const uint32_t bytes = ring.ReadUpTo(chunk, sizeof(chunk), 4);
		if (bytes == 0) {
			std::this_thread::yield();
			continue;
		}
		const uint32_t count = bytes / 4;
		for (uint32_t i = 1; i < count; ++i) {
			contiguous = contiguous && chunk[i] == chunk[i - 1] + 1;
		}
		increasing = increasing && (!started || chunk[0] > last);
		started = true;
		last = chunk[count - 1];
		read += count;
	}
	producer.join();
	CHECK(contiguous);
	CHECK(increasing);
	CHECK(discarded > 0);
	CHECK_EQ(read + discarded / 4, uint64_t{kTotal});
}

// A read takes what is there in whole units, so a discard between the caller's check and the read
// costs what it discarded and never the rest of the unit.
TEST(RingBuffer_ReadUpToTakesWhatIsLeft)
{
	RingHeader header;
	RingBufferView::Initialize(&header, 64);
	uint8_t data[64];
	RingBufferView ring(&header, data);
	const uint8_t bytes[30] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
	REQUIRE(ring.Write(bytes, sizeof(bytes)));

	CHECK_EQ(ring.Discard(8), uint32_t{8});
	uint8_t out[32] = {};
	// 22 bytes left: five whole 4-byte units, starting where the discard left off.
	CHECK_EQ(ring.ReadUpTo(out, sizeof(out), 4), uint32_t{20});
	CHECK_EQ(out[0], uint8_t{9});
	CHECK_EQ(ring.ReadableBytes(), uint32_t{2});
	CHECK_EQ(ring.ReadUpTo(out, sizeof(out), 4), uint32_t{0});
	// No more than is readable is ever discarded.
	CHECK_EQ(ring.Discard(100), uint32_t{2});
	CHECK_EQ(ring.ReadableBytes(), uint32_t{0});
}
