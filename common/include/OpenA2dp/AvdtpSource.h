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
constexpr size_t kMaxLocalSeps = 3;
// SEID 1 is always SBC (mandatory fallback); SEID 2 is AAC; SEID 3 is aptX HD. All are
// always advertised: whether a codec may actually be selected is decided per negotiation
// by the selector, because the encoder service can register after the session was created
// (and can disappear again).
constexpr uint8_t kLocalSeid = 1;
constexpr uint8_t kLocalAacSeid = 2;
constexpr uint8_t kLocalAptxHdSeid = 3;

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

// The state name in logs and in the tools' report. nullptr for a value outside the enum, so
// each caller keeps its own word for that. Full switch on purpose: a state added to the
// enum without a name here does not compile, and the tools no longer index a string array
// whose order has to match the enum.
constexpr const char* SessionStateName(SessionState state) noexcept
{
	switch (state)
	{
	case SessionState::Idle: return "Idle";
	case SessionState::Discovering: return "Discovering";
	case SessionState::GettingCapabilities: return "GettingCapabilities";
	case SessionState::Configuring: return "Configuring";
	case SessionState::Configured: return "Configured";
	case SessionState::Opening: return "Opening";
	case SessionState::Open: return "Open";
	case SessionState::Starting: return "Starting";
	case SessionState::Streaming: return "Streaming";
	case SessionState::Suspending: return "Suspending";
	case SessionState::Closing: return "Closing";
	case SessionState::Aborting: return "Aborting";
	case SessionState::Failed: return "Failed";
	}
	return nullptr;
}

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

// Checks a configuration IE the sink proposed for one of our SEPs, after the session's
// own capability check. Returns 0 or an AVDTP ErrorCode; the sink may retry.
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

	// Adds or replaces an extra local SEP (SEID 2 = AAC, SEID 3 = aptX HD). Ignored for SEID 1.
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
	// Outcome of the last RECONFIGURE; only meaningful once its response arrived.
	bool LastReconfigureAccepted() const noexcept { return lastReconfigureAccepted_; }
	DiscoveryFailure LastDiscoveryFailure() const noexcept { return discoveryFailure_; }
	uint8_t DiscoveryError() const noexcept { return discoveryError_; }
	bool HasDiscoveryError() const noexcept { return hasDiscoveryError_; }
	// True when the last discovery saw a sink SEP advertising this codec, whether or not
	// it was selectable at that moment.
	bool RemoteOffers(CodecType codec) const noexcept
	{
		const unsigned value = static_cast<unsigned>(codec);
		return value < 32 && (remoteCodecs_ & (1u << value)) != 0;
	}

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
		remoteCodecs_ = 0;
		remoteSeid_ = 0;
		localSeid_ = 0;
		selectedIeLength_ = 0;
		selectedRemoteIeLength_ = 0;
		lastReconfigureAccepted_ = false;
		acceptor_ = false;
		explicitConfiguration_ = false;
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

	// Replaces the configuration of the open (not streaming) stream. AVDTP allows
	// RECONFIGURE only in the Open state, so a streaming SEP is SUSPENDed first. A
	// rejected RECONFIGURE keeps the previous configuration.
	size_t Reconfigure(const uint8_t* ie, size_t ieLength, uint8_t* out, size_t cap) noexcept
	{
		if (state_ != SessionState::Open || pending_ || remoteSeid_ == 0 || ie == nullptr || ieLength == 0 ||
			ieLength > kMaxCodecIe) {
			return 0;
		}
		uint8_t payload[5 + kMaxCodecIe];
		size_t n = 0;
		payload[n++] = static_cast<uint8_t>(remoteSeid_ << 2);
		payload[n++] = static_cast<uint8_t>(ServiceCategory::MediaCodec);
		payload[n++] = static_cast<uint8_t>(2 + ieLength);
		payload[n++] = static_cast<uint8_t>(static_cast<uint8_t>(MediaType::Audio) << 4);
		payload[n++] = static_cast<uint8_t>(selectedCodec_);
		for (size_t i = 0; i < ieLength; ++i) {
			payload[n++] = ie[i];
		}
		const size_t sent = Command(Signal::Reconfigure, payload, n, out, cap);
		if (sent == 0) {
			return 0;
		}
		lastReconfigureAccepted_ = false;
		backupIeLength_ = selectedIeLength_;
		for (size_t i = 0; i < selectedIeLength_; ++i) {
			backupIe_[i] = selectedIe_[i];
		}
		selectedIeLength_ = ieLength;
		for (size_t i = 0; i < ieLength; ++i) {
			selectedIe_[i] = ie[i];
		}
		return sent;
	}

	// True when this side closed the stream and kept its sink SEP and capabilities, so a new
	// configuration can be sent without discovering again.
	bool CanReconfigure() const noexcept
	{
		return state_ == SessionState::Idle && remoteSeid_ != 0 && localSeid_ != 0 &&
			   selectedRemoteIeLength_ != 0;
	}

	// SET_CONFIGURATION for the SEP the closed stream used, carrying a new configuration IE.
	// AVDTP forbids changing the sampling frequency or the channel count with RECONFIGURE, so
	// a format change is CLOSE followed by this: no discovery, no new SEP, the same sink
	// connection. A refused configuration leaves the session Failed for the owner to
	// rediscover; the candidate list of the old discovery is not tried, because those
	// configurations do not carry the format that asked for the change.
	size_t Configure(const uint8_t* ie, size_t ieLength, uint8_t* out, size_t cap) noexcept
	{
		if (!CanReconfigure() || pending_ || ie == nullptr || ieLength == 0 || ieLength > kMaxCodecIe) {
			return 0;
		}
		selectedIeLength_ = ieLength;
		for (size_t i = 0; i < ieLength; ++i) {
			selectedIe_[i] = ie[i];
		}
		explicitConfiguration_ = true;
		state_ = SessionState::Configuring;
		const size_t sent = SendSelectedConfiguration(out, cap);
		if (sent == 0) {
			explicitConfiguration_ = false;
			state_ = SessionState::Idle;
		}
		return sent;
	}

	// Rebuilds the configuration IE for the selected sink SEP from the capabilities
	// that SEP advertised and the selector's current input, without changing the
	// session. This is what a RECONFIGURE should carry after a parameter change: the
	// same negotiation as discovery, applied to the SEP already in use - and what a
	// CLOSE for a format change reconfigures the stream with. False when the stream was
	// configured by the sink (acceptor role), where only the sink's own configuration IE
	// is known, or when nothing usable can be built.
	bool RefreshConfiguration(uint8_t* ie, size_t& ieLength) noexcept
	{
		if ((state_ != SessionState::Open && !CanReconfigure()) || pending_ || selector_ == nullptr ||
			selectedRemoteIeLength_ == 0) {
			return false;
		}
		uint8_t config[kMaxCodecIe];
		size_t configLength = 0;
		const uint32_t score = selector_(selectorContext_, selectedCodec_, selectedRemoteIe_, selectedRemoteIeLength_,
										  config, configLength);
		if (score == 0 || configLength == 0 || configLength > kMaxCodecIe) {
			return false;
		}
		for (size_t i = 0; i < configLength; ++i) {
			ie[i] = config[i];
		}
		ieLength = configLength;
		return true;
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
		// What this SEP advertised, kept so a later RECONFIGURE can re-run the
		// selector against the same capabilities.
		uint8_t remoteIe[kMaxCodecIe] = {};
		size_t remoteIeLength;
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
	}	uint8_t LocalSeidForCodec(CodecType codec) const noexcept
	{
		for (size_t i = 0; i < localSepCount_; ++i)
		{
			if (localSeps_[i].codec == codec)
			{
				return localSeps_[i].seid;
			}
		}
		return 0;
	}

	// A2DP has one codec type for every vendor codec, so a vendor configuration only belongs to
	// the local SEP whose own IE carries the same vendor and codec IDs (the first six bytes).
	uint8_t LocalSeidForVendor(const uint8_t* ie, size_t ieLength) const noexcept
	{
		if (ie == nullptr || ieLength < 6)
		{
			return 0;
		}
		for (size_t i = 0; i < localSepCount_; ++i)
		{
			const LocalSep& sep = localSeps_[i];
			if (sep.codec != CodecType::Vendor || sep.ieLength < 6)
			{
				continue;
			}
			bool match = true;
			for (size_t b = 0; b < 6; ++b)
			{
				match = match && sep.ie[b] == ie[b];
			}
			if (match)
			{
				return sep.seid;
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

	// The SET_CONFIGURATION both the discovery path and a re-configuration after CLOSE send:
	// the selected SEP pair, the selected codec and its configuration IE.
	size_t SendSelectedConfiguration(uint8_t* out, size_t cap) noexcept
	{
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
		selectedRemoteIeLength_ = c.remoteIeLength;
		for (size_t i = 0; i < c.remoteIeLength; ++i) {
			selectedRemoteIe_[i] = c.remoteIe[i];
		}
		return SendSelectedConfiguration(out, cap);
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
				explicitConfiguration_ = false;
				return SeidCommand(SessionState::Configuring, SessionState::Opening, Signal::Open, out, cap);
			}
			if (explicitConfiguration_) {
				// The sink refused the configuration built for the closed stream. There is no
				// candidate list to fall back to; the owner has to rediscover.
				explicitConfiguration_ = false;
				break;
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

		case Signal::Reconfigure:
			// The session left Open while this response was pending, which only a sink START
			// during the SUSPEND/reconfigure window does. The stream then runs a configuration
			// this response never confirmed, so a rejection must not be mistaken for silence
			// and an acceptance must not be adopted: fail, and let the owner rediscover from a
			// known state. A response that arrives after our own ABORT is dropped earlier, in
			// OnPacket, because the ABORT took over the pending label and signal.
			if (state_ != SessionState::Open) {
				break;
			}
			lastReconfigureAccepted_ = accepted;
			if (!accepted) {
				selectedIeLength_ = backupIeLength_;
				for (size_t i = 0; i < backupIeLength_; ++i) {
					selectedIe_[i] = backupIe_[i];
				}
			}
			return 0;

		case Signal::Close:
		case Signal::Abort:
			if (state_ == SessionState::Closing || state_ == SessionState::Aborting) {
				// Our own CLOSE keeps the sink SEP and its capabilities, so a format change can
				// configure the same SEP again (Configure). Anything else forgets them.
				const bool initiatorClose = state_ == SessionState::Closing;
				state_ = SessionState::Idle;
				acceptor_ = false;
				if (!initiatorClose) {
					remoteSeid_ = 0;
					localSeid_ = 0;
					selectedRemoteIeLength_ = 0;
				}
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
		if (static_cast<unsigned>(codec) < 32) {
			remoteCodecs_ |= 1u << static_cast<unsigned>(codec);
		}
		if (ie == nullptr || selector_ == nullptr) {
			return;
		}
		const uint8_t localSeid =
			codec == CodecType::Vendor ? LocalSeidForVendor(ie, ieLength) : LocalSeidForCodec(codec);
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
		c.remoteIeLength = ieLength <= kMaxCodecIe ? ieLength : kMaxCodecIe;
		for (size_t i = 0; i < c.remoteIeLength; ++i) {
			c.remoteIe[i] = ie[i];
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
			selectedRemoteIeLength_ = 0;
			acceptor_ = false;
			pending_ = false;
			return Reply(h, MessageType::ResponseAccept, nullptr, 0, out, cap);

		case Signal::Abort:
			state_ = SessionState::Idle;
			remoteSeid_ = 0;
			localSeid_ = 0;
			selectedRemoteIeLength_ = 0;
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
		if (sep->codec == CodecType::Sbc && !IsSubsetOfLocal(*sep, ie, ieLength)) {
			return Reject(h, ErrorCode::UnsupportedConfiguration, out, cap, ServiceCategory::MediaCodec);
		}
		// The owner also checks what the local SEP capabilities cannot express, such as a
		// format the audio endpoint cannot serve. Rejecting here lets the sink retry with
		// another configuration instead of the stream failing once media starts.
		if (validator_ != nullptr) {
			const uint8_t error = validator_(selectorContext_, codec, ie, ieLength);
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
		// Only the sink's proposed configuration is known here, not its
		// capabilities, so RefreshConfiguration cannot rebuild it.
		selectedRemoteIeLength_ = 0;
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
	uint32_t remoteCodecs_ = 0;

	uint8_t remoteSeid_ = 0;
	uint8_t localSeid_ = 0;
	CodecType selectedCodec_ = CodecType::Sbc;
	uint8_t selectedIe_[kMaxCodecIe] = {};
	size_t selectedIeLength_ = 0;
	// The configuration a pending RECONFIGURE replaces, restored if the sink rejects it.
	uint8_t backupIe_[kMaxCodecIe] = {};
	size_t backupIeLength_ = 0;
	bool lastReconfigureAccepted_ = false;
	// Capabilities of the selected sink SEP (initiator role only).
	uint8_t selectedRemoteIe_[kMaxCodecIe] = {};
	size_t selectedRemoteIeLength_ = 0;
	bool acceptor_ = false;
	// The pending SET_CONFIGURATION carries a configuration the owner supplied for a stream
	// it closed (Configure), not a candidate from discovery.
	bool explicitConfiguration_ = false;
};

} // namespace OpenA2dp::Avdtp
