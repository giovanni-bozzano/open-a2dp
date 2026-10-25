#include "TestFramework.h"

#include <OpenA2dp/Avdtp.h>
#include <OpenA2dp/IA2dpCodec.h>
#include <OpenA2dp/RingBuffer.h>
#include <OpenA2dp/Rtp.h>
#include <OpenA2dp/SbcCodec.h>

#include <thread>
#include <vector>

using namespace OpenA2dp;

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
