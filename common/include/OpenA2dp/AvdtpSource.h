#pragma once

// AVDTP signaling state machine for the source (INT) side of one stream.
// Kernel-safe: no STL, no exceptions, no allocation. The caller owns the L2CAP
// channels and timers; it passes received signaling packets to OnPacket() and
// sends whatever the session writes to the output buffer.

#include <OpenA2dp/Avdtp.h>

namespace OpenA2dp::Avdtp {

constexpr size_t kMaxSignalPacket = 128;
constexpr size_t kMaxSinkSeps = 8;
constexpr size_t kMaxCodecIe = 32;
constexpr size_t kMaxLocalSeps = 2;
// SEID 1 is always SBC (mandatory fallback); SEID 2 is AAC. Both are always
// advertised: whether AAC may actually be selected is decided per negotiation by
// the selector, because the encoder service can register after the session was
// created (and can disappear again).
constexpr uint8_t kLocalSeid = 1;
constexpr uint8_t kLocalAacSeid = 2;

enum class SessionState : uint8_t {
	Idle,
	Discovering,
	GettingCapabilities,
	Configuring,
	Configured,	// configured by the sink (acceptor role); waiting for OPEN
	Opening,
	Open,		// configured; the caller opens the media channel, then calls Start()
	Starting,
	Streaming,
	Suspending,
	Closing,
	Aborting,
	Failed,
};

enum class DiscoveryFailure : uint8_t {
	None,
	Rejected,
	NoUsableSink,
	MalformedResponse,
	Timeout,
};

// Called for each audio sink SEP. Returns a score (0 = unusable) and writes the
// chosen configuration IE. The highest score wins; ties keep the first SEP.
using ConfigSelector = uint32_t (*)(void* context, CodecType codec, const uint8_t* remoteIe, size_t remoteIeLength,
									uint8_t* configIe, size_t& configIeLength);

// Checks a configuration IE the sink proposed for one of our SEPs. Only used for
// codecs other than SBC (the session validates SBC itself); returns 0 or an
// AVDTP ErrorCode.
using ConfigValidator = uint8_t (*)(void* context, CodecType codec, const uint8_t* ie, size_t ieLength);

class SourceSession {
public:
	// localCodecIe: capabilities advertised for our SEP when the sink queries us.
	SourceSession(CodecType localCodec, const uint8_t* localCodecIe, size_t localCodecIeLength,
				  ConfigSelector selector, void* selectorContext, ConfigValidator validator = nullptr) noexcept
		: selector_(selector), selectorContext_(selectorContext), validator_(validator)
	{
		localSeps_[0].seid = kLocalSeid;
		localSeps_[0].codec = localCodec;
		localSeps_[0].ieLength = localCodecIeLength <= kMaxCodecIe ? localCodecIeLength : kMaxCodecIe;
		for (size_t i = 0; i < localSeps_[0].ieLength; ++i) {
			localSeps_[0].ie[i] = localCodecIe[i];
		}
		localSepCount_ = 1;
	}

	// Adds or replaces an extra local SEP (SEID 2 = AAC). Ignored for SEID 1.
	void SetLocalSep(uint8_t seid, CodecType codec, const uint8_t* ie, size_t ieLength) noexcept
	{
		if (seid == kLocalSeid || ieLength > kMaxCodecIe) {
			return;
		}
		LocalSep* sep = nullptr;
		for (size_t i = 0; i < localSepCount_; ++i) {
			if (localSeps_[i].seid == seid) {
				sep = &localSeps_[i];
			}
		}
		if (sep == nullptr) {
			if (localSepCount_ >= kMaxLocalSeps) {
				return;
			}
			sep = &localSeps_[localSepCount_++];
		}
		sep->seid = seid;
		sep->codec = codec;
		sep->ieLength = ieLength;
		for (size_t i = 0; i < ieLength; ++i) {
			sep->ie[i] = ie[i];
		}
	}

	SessionState State() const noexcept { return state_; }
	uint8_t RemoteSeid() const noexcept { return remoteSeid_; }
	// Local SEP the sink is (or was) configured against; 0 before configuration.
	uint8_t LocalSeid() const noexcept { return localSeid_; }
	CodecType SelectedCodec() const noexcept { return selectedCodec_; }
	const uint8_t* SelectedIe() const noexcept { return selectedIe_; }
	size_t SelectedIeLength() const noexcept { return selectedIeLength_; }
	bool HasPendingCommand() const noexcept { return pending_; }
	// True when the sink configured the stream; the sink then opens the media channel.
	bool IsAcceptor() const noexcept { return acceptor_; }
	DiscoveryFailure LastDiscoveryFailure() const noexcept { return discoveryFailure_; }
	uint8_t DiscoveryError() const noexcept { return discoveryError_; }
	bool HasDiscoveryError() const noexcept { return hasDiscoveryError_; }

	// Begins discovery. Returns the length of the command written to out (0 on bad state).
	size_t Connect(uint8_t* out, size_t cap) noexcept
	{
		if (state_ != SessionState::Idle && state_ != SessionState::Failed) {
			return 0;
		}
		sinkCount_ = 0;
		capsIndex_ = 0;
		candidateCount_ = 0;
		candidateIndex_ = 0;
		remoteSeid_ = 0;
		localSeid_ = 0;
		selectedIeLength_ = 0;
		acceptor_ = false;
		discoveryFailure_ = DiscoveryFailure::None;
		hasDiscoveryError_ = false;
		discoveryError_ = 0;
		state_ = SessionState::Discovering;
		return Command(Signal::Discover, nullptr, 0, out, cap);
	}

	size_t Start(uint8_t* out, size_t cap) noexcept
	{
		return SeidCommand(SessionState::Open, SessionState::Starting, Signal::Start, out, cap);
	}

	size_t Suspend(uint8_t* out, size_t cap) noexcept
	{
		return SeidCommand(SessionState::Streaming, SessionState::Suspending, Signal::Suspend, out, cap);
	}

	size_t Close(uint8_t* out, size_t cap) noexcept
	{
		if (state_ != SessionState::Open && state_ != SessionState::Streaming) {
			return 0;
		}
		return SeidCommand(state_, SessionState::Closing, Signal::Close, out, cap);
	}

	size_t Abort(uint8_t* out, size_t cap) noexcept
	{
		// Valid from any state once a remote SEP has been selected, including while another
		// command is pending and after a failure, so a configured SEP is never left in use.
		if (remoteSeid_ == 0 || state_ == SessionState::Aborting || state_ == SessionState::Idle ||
			state_ < SessionState::Configuring) {
			return 0;
		}
		state_ = SessionState::Aborting;
		const uint8_t seid = static_cast<uint8_t>(remoteSeid_ << 2);
		return Command(Signal::Abort, &seid, 1, out, cap);
	}

	// Called when the response to the pending command did not arrive in time.
	size_t OnTimeout(uint8_t* out, size_t cap) noexcept
	{
		if (!pending_) {
			return 0;
		}
		pending_ = false;
		if (state_ == SessionState::Discovering) {
			discoveryFailure_ = DiscoveryFailure::Timeout;
		}
		if (remoteSeid_ != 0 && state_ != SessionState::Aborting && state_ >= SessionState::Configuring) {
			return SeidCommand(state_, SessionState::Aborting, Signal::Abort, out, cap);
		}
		state_ = SessionState::Failed;
		return 0;
	}

	// Processes one received signaling packet. Returns the length of the packet to send (may be 0).
	size_t OnPacket(const uint8_t* in, size_t len, uint8_t* out, size_t cap) noexcept
	{
		SignalHeader h{};
		if (!ParseSignalHeader(in, len, h)) {
			return 0;
		}
		if (h.messageType == MessageType::Command) {
			return OnCommand(h, in + 2, len - 2, out, cap);
		}
		if (!pending_ || h.transactionLabel != label_ || h.signal != pendingSignal_) {
			return 0;
		}
		pending_ = false;
		const bool accepted = h.messageType == MessageType::ResponseAccept;
		return OnResponse(h.signal, accepted, in + 2, len - 2, out, cap);
	}

private:
	struct LocalSep {
		uint8_t seid;
		CodecType codec;
		uint8_t ie[kMaxCodecIe] = {};
		size_t ieLength;
	};

	struct SinkSep {
		uint8_t seid;
	};

	// One sink SEP we could configure, best score first. Kept after the selection
	// so a rejected SET_CONFIGURATION can fall back to the next candidate (AAC -> SBC).
	struct Candidate {
		uint32_t score;
		uint8_t remoteSeid;
		uint8_t localSeid;
		CodecType codec;
		uint8_t ie[kMaxCodecIe] = {};
		size_t ieLength;
	};

	size_t Command(Signal signal, const uint8_t* payload, size_t payloadLength, uint8_t* out, size_t cap) noexcept
	{
		if (cap < 2 + payloadLength) {
			state_ = SessionState::Failed;
			return 0;
		}
		label_ = static_cast<uint8_t>((label_ + 1) & 0x0F);
		WriteSignalHeader({label_, PacketType::Single, MessageType::Command, signal}, out, cap);
		for (size_t i = 0; i < payloadLength; ++i) {
			out[2 + i] = payload[i];
		}
		pending_ = true;
		pendingSignal_ = signal;
		return 2 + payloadLength;
	}

	size_t SeidCommand(SessionState required, SessionState next, Signal signal, uint8_t* out, size_t cap) noexcept
	{
		if (state_ != required || pending_) {
			return 0;
		}
		state_ = next;
		const uint8_t seid = static_cast<uint8_t>(remoteSeid_ << 2);
		return Command(signal, &seid, 1, out, cap);
	}

	static size_t Reply(const SignalHeader& h, MessageType type, const uint8_t* payload, size_t payloadLength,
						uint8_t* out, size_t cap) noexcept
	{
		if (cap < 2 + payloadLength) {
			return 0;
		}
		WriteSignalHeader({h.transactionLabel, PacketType::Single, type, h.signal}, out, cap);
		for (size_t i = 0; i < payloadLength; ++i) {
			out[2 + i] = payload[i];
		}
		return 2 + payloadLength;
	}

	const LocalSep* FindLocalSep(uint8_t seid) const noexcept
	{
		for (size_t i = 0; i < localSepCount_; ++i) {
			if (localSeps_[i].seid == seid) {
				return &localSeps_[i];
			}
		}
		return nullptr;
	}

	uint8_t LocalSeidForCodec(CodecType codec) const noexcept
	{
		for (size_t i = 0; i < localSepCount_; ++i) {
			if (localSeps_[i].codec == codec) {
				return localSeps_[i].seid;
			}
		}
		return 0;
	}

	bool LocalSepInUse(uint8_t seid) const noexcept
	{
		return state_ >= SessionState::Configuring && state_ < SessionState::Failed && localSeid_ == seid;
	}

	size_t SendNextCapabilities(uint8_t* out, size_t cap) noexcept
	{
		if (capsIndex_ < sinkCount_) {
			const uint8_t seid = static_cast<uint8_t>(sinks_[capsIndex_].seid << 2);
			return Command(Signal::GetCapabilities, &seid, 1, out, cap);
		}
		if (candidateCount_ == 0) {
			state_ = SessionState::Failed;
			return 0;
		}
		state_ = SessionState::Configuring;
		candidateIndex_ = 0;
		return SendConfiguration(out, cap);
	}

	// Configures the current candidate and remembers it as the selected stream.
	size_t SendConfiguration(uint8_t* out, size_t cap) noexcept
	{
		const Candidate& c = candidates_[candidateIndex_];
		selectedCodec_ = c.codec;
		localSeid_ = c.localSeid;
		remoteSeid_ = c.remoteSeid;
		selectedIeLength_ = c.ieLength;
		for (size_t i = 0; i < c.ieLength; ++i) {
			selectedIe_[i] = c.ie[i];
		}

		uint8_t payload[8 + kMaxCodecIe];
		size_t n = 0;
		payload[n++] = static_cast<uint8_t>(remoteSeid_ << 2);
		payload[n++] = static_cast<uint8_t>(localSeid_ << 2);
		payload[n++] = static_cast<uint8_t>(ServiceCategory::MediaTransport);
		payload[n++] = 0;
		payload[n++] = static_cast<uint8_t>(ServiceCategory::MediaCodec);
		payload[n++] = static_cast<uint8_t>(2 + selectedIeLength_);
		payload[n++] = static_cast<uint8_t>(static_cast<uint8_t>(MediaType::Audio) << 4);
		payload[n++] = static_cast<uint8_t>(selectedCodec_);
		for (size_t i = 0; i < selectedIeLength_; ++i) {
			payload[n++] = selectedIe_[i];
		}
		return Command(Signal::SetConfiguration, payload, n, out, cap);
	}

	size_t OnResponse(Signal signal, bool accepted, const uint8_t* p, size_t len, uint8_t* out, size_t cap) noexcept
	{
		switch (signal) {
		case Signal::Discover:
			if (state_ != SessionState::Discovering) {
				break;
			}
			if (!accepted) {
				discoveryFailure_ = DiscoveryFailure::Rejected;
				hasDiscoveryError_ = len != 0;
				if (hasDiscoveryError_) { discoveryError_ = p[0]; }
				break;
			}
			if ((len & 1) != 0) {
				discoveryFailure_ = DiscoveryFailure::MalformedResponse;
				break;
			}
			for (size_t pos = 0; pos + 2 <= len && sinkCount_ < kMaxSinkSeps; pos += 2) {
				const SepInfo s = ParseSepInfo(p + pos);
				if (!s.inUse && s.mediaType == MediaType::Audio && s.sepType == SepType::Sink && s.seid != 0) {
					sinks_[sinkCount_++].seid = s.seid;
				}
			}
			if (sinkCount_ == 0) {
				discoveryFailure_ = DiscoveryFailure::NoUsableSink;
				break;
			}
			state_ = SessionState::GettingCapabilities;
			return SendNextCapabilities(out, cap);

		case Signal::GetCapabilities:
			if (state_ != SessionState::GettingCapabilities) {
				break;
			}
			if (accepted) {
				EvaluateCapabilities(sinks_[capsIndex_].seid, p, len);
			}
			++capsIndex_;
			return SendNextCapabilities(out, cap);

		case Signal::SetConfiguration:
			if (state_ != SessionState::Configuring) {
				break;
			}
			if (accepted) {
				return SeidCommand(SessionState::Configuring, SessionState::Opening, Signal::Open, out, cap);
			}
			// The sink refused this codec; try the next-best candidate (AAC -> SBC).
			if (candidateIndex_ + 1 < candidateCount_) {
				++candidateIndex_;
				return SendConfiguration(out, cap);
			}
			break;

		case Signal::Open:
			if (!accepted || state_ != SessionState::Opening) {
				break;
			}
			state_ = SessionState::Open;
			return 0;

		case Signal::Start:
			if (state_ == SessionState::Starting) {
				state_ = accepted ? SessionState::Streaming : SessionState::Open;
				return 0;
			}
			break;

		case Signal::Suspend:
			if (state_ == SessionState::Suspending) {
				state_ = accepted ? SessionState::Open : SessionState::Streaming;
				return 0;
			}
			break;

		case Signal::Close:
		case Signal::Abort:
			if (state_ == SessionState::Closing || state_ == SessionState::Aborting) {
				state_ = SessionState::Idle;
				remoteSeid_ = 0;
				localSeid_ = 0;
				acceptor_ = false;
				return 0;
			}
			break;

		default:
			break;
		}
		state_ = SessionState::Failed;
		return 0;
	}

	// A sink SEP may advertise more than one media codec, so evaluate every Media
	// Codec entry instead of stopping at the first one. Missing a second entry
	// silently drops a codec the sink supports (AAC listed after SBC, for example).
	void EvaluateCapabilities(uint8_t seid, const uint8_t* caps, size_t len) noexcept
	{
		size_t pos = 0;
		while (pos + 2 <= len) {
			const size_t losc = caps[pos + 1];
			if (pos + 2 + losc > len) {
				return;
			}
			if (static_cast<ServiceCategory>(caps[pos]) == ServiceCategory::MediaCodec && losc >= 2) {
				EvaluateCodec(seid, static_cast<CodecType>(caps[pos + 3]), caps + pos + 4, losc - 2);
			}
			pos += 2 + losc;
		}
	}

	void EvaluateCodec(uint8_t seid, CodecType codec, const uint8_t* ie, size_t ieLength) noexcept
	{
		if (ie == nullptr || selector_ == nullptr) {
			return;
		}
		const uint8_t localSeid = LocalSeidForCodec(codec);
		if (localSeid == 0) {
			return;
		}
		uint8_t config[kMaxCodecIe];
		size_t configLength = 0;
		const uint32_t score = selector_(selectorContext_, codec, ie, ieLength, config, configLength);
		if (score == 0 || configLength == 0 || configLength > kMaxCodecIe) {
			return;
		}

		if (candidateCount_ >= kMaxSinkSeps) {
			if (score <= candidates_[candidateCount_ - 1].score) {
				return;
			}
			--candidateCount_;
		}

		// Insert in descending score order; equal scores keep the earlier SEP.
		size_t pos = candidateCount_;
		while (pos > 0 && candidates_[pos - 1].score < score) {
			candidates_[pos] = candidates_[pos - 1];
			--pos;
		}
		Candidate& c = candidates_[pos];
		c.score = score;
		c.remoteSeid = seid;
		c.localSeid = localSeid;
		c.codec = codec;
		c.ieLength = configLength;
		for (size_t i = 0; i < configLength; ++i) {
			c.ie[i] = config[i];
		}
		++candidateCount_;
	}

	// Commands initiated by the sink.
	size_t OnCommand(const SignalHeader& h, const uint8_t* p, size_t len, uint8_t* out, size_t cap) noexcept
	{
		const uint8_t acpSeid = len >= 1 ? static_cast<uint8_t>(p[0] >> 2) : 0;
		switch (h.signal) {
		case Signal::Discover: {
			uint8_t info[2 * kMaxLocalSeps];
			size_t n = 0;
			for (size_t i = 0; i < localSepCount_; ++i) {
				WriteSepInfo({localSeps_[i].seid, LocalSepInUse(localSeps_[i].seid), MediaType::Audio, SepType::Source},
							 info + n);
				n += 2;
			}
			return Reply(h, MessageType::ResponseAccept, info, n, out, cap);
		}

		case Signal::GetCapabilities:
		case Signal::GetAllCapabilities: {
			const LocalSep* sep = FindLocalSep(acpSeid);
			if (sep == nullptr) {
				return Reject(h, ErrorCode::BadAcpSeid, out, cap);
			}
			uint8_t caps[6 + kMaxCodecIe];
			size_t n = 0;
			caps[n++] = static_cast<uint8_t>(ServiceCategory::MediaTransport);
			caps[n++] = 0;
			caps[n++] = static_cast<uint8_t>(ServiceCategory::MediaCodec);
			caps[n++] = static_cast<uint8_t>(2 + sep->ieLength);
			caps[n++] = static_cast<uint8_t>(static_cast<uint8_t>(MediaType::Audio) << 4);
			caps[n++] = static_cast<uint8_t>(sep->codec);
			for (size_t i = 0; i < sep->ieLength; ++i) {
				caps[n++] = sep->ie[i];
			}
			return Reply(h, MessageType::ResponseAccept, caps, n, out, cap);
		}

		case Signal::Start:
			if (acpSeid != localSeid_ || state_ != SessionState::Open) {
				return Reject(h, acpSeid != localSeid_ ? ErrorCode::BadAcpSeid : ErrorCode::BadState, out, cap,
							  static_cast<ServiceCategory>(0), acpSeid);
			}
			state_ = SessionState::Streaming;
			return Reply(h, MessageType::ResponseAccept, nullptr, 0, out, cap);

		case Signal::Suspend:
			if (acpSeid != localSeid_ || state_ != SessionState::Streaming) {
				return Reject(h, acpSeid != localSeid_ ? ErrorCode::BadAcpSeid : ErrorCode::BadState, out, cap,
							  static_cast<ServiceCategory>(0), acpSeid);
			}
			state_ = SessionState::Open;
			return Reply(h, MessageType::ResponseAccept, nullptr, 0, out, cap);

		case Signal::Close:
			if (acpSeid != localSeid_ || (state_ != SessionState::Open && state_ != SessionState::Streaming)) {
				return Reject(h, acpSeid != localSeid_ ? ErrorCode::BadAcpSeid : ErrorCode::BadState, out, cap);
			}
			state_ = SessionState::Idle;
			remoteSeid_ = 0;
			localSeid_ = 0;
			acceptor_ = false;
			pending_ = false;
			return Reply(h, MessageType::ResponseAccept, nullptr, 0, out, cap);

		case Signal::Abort:
			state_ = SessionState::Idle;
			remoteSeid_ = 0;
			localSeid_ = 0;
			acceptor_ = false;
			pending_ = false;
			return Reply(h, MessageType::ResponseAccept, nullptr, 0, out, cap);

		case Signal::SetConfiguration:
			return OnSetConfiguration(h, p, len, out, cap);

		case Signal::GetConfiguration: {
			if (acpSeid != localSeid_) {
				return Reject(h, ErrorCode::BadAcpSeid, out, cap);
			}
			if (selectedIeLength_ == 0 || state_ < SessionState::Configured || state_ == SessionState::Failed) {
				return Reject(h, ErrorCode::SepNotInUse, out, cap);
			}
			uint8_t config[6 + kMaxCodecIe];
			size_t n = 0;
			config[n++] = static_cast<uint8_t>(ServiceCategory::MediaTransport);
			config[n++] = 0;
			config[n++] = static_cast<uint8_t>(ServiceCategory::MediaCodec);
			config[n++] = static_cast<uint8_t>(2 + selectedIeLength_);
			config[n++] = static_cast<uint8_t>(static_cast<uint8_t>(MediaType::Audio) << 4);
			config[n++] = static_cast<uint8_t>(selectedCodec_);
			for (size_t i = 0; i < selectedIeLength_; ++i) {
				config[n++] = selectedIe_[i];
			}
			return Reply(h, MessageType::ResponseAccept, config, n, out, cap);
		}

		case Signal::Open:
			if (acpSeid != localSeid_ || state_ != SessionState::Configured) {
				return Reject(h, acpSeid != localSeid_ ? ErrorCode::BadAcpSeid : ErrorCode::BadState, out, cap);
			}
			state_ = SessionState::Open;
			return Reply(h, MessageType::ResponseAccept, nullptr, 0, out, cap);

		case Signal::Reconfigure:
		case Signal::SecurityControl:
		case Signal::DelayReport:
			return Reject(h, ErrorCode::NotSupportedCommand, out, cap);

		default:
			return Reply(h, MessageType::GeneralReject, nullptr, 0, out, cap);
		}
	}

	// SET_CONFIGURATION from the sink: [acpSeid][intSeid][capabilities...]. Takes over any
	// discovery this side started; rejected once our own configuration is under way.
	size_t OnSetConfiguration(const SignalHeader& h, const uint8_t* p, size_t len, uint8_t* out, size_t cap) noexcept
	{
		if (len < 2) {
			return Reject(h, ErrorCode::BadLength, out, cap);
		}
		const uint8_t acpSeid = static_cast<uint8_t>(p[0] >> 2);
		const LocalSep* sep = FindLocalSep(acpSeid);
		if (sep == nullptr) {
			return Reject(h, ErrorCode::BadAcpSeid, out, cap);
		}
		if (state_ != SessionState::Idle && state_ != SessionState::Failed &&
			state_ != SessionState::Discovering && state_ != SessionState::GettingCapabilities) {
			return Reject(h, ErrorCode::SepInUse, out, cap);
		}
		CodecType codec{};
		size_t ieLength = 0;
		const uint8_t* ie = FindMediaCodec(p + 2, len - 2, codec, ieLength);
		if (ie == nullptr) {
			return Reject(h, ErrorCode::BadPayloadFormat, out, cap, ServiceCategory::MediaCodec);
		}
		if (codec != sep->codec) {
			return Reject(h, ErrorCode::NotSupportedCodecType, out, cap, ServiceCategory::MediaCodec);
		}
		if (sep->codec == CodecType::Sbc) {
			if (!IsSubsetOfLocal(*sep, ie, ieLength)) {
				return Reject(h, ErrorCode::UnsupportedConfiguration, out, cap, ServiceCategory::MediaCodec);
			}
		} else {
			const uint8_t error = validator_ != nullptr ? validator_(selectorContext_, codec, ie, ieLength) : 0;
			if (error != 0) {
				return Reject(h, static_cast<ErrorCode>(error), out, cap, ServiceCategory::MediaCodec);
			}
		}

		pending_ = false;
		discoveryFailure_ = DiscoveryFailure::None;
		acceptor_ = true;
		remoteSeid_ = static_cast<uint8_t>(p[1] >> 2);
		localSeid_ = acpSeid;
		selectedCodec_ = codec;
		selectedIeLength_ = ieLength <= kMaxCodecIe ? ieLength : kMaxCodecIe;
		for (size_t i = 0; i < selectedIeLength_; ++i) {
			selectedIe_[i] = ie[i];
		}
		state_ = SessionState::Configured;
		return Reply(h, MessageType::ResponseAccept, nullptr, 0, out, cap);
	}

	// SBC-style IE check: the bit fields must be offered locally; for SBC the
	// bitpool range (bytes 2..3) must lie inside the local range.
	static bool IsSubsetOfLocal(const LocalSep& sep, const uint8_t* ie, size_t len) noexcept
	{
		if (sep.codec == CodecType::Sbc && sep.ieLength == 4 && len == 4) {
			return (ie[0] & ~sep.ie[0]) == 0 && (ie[1] & ~sep.ie[1]) == 0 &&
				   ie[2] >= sep.ie[2] && ie[3] <= sep.ie[3] && ie[2] <= ie[3];
		}
		if (len != sep.ieLength) {
			return false;
		}
		for (size_t i = 0; i < len; ++i) {
			if ((ie[i] & ~sep.ie[i]) != 0) {
				return false;
			}
		}
		return true;
	}

	// Reject payload layout depends on the signal (AVDTP 8.x).
	static size_t Reject(const SignalHeader& h, ErrorCode error, uint8_t* out, size_t cap,
						 ServiceCategory category = static_cast<ServiceCategory>(0),
						 uint8_t seid = kLocalSeid) noexcept
	{
		uint8_t payload[2];
		size_t n = 0;
		if (h.signal == Signal::SetConfiguration || h.signal == Signal::Reconfigure) {
			payload[n++] = static_cast<uint8_t>(category);
		} else if (h.signal == Signal::Start || h.signal == Signal::Suspend) {
			payload[n++] = static_cast<uint8_t>(seid << 2);
		}
		payload[n++] = static_cast<uint8_t>(error);
		return Reply(h, MessageType::ResponseReject, payload, n, out, cap);
	}

	LocalSep localSeps_[kMaxLocalSeps] = {};
	size_t localSepCount_ = 0;
	ConfigSelector selector_;
	void* selectorContext_;
	ConfigValidator validator_;

	SessionState state_ = SessionState::Idle;
	DiscoveryFailure discoveryFailure_ = DiscoveryFailure::None;
	uint8_t discoveryError_ = 0;
	bool hasDiscoveryError_ = false;
	uint8_t label_ = 0;
	bool pending_ = false;
	Signal pendingSignal_ = Signal::Discover;

	SinkSep sinks_[kMaxSinkSeps] = {};
	size_t sinkCount_ = 0;
	size_t capsIndex_ = 0;
	Candidate candidates_[kMaxSinkSeps] = {};
	size_t candidateCount_ = 0;
	size_t candidateIndex_ = 0;

	uint8_t remoteSeid_ = 0;
	uint8_t localSeid_ = 0;
	CodecType selectedCodec_ = CodecType::Sbc;
	uint8_t selectedIe_[kMaxCodecIe] = {};
	size_t selectedIeLength_ = 0;
	bool acceptor_ = false;
};

} // namespace OpenA2dp::Avdtp
