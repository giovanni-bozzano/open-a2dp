#include "TestFramework.h"

#include <OpenA2dp/AacConfig.h>
#include <OpenA2dp/AvdtpSource.h>
#include <OpenA2dp/SbcCodec.h>

#include <cstring>

using namespace OpenA2dp;
using namespace OpenA2dp::Avdtp;

namespace {

const uint8_t kSbcLocalIe[4] = {0x3F, 0xFF, 0x02, 0xFA};
const uint8_t kSbcRemoteCaps[4] = {0xFF, 0xFF, 0x02, 0x35};
const uint8_t kSbcConfig[4] = {0x21, 0x15, 0x02, 0x35};
const uint8_t kAacRemoteCaps[6] = {0x80, 0x01, 0x8C, 0x84, 0xE8, 0x00};
const uint8_t kAacConfig[6] = {0x80, 0x01, 0x04, 0x02, 0xEE, 0x00};

// Picks SBC (score 1) or AAC (score 2) and writes a fixed config IE.
uint32_t TestSelector(void*, CodecType codec, const uint8_t*, size_t, uint8_t* config, size_t& configLength)
{
	if (codec == CodecType::Sbc) {
		std::memcpy(config, kSbcConfig, sizeof(kSbcConfig));
		configLength = sizeof(kSbcConfig);
		return 1;
	}
	if (codec == CodecType::Aac) {
		std::memcpy(config, kAacConfig, sizeof(kAacConfig));
		configLength = sizeof(kAacConfig);
		return 2;
	}
	return 0;
}

// Mirrors the driver while no encoder service is registered: AAC scores 0.
uint32_t SbcOnlySelector(void* context, CodecType codec, const uint8_t* ie, size_t ieLength, uint8_t* config,
						 size_t& configLength)
{
	return codec == CodecType::Sbc ? TestSelector(context, codec, ie, ieLength, config, configLength) : 0;
}

// Mirrors the driver's SBC selection: the request is the context bitpool, lifted to
// the sink's minimum and capped at its maximum for the chosen mode.
struct BitpoolState
{
	uint8_t bitpool;
};

uint32_t BitpoolSelector(void* context, CodecType codec, const uint8_t* remoteIe, size_t remoteIeLength, uint8_t* config,
						 size_t& configLength)
{
	if (codec != CodecType::Sbc || remoteIeLength < 4) {
		return 0;
	}
	const auto* state = static_cast<const BitpoolState*>(context);
	config[0] = remoteIe[0] & kSbcLocalIe[0];
	config[1] = remoteIe[1] & kSbcLocalIe[1];
	config[2] = remoteIe[2];
	config[3] = state->bitpool < remoteIe[2] ? remoteIe[2] : (state->bitpool > remoteIe[3] ? remoteIe[3] : state->bitpool);
	configLength = 4;
	return 1;
}

// Mirrors the driver: only AAC has a validator; the session checks SBC itself.
uint8_t TestValidator(void*, CodecType codec, const uint8_t* ie, size_t ieLength)
{
	if (codec != CodecType::Aac) {
		return static_cast<uint8_t>(ErrorCode::NotSupportedCodecType);
	}
	return AacConfig::Validate(ie, ieLength);
}

// SEID 1 = SBC always; SEID 2 = AAC. The driver always registers both SEPs and
// lets the selector decide whether AAC is usable for the current negotiation.
SourceSession MakeSession()
{
	SourceSession s(CodecType::Sbc, kSbcLocalIe, sizeof(kSbcLocalIe), TestSelector, nullptr, TestValidator);
	s.SetLocalSep(kLocalAacSeid, CodecType::Aac, AacConfig::kLocalCapabilities, AacConfig::kIeLength);
	return s;
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

size_t RejectResponse(const uint8_t* cmd, uint8_t* out)
{
	const SignalHeader c = Header(cmd);
	WriteSignalHeader({c.transactionLabel, PacketType::Single, MessageType::ResponseReject, c.signal}, out, 2);
	return 2;
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

// One sink SEP advertising several codecs in a single capabilities payload.
size_t MultiCodecCaps(uint8_t* out, const CodecType* codecs, const uint8_t* const* ies, const size_t* lengths,
					  size_t count)
{
	size_t n = 0;
	out[n++] = 0x01;
	out[n++] = 0x00;
	for (size_t i = 0; i < count; ++i) {
		out[n++] = 0x07;
		out[n++] = static_cast<uint8_t>(2 + lengths[i]);
		out[n++] = 0x00;
		out[n++] = static_cast<uint8_t>(codecs[i]);
		std::memcpy(out + n, ies[i], lengths[i]);
		n += lengths[i];
	}
	return n;
}

// Drives DISCOVER + GET_CAPABILITIES until the session emits SET_CONFIGURATION.
// Sink SEID 3 is SBC; SEID 5 is AAC and only offered when aac is true.
void DriveToConfiguration(SourceSession& s, bool aac, uint8_t* cmd, uint8_t* rsp, uint8_t* payload)
{
	size_t n = s.Connect(cmd, kMaxSignalPacket);
	REQUIRE(n == 2);
	CHECK(Header(cmd).signal == Signal::Discover);

	uint8_t seps[6];
	WriteSepInfo({3, false, MediaType::Audio, SepType::Sink}, seps);
	WriteSepInfo({4, false, MediaType::Audio, SepType::Source}, seps + 2);
	WriteSepInfo({5, false, MediaType::Audio, SepType::Sink}, seps + 4);
	n = s.OnPacket(rsp, Accept(cmd, seps, aac ? sizeof(seps) : size_t{4}, rsp), cmd, kMaxSignalPacket);
	REQUIRE(n == 3);
	CHECK(Header(cmd).signal == Signal::GetCapabilities);
	CHECK_EQ(cmd[2], uint8_t{3 << 2});

	n = s.OnPacket(rsp, Accept(cmd, payload, CodecCaps(CodecType::Sbc, kSbcRemoteCaps, 4, payload), rsp),
		cmd, kMaxSignalPacket);
	REQUIRE(n != 0);
	if (aac) {
		// The last sink SEP is AAC, so this response still asks for capabilities.
		REQUIRE(n == 3);
		CHECK_EQ(cmd[2], uint8_t{5 << 2});
		n = s.OnPacket(rsp, Accept(cmd, payload, CodecCaps(CodecType::Aac, kAacRemoteCaps, 6, payload), rsp),
			cmd, kMaxSignalPacket);
		REQUIRE(n != 0);
	}
	REQUIRE(Header(cmd).signal == Signal::SetConfiguration);
}

// Drives a session to Open against a sink with SEID 3 (SBC) and SEID 5 (AAC).
void ConnectToOpen(SourceSession& s)
{
	uint8_t cmd[kMaxSignalPacket];
	uint8_t rsp[kMaxSignalPacket];
	uint8_t payload[64];

	DriveToConfiguration(s, true, cmd, rsp, payload);
	REQUIRE(Header(cmd).signal == Signal::SetConfiguration);
	CHECK_EQ(cmd[2], uint8_t{5 << 2});
	CHECK_EQ(cmd[3], uint8_t{kLocalAacSeid << 2});
	CHECK_EQ(cmd[9], uint8_t{static_cast<uint8_t>(CodecType::Aac)});
	CHECK(s.State() == SessionState::Configuring);

	size_t n = s.OnPacket(rsp, Accept(cmd, nullptr, 0, rsp), cmd, kMaxSignalPacket);
	REQUIRE(n == 3);
	CHECK(Header(cmd).signal == Signal::Open);

	n = s.OnPacket(rsp, Accept(cmd, nullptr, 0, rsp), cmd, kMaxSignalPacket);
	CHECK_EQ(n, size_t{0});
	REQUIRE(s.State() == SessionState::Open);
}

} // namespace

TEST(AvdtpSource_ReconfigureKeepsConfigurationOnReject)
{
	SourceSession s = MakeSession();
	uint8_t cmd[kMaxSignalPacket];
	uint8_t rsp[kMaxSignalPacket];
	const uint8_t ie[6] = {0x80, 0x00, 0x84, 0x01, 0xF4, 0x00}; // 48 kHz, mono, 128 kbps

	// Only the Open state may reconfigure.
	CHECK_EQ(s.Reconfigure(ie, 6, cmd, kMaxSignalPacket), size_t{0});

	ConnectToOpen(s);
	const size_t before = s.SelectedIeLength();
	size_t n = s.Reconfigure(ie, 6, cmd, kMaxSignalPacket);
	REQUIRE(n == 2 + 5 + 6);
	CHECK(Header(cmd).signal == Signal::Reconfigure);
	CHECK_EQ(cmd[2], uint8_t{5 << 2});
	CHECK_EQ(cmd[3], uint8_t{static_cast<uint8_t>(ServiceCategory::MediaCodec)});
	CHECK_EQ(cmd[5], uint8_t{static_cast<uint8_t>(MediaType::Audio) << 4});
	CHECK_EQ(cmd[6], uint8_t{static_cast<uint8_t>(CodecType::Aac)});
	CHECK_EQ(s.SelectedIeLength(), size_t{6});

	n = s.OnPacket(rsp, RejectResponse(cmd, rsp), cmd, kMaxSignalPacket);
	CHECK_EQ(n, size_t{0});
	CHECK(s.State() == SessionState::Open);
	CHECK_EQ(s.SelectedIeLength(), before);
	CHECK(!s.LastReconfigureAccepted());

	// Accepted: the new configuration stays and the state does not change.
	n = s.Reconfigure(ie, 6, cmd, kMaxSignalPacket);
	REQUIRE(n != 0);
	n = s.OnPacket(rsp, Accept(cmd, nullptr, 0, rsp), cmd, kMaxSignalPacket);
	CHECK_EQ(n, size_t{0});
	CHECK(s.State() == SessionState::Open);
	CHECK_EQ(s.SelectedIeLength(), size_t{6});
	CHECK(s.LastReconfigureAccepted());
}

TEST(AvdtpSource_RefreshConfigurationRebuildsFromSinkCapabilities)
{
	BitpoolState state{0x35};
	SourceSession s(CodecType::Sbc, kSbcLocalIe, sizeof(kSbcLocalIe), BitpoolSelector, &state);
	uint8_t cmd[kMaxSignalPacket];
	uint8_t rsp[kMaxSignalPacket];
	uint8_t payload[64];
	uint8_t ie[kMaxCodecIe];
	size_t ieLength = 0;

	CHECK(!s.RefreshConfiguration(ie, ieLength)); // nothing configured yet

	DriveToConfiguration(s, false, cmd, rsp, payload);
	REQUIRE(Header(cmd).signal == Signal::SetConfiguration);
	s.OnPacket(rsp, Accept(cmd, nullptr, 0, rsp), cmd, kMaxSignalPacket);
	REQUIRE(Header(cmd).signal == Signal::Open);
	s.OnPacket(rsp, Accept(cmd, nullptr, 0, rsp), cmd, kMaxSignalPacket);
	REQUIRE(s.State() == SessionState::Open);
	CHECK_EQ(s.SelectedIe()[3], uint8_t{0x35});

	// A lower bitpool is rebuilt from the same sink capabilities, and the session is
	// unchanged until the sink accepts the RECONFIGURE.
	state.bitpool = 0x20;
	REQUIRE(s.RefreshConfiguration(ie, ieLength));
	CHECK_EQ(ieLength, size_t{4});
	CHECK_EQ(ie[3], uint8_t{0x20});
	CHECK(std::memcmp(ie, s.SelectedIe(), ieLength) != 0);
	CHECK_EQ(s.SelectedIe()[3], uint8_t{0x35});

	// A request above what the sink advertises is capped, as during discovery.
	state.bitpool = 0xFA;
	REQUIRE(s.RefreshConfiguration(ie, ieLength));
	CHECK_EQ(ie[3], uint8_t{0x35});

	// A pending command blocks the rebuild, since RECONFIGURE needs a settled stream.
	state.bitpool = 0x20;
	REQUIRE(s.Reconfigure(ie, ieLength, cmd, kMaxSignalPacket) != 0);
	CHECK(!s.RefreshConfiguration(ie, ieLength));
	s.OnPacket(rsp, Accept(cmd, nullptr, 0, rsp), cmd, kMaxSignalPacket);
	REQUIRE(s.RefreshConfiguration(ie, ieLength));
}

TEST(AvdtpSource_ConnectSelectsBestCodec)
{
	SourceSession s = MakeSession();
	ConnectToOpen(s);
	CHECK_EQ(s.RemoteSeid(), uint8_t{5});
	CHECK_EQ(s.LocalSeid(), uint8_t{kLocalAacSeid});
	CHECK(s.SelectedCodec() == CodecType::Aac);
	CHECK_EQ(s.SelectedIeLength(), size_t{6});
	CHECK_EQ(std::memcmp(s.SelectedIe(), kAacConfig, sizeof(kAacConfig)), 0);
	CHECK(!s.HasPendingCommand());
}

TEST(AvdtpSource_SelectsSbcWhenSinkHasOnlySbc)
{
	SourceSession s = MakeSession();
	uint8_t cmd[kMaxSignalPacket];
	uint8_t rsp[kMaxSignalPacket];
	uint8_t payload[64];

	DriveToConfiguration(s, false, cmd, rsp, payload);
	CHECK_EQ(cmd[2], uint8_t{3 << 2});
	CHECK_EQ(cmd[3], uint8_t{kLocalSeid << 2});
	CHECK_EQ(cmd[9], uint8_t{static_cast<uint8_t>(CodecType::Sbc)});
	CHECK(s.SelectedCodec() == CodecType::Sbc);
	CHECK_EQ(s.LocalSeid(), uint8_t{kLocalSeid});

	REQUIRE(s.OnPacket(rsp, Accept(cmd, nullptr, 0, rsp), cmd, kMaxSignalPacket) == 3);
	CHECK(Header(cmd).signal == Signal::Open);
	REQUIRE(s.OnPacket(rsp, Accept(cmd, nullptr, 0, rsp), cmd, kMaxSignalPacket) == 0);
	CHECK(s.State() == SessionState::Open);
}

TEST(AvdtpSource_SelectsAacFromSingleSepAdvertisingBothCodecs)
{
	// Some sinks advertise every codec on one SEP instead of one SEP per codec.
	// Only the first Media Codec entry used to be evaluated, which silently lost
	// AAC whenever the sink listed SBC first.
	SourceSession s = MakeSession();
	uint8_t cmd[kMaxSignalPacket];
	uint8_t rsp[kMaxSignalPacket];
	uint8_t payload[64];

	REQUIRE(s.Connect(cmd, kMaxSignalPacket) == 2);
	CHECK(Header(cmd).signal == Signal::Discover);

	uint8_t seps[2];
	WriteSepInfo({3, false, MediaType::Audio, SepType::Sink}, seps);
	REQUIRE(s.OnPacket(rsp, Accept(cmd, seps, sizeof(seps), rsp), cmd, kMaxSignalPacket) == 3);
	CHECK(Header(cmd).signal == Signal::GetCapabilities);
	CHECK_EQ(cmd[2], uint8_t{3 << 2});

	const CodecType codecs[2] = {CodecType::Sbc, CodecType::Aac};
	const uint8_t* ies[2] = {kSbcRemoteCaps, kAacRemoteCaps};
	const size_t lengths[2] = {sizeof(kSbcRemoteCaps), sizeof(kAacRemoteCaps)};
	const size_t capsLength = MultiCodecCaps(payload, codecs, ies, lengths, 2);

	REQUIRE(s.OnPacket(rsp, Accept(cmd, payload, capsLength, rsp), cmd, kMaxSignalPacket) != 0);
	REQUIRE(Header(cmd).signal == Signal::SetConfiguration);
	CHECK_EQ(cmd[2], uint8_t{3 << 2});
	CHECK_EQ(cmd[3], uint8_t{kLocalAacSeid << 2});
	CHECK_EQ(cmd[9], uint8_t{static_cast<uint8_t>(CodecType::Aac)});
	CHECK(s.SelectedCodec() == CodecType::Aac);
}

TEST(AvdtpSource_FallsBackToSbcWhenAacRejected)
{
	SourceSession s = MakeSession();
	uint8_t cmd[kMaxSignalPacket];
	uint8_t rsp[kMaxSignalPacket];
	uint8_t payload[64];

	DriveToConfiguration(s, true, cmd, rsp, payload);
	CHECK_EQ(cmd[2], uint8_t{5 << 2});
	CHECK_EQ(cmd[3], uint8_t{kLocalAacSeid << 2});

	size_t n = s.OnPacket(rsp, RejectResponse(cmd, rsp), cmd, kMaxSignalPacket);
	REQUIRE(n == 2 + 8 + 4);
	REQUIRE(Header(cmd).signal == Signal::SetConfiguration);
	CHECK_EQ(cmd[2], uint8_t{3 << 2});
	CHECK_EQ(cmd[3], uint8_t{kLocalSeid << 2});
	CHECK_EQ(cmd[9], uint8_t{static_cast<uint8_t>(CodecType::Sbc)});
	CHECK(s.State() == SessionState::Configuring);
	CHECK(s.SelectedCodec() == CodecType::Sbc);
	CHECK_EQ(s.LocalSeid(), uint8_t{kLocalSeid});
	CHECK_EQ(s.RemoteSeid(), uint8_t{3});

	REQUIRE(s.OnPacket(rsp, Accept(cmd, nullptr, 0, rsp), cmd, kMaxSignalPacket) == 3);
	CHECK(Header(cmd).signal == Signal::Open);
	REQUIRE(s.OnPacket(rsp, Accept(cmd, nullptr, 0, rsp), cmd, kMaxSignalPacket) == 0);
	CHECK(s.State() == SessionState::Open);
}

TEST(AvdtpSource_RemembersRemoteCodecsTheSelectorSkipped)
{
	// The driver scores AAC 0 while no encoder service is registered; a later
	// registration renegotiates only if the sink advertised AAC.
	SourceSession s(CodecType::Sbc, kSbcLocalIe, sizeof(kSbcLocalIe), SbcOnlySelector, nullptr, TestValidator);
	s.SetLocalSep(kLocalAacSeid, CodecType::Aac, AacConfig::kLocalCapabilities, AacConfig::kIeLength);
	uint8_t cmd[kMaxSignalPacket];
	uint8_t rsp[kMaxSignalPacket];
	uint8_t payload[64];

	DriveToConfiguration(s, true, cmd, rsp, payload);
	CHECK(s.SelectedCodec() == CodecType::Sbc);
	CHECK(s.RemoteOffers(CodecType::Aac));
	CHECK(s.RemoteOffers(CodecType::Sbc));

	SourceSession sbcOnly = MakeSession();
	DriveToConfiguration(sbcOnly, false, cmd, rsp, payload);
	CHECK(!sbcOnly.RemoteOffers(CodecType::Aac));
}

TEST(AvdtpSource_FailsWhenAllCandidatesRejected)
{
	SourceSession s = MakeSession();
	uint8_t cmd[kMaxSignalPacket];
	uint8_t rsp[kMaxSignalPacket];
	uint8_t payload[64];

	DriveToConfiguration(s, true, cmd, rsp, payload);
	s.OnPacket(rsp, RejectResponse(cmd, rsp), cmd, kMaxSignalPacket);
	CHECK(Header(cmd).signal == Signal::SetConfiguration);
	CHECK_EQ(cmd[2], uint8_t{3 << 2});

	CHECK_EQ(s.OnPacket(rsp, RejectResponse(cmd, rsp), cmd, kMaxSignalPacket), size_t{0});
	CHECK(s.State() == SessionState::Failed);
	CHECK(s.LastDiscoveryFailure() == DiscoveryFailure::None);
}

TEST(AvdtpSource_StartSuspendClose)
{
	SourceSession s = MakeSession();
	ConnectToOpen(s);
	uint8_t cmd[kMaxSignalPacket];
	uint8_t rsp[kMaxSignalPacket];

	REQUIRE(s.Start(cmd, sizeof(cmd)) == 3);
	CHECK(Header(cmd).signal == Signal::Start);
	CHECK_EQ(cmd[2], uint8_t{5 << 2});
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
	CHECK_EQ(s.LocalSeid(), uint8_t{0});
}

TEST(AvdtpSource_IgnoresMismatchedLabel)
{
	SourceSession s = MakeSession();
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
	SourceSession s = MakeSession();
	uint8_t cmd[kMaxSignalPacket];
	uint8_t rsp[kMaxSignalPacket];
	REQUIRE(s.Connect(cmd, sizeof(cmd)) == 2);
	uint8_t seps[2];
	WriteSepInfo({3, true, MediaType::Audio, SepType::Sink}, seps);
	CHECK_EQ(s.OnPacket(rsp, Accept(cmd, seps, 2, rsp), cmd, sizeof(cmd)), size_t{0});
	CHECK(s.State() == SessionState::Failed);
	CHECK(s.LastDiscoveryFailure() == DiscoveryFailure::NoUsableSink);
}

TEST(AvdtpSource_DiscoverListsBothSeps)
{
	SourceSession s = MakeSession();
	uint8_t in[kMaxSignalPacket];
	uint8_t out[kMaxSignalPacket];

	WriteSignalHeader({7, PacketType::Single, MessageType::Command, Signal::Discover}, in, 2);
	REQUIRE(s.OnPacket(in, 2, out, sizeof(out)) == 2 + 4);
	CHECK(Header(out).messageType == MessageType::ResponseAccept);

	const SepInfo sbc = ParseSepInfo(out + 2);
	const SepInfo aac = ParseSepInfo(out + 4);
	CHECK_EQ(sbc.seid, kLocalSeid);
	CHECK(sbc.sepType == SepType::Source);
	CHECK(!sbc.inUse);
	CHECK_EQ(aac.seid, kLocalAacSeid);
	CHECK(aac.sepType == SepType::Source);
	CHECK(!aac.inUse);
}

TEST(AvdtpSource_AacCapabilities)
{
	SourceSession s = MakeSession();
	uint8_t in[kMaxSignalPacket];
	uint8_t out[kMaxSignalPacket];

	WriteSignalHeader({8, PacketType::Single, MessageType::Command, Signal::GetCapabilities}, in, 2);
	in[2] = kLocalAacSeid << 2;
	const size_t n = s.OnPacket(in, 3, out, sizeof(out));
	REQUIRE(n == 2 + 6 + AacConfig::kIeLength);
	CodecType codec{};
	size_t ieLength = 0;
	const uint8_t* ie = FindMediaCodec(out + 2, n - 2, codec, ieLength);
	REQUIRE(ie != nullptr);
	CHECK(codec == CodecType::Aac);
	CHECK_EQ(ieLength, AacConfig::kIeLength);
	CHECK_EQ(std::memcmp(ie, AacConfig::kLocalCapabilities, AacConfig::kIeLength), 0);
}

TEST(AvdtpSource_AnswersSinkCommands)
{
	// A session without the AAC SEP (service not registered) advertises SBC only.
	SourceSession s(CodecType::Sbc, kSbcLocalIe, 4, TestSelector, nullptr, TestValidator);
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

	in[2] = kLocalAacSeid << 2;
	REQUIRE(s.OnPacket(in, 3, out, sizeof(out)) == 3);
	CHECK(Header(out).messageType == MessageType::ResponseReject);
	CHECK_EQ(out[2], uint8_t{static_cast<uint8_t>(ErrorCode::BadAcpSeid)});

	in[2] = 9 << 2;
	REQUIRE(s.OnPacket(in, 3, out, sizeof(out)) == 3);
	CHECK(Header(out).messageType == MessageType::ResponseReject);
	CHECK_EQ(out[2], uint8_t{static_cast<uint8_t>(ErrorCode::BadAcpSeid)});

	WriteSignalHeader({9, PacketType::Single, MessageType::Command, Signal::SetConfiguration}, in, 2);
	REQUIRE(s.OnPacket(in, 2, out, sizeof(out)) == 4);
	CHECK(Header(out).messageType == MessageType::ResponseReject);
	CHECK_EQ(out[3], uint8_t{static_cast<uint8_t>(ErrorCode::BadLength)});
}

TEST(AvdtpSource_AbortBeforeTeardown)
{
	SourceSession s = MakeSession();
	ConnectToOpen(s);
	uint8_t cmd[kMaxSignalPacket];
	uint8_t rsp[kMaxSignalPacket];
	REQUIRE(s.Start(cmd, sizeof(cmd)) == 3);
	s.OnPacket(rsp, Accept(cmd, nullptr, 0, rsp), cmd, sizeof(cmd));
	REQUIRE(s.State() == SessionState::Streaming);
	REQUIRE(s.Abort(cmd, sizeof(cmd)) == 3);
	CHECK(Header(cmd).signal == Signal::Abort);
	CHECK(s.State() == SessionState::Aborting);
	s.OnPacket(rsp, Accept(cmd, nullptr, 0, rsp), cmd, sizeof(cmd));
	CHECK(s.State() == SessionState::Idle);

	SourceSession d = MakeSession();
	REQUIRE(d.Connect(cmd, sizeof(cmd)) == 2);
	CHECK_EQ(d.Abort(cmd, sizeof(cmd)), size_t{0});
}

TEST(AvdtpSource_TimeoutAbortsAfterConfiguration)
{
	SourceSession s = MakeSession();
	ConnectToOpen(s);
	uint8_t cmd[kMaxSignalPacket];
	uint8_t rsp[kMaxSignalPacket];
	REQUIRE(s.Start(cmd, sizeof(cmd)) == 3);
	REQUIRE(s.OnTimeout(cmd, sizeof(cmd)) == 3);
	CHECK(Header(cmd).signal == Signal::Abort);
	CHECK(s.State() == SessionState::Aborting);
	s.OnPacket(rsp, Accept(cmd, nullptr, 0, rsp), cmd, sizeof(cmd));
	CHECK(s.State() == SessionState::Idle);

	SourceSession d = MakeSession();
	REQUIRE(d.Connect(cmd, sizeof(cmd)) == 2);
	CHECK_EQ(d.OnTimeout(cmd, sizeof(cmd)), size_t{0});
	CHECK(d.State() == SessionState::Failed);
}

TEST(AvdtpSource_AbortOverridesPendingCommand)
{
	SourceSession s = MakeSession();
	ConnectToOpen(s);
	uint8_t cmd[kMaxSignalPacket];
	uint8_t rsp[kMaxSignalPacket];
	REQUIRE(s.Start(cmd, sizeof(cmd)) == 3);
	REQUIRE(s.State() == SessionState::Starting);
	REQUIRE(s.Abort(cmd, sizeof(cmd)) == 3);
	CHECK(Header(cmd).signal == Signal::Abort);
	CHECK(s.State() == SessionState::Aborting);
	s.OnPacket(rsp, Accept(cmd, nullptr, 0, rsp), cmd, sizeof(cmd));
	CHECK(s.State() == SessionState::Idle);
}

TEST(AvdtpSource_AcceptsSinkConfiguration)
{
	SourceSession s = MakeSession();
	uint8_t in[kMaxSignalPacket];
	uint8_t out[kMaxSignalPacket];

	WriteSignalHeader({1, PacketType::Single, MessageType::Command, Signal::SetConfiguration}, in, 2);
	in[2] = kLocalSeid << 2;
	in[3] = 2 << 2;
	size_t n = 4 + CodecCaps(CodecType::Aac, kSbcConfig, 4, in + 4);
	REQUIRE(s.OnPacket(in, n, out, sizeof(out)) == 4);
	CHECK(Header(out).messageType == MessageType::ResponseReject);
	CHECK_EQ(out[2], uint8_t{static_cast<uint8_t>(ServiceCategory::MediaCodec)});
	CHECK_EQ(out[3], uint8_t{static_cast<uint8_t>(ErrorCode::NotSupportedCodecType)});

	WriteSignalHeader({2, PacketType::Single, MessageType::Command, Signal::Open}, in, 2);
	in[2] = kLocalSeid << 2;
	REQUIRE(s.OnPacket(in, 3, out, sizeof(out)) == 3);
	CHECK(Header(out).messageType == MessageType::ResponseReject);

	WriteSignalHeader({3, PacketType::Single, MessageType::Command, Signal::SetConfiguration}, in, 2);
	in[2] = kLocalSeid << 2;
	in[3] = 2 << 2;
	n = 4 + CodecCaps(CodecType::Sbc, kSbcConfig, 4, in + 4);
	REQUIRE(s.OnPacket(in, n, out, sizeof(out)) == 2);
	CHECK(Header(out).messageType == MessageType::ResponseAccept);
	CHECK(s.State() == SessionState::Configured);
	CHECK(s.IsAcceptor());
	CHECK_EQ(s.RemoteSeid(), uint8_t{2});
	CHECK_EQ(s.LocalSeid(), uint8_t{kLocalSeid});
	CHECK_EQ(std::memcmp(s.SelectedIe(), kSbcConfig, 4), 0);

	WriteSignalHeader({4, PacketType::Single, MessageType::Command, Signal::GetConfiguration}, in, 2);
	in[2] = kLocalSeid << 2;
	n = s.OnPacket(in, 3, out, sizeof(out));
	REQUIRE(n == 2 + 6 + 4);
	CHECK_EQ(std::memcmp(out + 8, kSbcConfig, 4), 0);

	WriteSignalHeader({5, PacketType::Single, MessageType::Command, Signal::Open}, in, 2);
	REQUIRE(s.OnPacket(in, 3, out, sizeof(out)) == 2);
	CHECK(Header(out).messageType == MessageType::ResponseAccept);
	CHECK(s.State() == SessionState::Open);

	// Only the sink's configuration IE is known in this role, so it cannot be rebuilt.
	uint8_t ie[kMaxCodecIe];
	size_t ieLength = 0;
	CHECK(!s.RefreshConfiguration(ie, ieLength));

	REQUIRE(s.Start(out, sizeof(out)) == 3);
	CHECK_EQ(out[2], uint8_t{2 << 2});
}

TEST(AvdtpSource_RejectsSinkConfigurationOutsideAdvertisedRates)
{
	// Mirrors the driver: only 44.1 kHz (with the stereo-capable modes) is advertised,
	// because the endpoint cannot play anything else. A sink that still configures
	// 48 kHz must be refused instead of being let into a stream MediaStart would drop.
	const uint8_t localIe[4] = {0x27, 0xFF, 0x02, 0xFA};
	SourceSession s(CodecType::Sbc, localIe, sizeof(localIe), TestSelector, nullptr, TestValidator);
	uint8_t in[kMaxSignalPacket];
	uint8_t out[kMaxSignalPacket];

	const uint8_t at48k[4] = {0x11, 0x15, 0x02, 0x35};
	WriteSignalHeader({1, PacketType::Single, MessageType::Command, Signal::SetConfiguration}, in, 2);
	in[2] = kLocalSeid << 2;
	in[3] = 2 << 2;
	size_t n = 4 + CodecCaps(CodecType::Sbc, at48k, 4, in + 4);
	REQUIRE(s.OnPacket(in, n, out, sizeof(out)) == 4);
	CHECK(Header(out).messageType == MessageType::ResponseReject);
	CHECK_EQ(out[2], uint8_t{static_cast<uint8_t>(ServiceCategory::MediaCodec)});
	CHECK_EQ(out[3], uint8_t{static_cast<uint8_t>(ErrorCode::UnsupportedConfiguration)});
	CHECK(s.State() == SessionState::Idle);

	const uint8_t at44k[4] = {0x21, 0x15, 0x02, 0x35};
	WriteSignalHeader({2, PacketType::Single, MessageType::Command, Signal::SetConfiguration}, in, 2);
	in[2] = kLocalSeid << 2;
	in[3] = 2 << 2;
	n = 4 + CodecCaps(CodecType::Sbc, at44k, 4, in + 4);
	REQUIRE(s.OnPacket(in, n, out, sizeof(out)) == 2);
	CHECK(Header(out).messageType == MessageType::ResponseAccept);
	CHECK(s.State() == SessionState::Configured);
	CHECK(s.IsAcceptor());
}

TEST(AvdtpSource_AcceptsSinkAacConfiguration)
{
	SourceSession s = MakeSession();
	uint8_t in[kMaxSignalPacket];
	uint8_t out[kMaxSignalPacket];

	WriteSignalHeader({1, PacketType::Single, MessageType::Command, Signal::SetConfiguration}, in, 2);
	in[2] = kLocalAacSeid << 2;
	in[3] = 5 << 2;
	size_t n = 4 + CodecCaps(CodecType::Aac, kAacConfig, sizeof(kAacConfig), in + 4);
	REQUIRE(s.OnPacket(in, n, out, sizeof(out)) == 2);
	CHECK(Header(out).messageType == MessageType::ResponseAccept);
	CHECK(s.State() == SessionState::Configured);
	CHECK(s.IsAcceptor());
	CHECK_EQ(s.LocalSeid(), uint8_t{kLocalAacSeid});
	CHECK_EQ(s.RemoteSeid(), uint8_t{5});
	CHECK(s.SelectedCodec() == CodecType::Aac);
	CHECK_EQ(s.SelectedIeLength(), sizeof(kAacConfig));
	CHECK_EQ(std::memcmp(s.SelectedIe(), kAacConfig, sizeof(kAacConfig)), 0);

	WriteSignalHeader({2, PacketType::Single, MessageType::Command, Signal::GetConfiguration}, in, 2);
	in[2] = kLocalAacSeid << 2;
	n = s.OnPacket(in, 3, out, sizeof(out));
	REQUIRE(n == 2 + 6 + sizeof(kAacConfig));
	CHECK_EQ(std::memcmp(out + 8, kAacConfig, sizeof(kAacConfig)), 0);

	WriteSignalHeader({3, PacketType::Single, MessageType::Command, Signal::Open}, in, 2);
	in[2] = kLocalAacSeid << 2;
	REQUIRE(s.OnPacket(in, 3, out, sizeof(out)) == 2);
	CHECK(Header(out).messageType == MessageType::ResponseAccept);
	CHECK(s.State() == SessionState::Open);

	REQUIRE(s.Start(out, sizeof(out)) == 3);
	CHECK_EQ(out[2], uint8_t{5 << 2});
}

TEST(AvdtpSource_RejectsInvalidAacConfiguration)
{
	SourceSession s = MakeSession();
	uint8_t in[kMaxSignalPacket];
	uint8_t out[kMaxSignalPacket];
	const uint8_t invalid[6] = {0xC0, 0x01, 0x04, 0x02, 0xEE, 0x00};

	WriteSignalHeader({1, PacketType::Single, MessageType::Command, Signal::SetConfiguration}, in, 2);
	in[2] = kLocalAacSeid << 2;
	in[3] = 5 << 2;
	const size_t n = 4 + CodecCaps(CodecType::Aac, invalid, sizeof(invalid), in + 4);
	REQUIRE(s.OnPacket(in, n, out, sizeof(out)) == 4);
	CHECK(Header(out).messageType == MessageType::ResponseReject);
	CHECK_EQ(out[2], uint8_t{static_cast<uint8_t>(ServiceCategory::MediaCodec)});
	CHECK_EQ(out[3], uint8_t{static_cast<uint8_t>(ErrorCode::InvalidObjectType)});
	CHECK(s.State() == SessionState::Idle);
}
