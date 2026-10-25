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
constexpr uint8_t kLocalSeid = 1;

enum class SessionState : uint8_t {
	Idle,
	Discovering,
	GettingCapabilities,
	Configuring,
	Opening,
	Open,		// configured; the caller opens the media channel, then calls Start()
	Starting,
	Streaming,
	Suspending,
	Closing,
	Aborting,
	Failed,
};

// Called for each audio sink SEP. Returns a score (0 = unusable) and writes the
// chosen configuration IE. The highest score wins; ties keep the first SEP.
using ConfigSelector = uint32_t (*)(void* context, CodecType codec, const uint8_t* remoteIe, size_t remoteIeLength,
									uint8_t* configIe, size_t& configIeLength);

class SourceSession {
public:
	// localCodecIe: capabilities advertised for our SEP when the sink queries us.
	SourceSession(CodecType localCodec, const uint8_t* localCodecIe, size_t localCodecIeLength,
				  ConfigSelector selector, void* selectorContext) noexcept
		: localCodec_(localCodec), selector_(selector), selectorContext_(selectorContext)
	{
		localIeLength_ = localCodecIeLength <= kMaxCodecIe ? localCodecIeLength : kMaxCodecIe;
		for (size_t i = 0; i < localIeLength_; ++i) {
			localIe_[i] = localCodecIe[i];
		}
	}

	SessionState State() const noexcept { return state_; }
	uint8_t RemoteSeid() const noexcept { return remoteSeid_; }
	CodecType SelectedCodec() const noexcept { return selectedCodec_; }
	const uint8_t* SelectedIe() const noexcept { return selectedIe_; }
	size_t SelectedIeLength() const noexcept { return selectedIeLength_; }
	bool HasPendingCommand() const noexcept { return pending_; }

	// Begins discovery. Returns the length of the command written to out (0 on bad state).
	size_t Connect(uint8_t* out, size_t cap) noexcept
	{
		if (state_ != SessionState::Idle && state_ != SessionState::Failed) {
			return 0;
		}
		sinkCount_ = 0;
		capsIndex_ = 0;
		bestScore_ = 0;
		remoteSeid_ = 0;
		selectedIeLength_ = 0;
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

	// Called when the response to the pending command did not arrive in time.
	size_t OnTimeout(uint8_t* out, size_t cap) noexcept
	{
		if (!pending_) {
			return 0;
		}
		pending_ = false;
		if (remoteSeid_ != 0 && state_ != SessionState::Aborting && state_ >= SessionState::Opening) {
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
	struct SinkSep {
		uint8_t seid;
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

	size_t SendNextCapabilities(uint8_t* out, size_t cap) noexcept
	{
		if (capsIndex_ < sinkCount_) {
			const uint8_t seid = static_cast<uint8_t>(sinks_[capsIndex_].seid << 2);
			return Command(Signal::GetCapabilities, &seid, 1, out, cap);
		}
		if (bestScore_ == 0) {
			state_ = SessionState::Failed;
			return 0;
		}
		state_ = SessionState::Configuring;
		uint8_t payload[8 + kMaxCodecIe];
		size_t n = 0;
		payload[n++] = static_cast<uint8_t>(remoteSeid_ << 2);
		payload[n++] = static_cast<uint8_t>(kLocalSeid << 2);
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
			if (!accepted || state_ != SessionState::Discovering) {
				break;
			}
			for (size_t pos = 0; pos + 2 <= len && sinkCount_ < kMaxSinkSeps; pos += 2) {
				const SepInfo s = ParseSepInfo(p + pos);
				if (!s.inUse && s.mediaType == MediaType::Audio && s.sepType == SepType::Sink && s.seid != 0) {
					sinks_[sinkCount_++].seid = s.seid;
				}
			}
			if (sinkCount_ == 0) {
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
			if (!accepted || state_ != SessionState::Configuring) {
				break;
			}
			return SeidCommand(SessionState::Configuring, SessionState::Opening, Signal::Open, out, cap);

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
				return 0;
			}
			break;

		default:
			break;
		}
		state_ = SessionState::Failed;
		return 0;
	}

	void EvaluateCapabilities(uint8_t seid, const uint8_t* caps, size_t len) noexcept
	{
		CodecType codec{};
		size_t ieLength = 0;
		const uint8_t* ie = FindMediaCodec(caps, len, codec, ieLength);
		if (ie == nullptr || selector_ == nullptr) {
			return;
		}
		uint8_t config[kMaxCodecIe];
		size_t configLength = 0;
		const uint32_t score = selector_(selectorContext_, codec, ie, ieLength, config, configLength);
		if (score <= bestScore_ || configLength == 0 || configLength > kMaxCodecIe) {
			return;
		}
		bestScore_ = score;
		remoteSeid_ = seid;
		selectedCodec_ = codec;
		selectedIeLength_ = configLength;
		for (size_t i = 0; i < configLength; ++i) {
			selectedIe_[i] = config[i];
		}
	}

	// Commands initiated by the sink.
	size_t OnCommand(const SignalHeader& h, const uint8_t* p, size_t len, uint8_t* out, size_t cap) noexcept
	{
		const uint8_t acpSeid = len >= 1 ? static_cast<uint8_t>(p[0] >> 2) : 0;
		switch (h.signal) {
		case Signal::Discover: {
			uint8_t info[2];
			WriteSepInfo({kLocalSeid, state_ >= SessionState::Configuring && state_ < SessionState::Failed,
						  MediaType::Audio, SepType::Source}, info);
			return Reply(h, MessageType::ResponseAccept, info, 2, out, cap);
		}

		case Signal::GetCapabilities:
		case Signal::GetAllCapabilities: {
			if (acpSeid != kLocalSeid) {
				return Reject(h, ErrorCode::BadAcpSeid, out, cap);
			}
			uint8_t caps[6 + kMaxCodecIe];
			size_t n = 0;
			caps[n++] = static_cast<uint8_t>(ServiceCategory::MediaTransport);
			caps[n++] = 0;
			caps[n++] = static_cast<uint8_t>(ServiceCategory::MediaCodec);
			caps[n++] = static_cast<uint8_t>(2 + localIeLength_);
			caps[n++] = static_cast<uint8_t>(static_cast<uint8_t>(MediaType::Audio) << 4);
			caps[n++] = static_cast<uint8_t>(localCodec_);
			for (size_t i = 0; i < localIeLength_; ++i) {
				caps[n++] = localIe_[i];
			}
			return Reply(h, MessageType::ResponseAccept, caps, n, out, cap);
		}

		case Signal::Start:
			if (acpSeid != kLocalSeid || state_ != SessionState::Open) {
				return Reject(h, acpSeid != kLocalSeid ? ErrorCode::BadAcpSeid : ErrorCode::BadState, out, cap);
			}
			state_ = SessionState::Streaming;
			return Reply(h, MessageType::ResponseAccept, nullptr, 0, out, cap);

		case Signal::Suspend:
			if (acpSeid != kLocalSeid || state_ != SessionState::Streaming) {
				return Reject(h, acpSeid != kLocalSeid ? ErrorCode::BadAcpSeid : ErrorCode::BadState, out, cap);
			}
			state_ = SessionState::Open;
			return Reply(h, MessageType::ResponseAccept, nullptr, 0, out, cap);

		case Signal::Close:
			if (acpSeid != kLocalSeid || (state_ != SessionState::Open && state_ != SessionState::Streaming)) {
				return Reject(h, acpSeid != kLocalSeid ? ErrorCode::BadAcpSeid : ErrorCode::BadState, out, cap);
			}
			state_ = SessionState::Idle;
			remoteSeid_ = 0;
			pending_ = false;
			return Reply(h, MessageType::ResponseAccept, nullptr, 0, out, cap);

		case Signal::Abort:
			state_ = SessionState::Idle;
			remoteSeid_ = 0;
			pending_ = false;
			return Reply(h, MessageType::ResponseAccept, nullptr, 0, out, cap);

		case Signal::SetConfiguration:
		case Signal::GetConfiguration:
		case Signal::Reconfigure:
		case Signal::Open:
		case Signal::SecurityControl:
		case Signal::DelayReport:
			// TODO: sink-initiated configuration and delay reporting.
			return Reject(h, ErrorCode::NotSupportedCommand, out, cap);

		default:
			return Reply(h, MessageType::GeneralReject, nullptr, 0, out, cap);
		}
	}

	// Reject payload layout depends on the signal (AVDTP 8.x).
	static size_t Reject(const SignalHeader& h, ErrorCode error, uint8_t* out, size_t cap) noexcept
	{
		uint8_t payload[2];
		size_t n = 0;
		if (h.signal == Signal::SetConfiguration || h.signal == Signal::Reconfigure) {
			payload[n++] = 0;	// service category: none
		} else if (h.signal == Signal::Start || h.signal == Signal::Suspend) {
			payload[n++] = static_cast<uint8_t>(kLocalSeid << 2);
		}
		payload[n++] = static_cast<uint8_t>(error);
		return Reply(h, MessageType::ResponseReject, payload, n, out, cap);
	}

	CodecType localCodec_;
	uint8_t localIe_[kMaxCodecIe] = {};
	size_t localIeLength_ = 0;
	ConfigSelector selector_;
	void* selectorContext_;

	SessionState state_ = SessionState::Idle;
	uint8_t label_ = 0;
	bool pending_ = false;
	Signal pendingSignal_ = Signal::Discover;

	SinkSep sinks_[kMaxSinkSeps] = {};
	size_t sinkCount_ = 0;
	size_t capsIndex_ = 0;
	uint32_t bestScore_ = 0;

	uint8_t remoteSeid_ = 0;
	CodecType selectedCodec_ = CodecType::Sbc;
	uint8_t selectedIe_[kMaxCodecIe] = {};
	size_t selectedIeLength_ = 0;
};

} // namespace OpenA2dp::Avdtp
