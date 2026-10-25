#include "TestFramework.h"

#include <OpenA2dp/AvdtpSource.h>
#include <OpenA2dp/SbcCodec.h>

#include <cstring>

using namespace OpenA2dp;
using namespace OpenA2dp::Avdtp;

namespace {

const uint8_t kSbcLocalIe[4] = {0x3F, 0xFF, 0x02, 0xFA};

// Picks SBC (score 1) or AAC (score 2) and writes a fixed config IE.
uint32_t TestSelector(void*, CodecType codec, const uint8_t*, size_t, uint8_t* config, size_t& configLength)
{
	if (codec == CodecType::Sbc) {
		const uint8_t ie[4] = {0x21, 0x15, 0x02, 0x35};
		std::memcpy(config, ie, sizeof(ie));
		configLength = sizeof(ie);
		return 1;
	}
	if (codec == CodecType::Aac) {
		const uint8_t ie[6] = {0x80, 0x01, 0x04, 0x84, 0xE8, 0x00};
		std::memcpy(config, ie, sizeof(ie));
		configLength = sizeof(ie);
		return 2;
	}
	return 0;
}

SignalHeader Header(const uint8_t* p)
{
	SignalHeader h{};
	ParseSignalHeader(p, 2, h);
	return h;
}

size_t Accept(const uint8_t* cmd, const uint8_t* payload, size_t payloadLength, uint8_t* out)
{
	const SignalHeader c = Header(cmd);
	WriteSignalHeader({c.transactionLabel, PacketType::Single, MessageType::ResponseAccept, c.signal}, out, 2);
	if (payloadLength != 0) {
		std::memcpy(out + 2, payload, payloadLength);
	}
	return 2 + payloadLength;
}

size_t CodecCaps(CodecType codec, const uint8_t* ie, size_t ieLength, uint8_t* out)
{
	size_t n = 0;
	out[n++] = 0x01;
	out[n++] = 0x00;
	out[n++] = 0x07;
	out[n++] = static_cast<uint8_t>(2 + ieLength);
	out[n++] = 0x00;
	out[n++] = static_cast<uint8_t>(codec);
	std::memcpy(out + n, ie, ieLength);
	return n + ieLength;
}

// Drives a session to Open against a sink with SEID 3 (SBC) and SEID 5 (AAC).
void ConnectToOpen(SourceSession& s)
{
	uint8_t cmd[kMaxSignalPacket];
	uint8_t rsp[kMaxSignalPacket];
	uint8_t payload[64];

	size_t n = s.Connect(cmd, sizeof(cmd));
	REQUIRE(n == 2);
	CHECK(Header(cmd).signal == Signal::Discover);

	uint8_t seps[6];
	WriteSepInfo({3, false, MediaType::Audio, SepType::Sink}, seps);
	WriteSepInfo({4, false, MediaType::Audio, SepType::Source}, seps + 2);
	WriteSepInfo({5, false, MediaType::Audio, SepType::Sink}, seps + 4);
	n = s.OnPacket(rsp, Accept(cmd, seps, sizeof(seps), rsp), cmd, sizeof(cmd));
	REQUIRE(n == 3);
	CHECK(Header(cmd).signal == Signal::GetCapabilities);
	CHECK_EQ(cmd[2], uint8_t{3 << 2});

	const uint8_t sbcIe[4] = {0xFF, 0xFF, 0x02, 0x35};
	n = s.OnPacket(rsp, Accept(cmd, payload, CodecCaps(CodecType::Sbc, sbcIe, 4, payload), rsp), cmd, sizeof(cmd));
	REQUIRE(n == 3);
	CHECK_EQ(cmd[2], uint8_t{5 << 2});

	const uint8_t aacIe[6] = {0x80, 0x01, 0x8C, 0x84, 0xE8, 0x00};
	n = s.OnPacket(rsp, Accept(cmd, payload, CodecCaps(CodecType::Aac, aacIe, 6, payload), rsp), cmd, sizeof(cmd));
	REQUIRE(Header(cmd).signal == Signal::SetConfiguration);
	CHECK_EQ(n, size_t{2 + 8 + 6});
	CHECK_EQ(cmd[2], uint8_t{5 << 2});
	CHECK_EQ(cmd[3], uint8_t{kLocalSeid << 2});
	CHECK_EQ(cmd[9], uint8_t{static_cast<uint8_t>(CodecType::Aac)});
	CHECK(s.State() == SessionState::Configuring);

	n = s.OnPacket(rsp, Accept(cmd, nullptr, 0, rsp), cmd, sizeof(cmd));
	REQUIRE(n == 3);
	CHECK(Header(cmd).signal == Signal::Open);

	n = s.OnPacket(rsp, Accept(cmd, nullptr, 0, rsp), cmd, sizeof(cmd));
	CHECK_EQ(n, size_t{0});
	REQUIRE(s.State() == SessionState::Open);
}

} // namespace

TEST(AvdtpSource_ConnectSelectsBestCodec)
{
	SourceSession s(CodecType::Sbc, kSbcLocalIe, 4, TestSelector, nullptr);
	ConnectToOpen(s);
	CHECK_EQ(s.RemoteSeid(), uint8_t{5});
	CHECK(s.SelectedCodec() == CodecType::Aac);
	CHECK_EQ(s.SelectedIeLength(), size_t{6});
	CHECK(!s.HasPendingCommand());
}

TEST(AvdtpSource_StartSuspendClose)
{
	SourceSession s(CodecType::Sbc, kSbcLocalIe, 4, TestSelector, nullptr);
	ConnectToOpen(s);
	uint8_t cmd[kMaxSignalPacket];
	uint8_t rsp[kMaxSignalPacket];

	REQUIRE(s.Start(cmd, sizeof(cmd)) == 3);
	CHECK(Header(cmd).signal == Signal::Start);
	CHECK_EQ(s.Start(cmd, sizeof(cmd)), size_t{0});
	s.OnPacket(rsp, Accept(cmd, nullptr, 0, rsp), cmd, sizeof(cmd));
	CHECK(s.State() == SessionState::Streaming);

	REQUIRE(s.Suspend(cmd, sizeof(cmd)) == 3);
	s.OnPacket(rsp, Accept(cmd, nullptr, 0, rsp), cmd, sizeof(cmd));
	CHECK(s.State() == SessionState::Open);

	REQUIRE(s.Close(cmd, sizeof(cmd)) == 3);
	CHECK(Header(cmd).signal == Signal::Close);
	s.OnPacket(rsp, Accept(cmd, nullptr, 0, rsp), cmd, sizeof(cmd));
	CHECK(s.State() == SessionState::Idle);
	CHECK_EQ(s.RemoteSeid(), uint8_t{0});
}

TEST(AvdtpSource_IgnoresMismatchedLabel)
{
	SourceSession s(CodecType::Sbc, kSbcLocalIe, 4, TestSelector, nullptr);
	uint8_t cmd[kMaxSignalPacket];
	uint8_t rsp[kMaxSignalPacket];
	REQUIRE(s.Connect(cmd, sizeof(cmd)) == 2);
	const size_t n = Accept(cmd, nullptr, 0, rsp);
	rsp[0] ^= 0x10;
	CHECK_EQ(s.OnPacket(rsp, n, cmd, sizeof(cmd)), size_t{0});
	CHECK(s.State() == SessionState::Discovering);
	CHECK(s.HasPendingCommand());
}

TEST(AvdtpSource_FailsWithoutUsableSink)
{
	SourceSession s(CodecType::Sbc, kSbcLocalIe, 4, TestSelector, nullptr);
	uint8_t cmd[kMaxSignalPacket];
	uint8_t rsp[kMaxSignalPacket];
	REQUIRE(s.Connect(cmd, sizeof(cmd)) == 2);
	uint8_t seps[2];
	WriteSepInfo({3, true, MediaType::Audio, SepType::Sink}, seps);
	CHECK_EQ(s.OnPacket(rsp, Accept(cmd, seps, 2, rsp), cmd, sizeof(cmd)), size_t{0});
	CHECK(s.State() == SessionState::Failed);
}

TEST(AvdtpSource_AnswersSinkCommands)
{
	SourceSession s(CodecType::Sbc, kSbcLocalIe, 4, TestSelector, nullptr);
	uint8_t in[8];
	uint8_t out[kMaxSignalPacket];

	WriteSignalHeader({7, PacketType::Single, MessageType::Command, Signal::Discover}, in, 2);
	REQUIRE(s.OnPacket(in, 2, out, sizeof(out)) == 4);
	CHECK(Header(out).messageType == MessageType::ResponseAccept);
	CHECK_EQ(Header(out).transactionLabel, uint8_t{7});
	const SepInfo info = ParseSepInfo(out + 2);
	CHECK_EQ(info.seid, kLocalSeid);
	CHECK(info.sepType == SepType::Source);

	WriteSignalHeader({8, PacketType::Single, MessageType::Command, Signal::GetCapabilities}, in, 2);
	in[2] = kLocalSeid << 2;
	const size_t n = s.OnPacket(in, 3, out, sizeof(out));
	REQUIRE(n == 2 + 6 + 4);
	CodecType codec{};
	size_t ieLength = 0;
	const uint8_t* ie = FindMediaCodec(out + 2, n - 2, codec, ieLength);
	REQUIRE(ie != nullptr);
	CHECK(codec == CodecType::Sbc);
	CHECK_EQ(ieLength, size_t{4});
	CHECK_EQ(std::memcmp(ie, kSbcLocalIe, 4), 0);

	in[2] = 9 << 2;
	REQUIRE(s.OnPacket(in, 3, out, sizeof(out)) == 3);
	CHECK(Header(out).messageType == MessageType::ResponseReject);
	CHECK_EQ(out[2], uint8_t{static_cast<uint8_t>(ErrorCode::BadAcpSeid)});

	WriteSignalHeader({9, PacketType::Single, MessageType::Command, Signal::SetConfiguration}, in, 2);
	REQUIRE(s.OnPacket(in, 2, out, sizeof(out)) == 4);
	CHECK(Header(out).messageType == MessageType::ResponseReject);
	CHECK_EQ(out[3], uint8_t{static_cast<uint8_t>(ErrorCode::NotSupportedCommand)});
}

TEST(AvdtpSource_TimeoutAbortsAfterConfiguration)
{
	SourceSession s(CodecType::Sbc, kSbcLocalIe, 4, TestSelector, nullptr);
	ConnectToOpen(s);
	uint8_t cmd[kMaxSignalPacket];
	uint8_t rsp[kMaxSignalPacket];
	REQUIRE(s.Start(cmd, sizeof(cmd)) == 3);
	REQUIRE(s.OnTimeout(cmd, sizeof(cmd)) == 3);
	CHECK(Header(cmd).signal == Signal::Abort);
	CHECK(s.State() == SessionState::Aborting);
	s.OnPacket(rsp, Accept(cmd, nullptr, 0, rsp), cmd, sizeof(cmd));
	CHECK(s.State() == SessionState::Idle);

	SourceSession d(CodecType::Sbc, kSbcLocalIe, 4, TestSelector, nullptr);
	REQUIRE(d.Connect(cmd, sizeof(cmd)) == 2);
	CHECK_EQ(d.OnTimeout(cmd, sizeof(cmd)), size_t{0});
	CHECK(d.State() == SessionState::Failed);
}
