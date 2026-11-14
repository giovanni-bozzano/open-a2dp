#include "MediaInternal.h"

using namespace OpenA2dpDriver::Media;

NTSTATUS MediaInitialize(_In_ DEVICE_CONTEXT* ctx)
{
	WDF_OBJECT_ATTRIBUTES attrs;
	WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
	attrs.ParentObject = ctx->Device;

	NTSTATUS status = WdfWaitLockCreate(&attrs, &ctx->MediaLock);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	WDF_WORKITEM_CONFIG workConfig;
	WDF_WORKITEM_CONFIG_INIT(&workConfig, OnMediaWorkItem);
	status = WdfWorkItemCreate(&workConfig, &attrs, &ctx->MediaWorkItem);
	if (!NT_SUCCESS(status))
	{
		return status;
	}
	// Send completions drive the stream: each one enqueues the next packet.
	ctx->MediaSend.CompletionWorkItem = ctx->MediaWorkItem;

	// One shot: every run arms it again for the next wake this stream actually needs. A
	// periodic timer fired whether or not anything was due, which woke the CPU for nothing
	// about a third of the time and kept a playing stream out of deeper idle states.
	WDF_TIMER_CONFIG timerConfig;
	WDF_TIMER_CONFIG_INIT(&timerConfig, OnMediaTimer);
	return WdfTimerCreate(&timerConfig, &attrs, &ctx->MediaTimer);
}

void MediaStart(_In_ DEVICE_CONTEXT* ctx, Avdtp::CodecType codec, _In_reads_bytes_(ieLength) const UCHAR* ie,
	_In_ SIZE_T ieLength)
{
	// Called under SessionLock. A link that is closing, or a device going away, must
	// not get a stream again after its teardown stopped media.
	if (ctx->SignalingState != ChannelOpen || KeReadStateEvent(&ctx->StopEvent) != 0)
	{
		OA2DP_WARN(ctx, "Media", "not starting, the link is closing");
		return;
	}
	// Take the pacing lead once per stream: the kernel pacer, the service event and the
	// presentation position then all describe the same distance ahead of the media clock.
	{
		KIRQL irql;
		KeAcquireSpinLock(&ctx->PreferenceLock, &irql);
		const uint32_t preferred = ctx->Preference.prerollMs;
		const uint32_t framesPerPacket = ctx->Preference.sbcFramesPerPacket;
		KeReleaseSpinLock(&ctx->PreferenceLock, irql);
		ctx->PacingLeadMs = Ioctl::EffectivePrerollMs(preferred);
		ctx->SbcFramesPerPacket = framesPerPacket;
		// The peak starts from the lead itself, so the report never claims less than the stream
		// always carries, and a previous stream's worst case does not leak into this one.
		ctx->LatencyPeakMs = static_cast<LONG>(ctx->PacingLeadMs);
		ctx->SbcUnderrunPackets = 0;
		ctx->SbcUnderrunLogTime = 0;
	}
	const PinFormat pin = SnapshotPinFormat(ctx);
	// Audio the endpoint played while no stream carried it is the beginning of playback when the
	// endpoint has been heard and the ring still holds it at this stream's own format; the copy
	// keeps that much (bounded to the lead) for exactly this. Anything else predates the stream -
	// another format, or a run that never played - and is dropped here, before any codec queues
	// it.
	const ULONG flushed = AudioDropStalePcm(ctx, pin.sampleRate, pin.bytesPerFrame);
	if (codec == Avdtp::CodecType::Aac)
	{
		StartAac(ctx, ie, ieLength);
		return;
	}
	if (codec == Avdtp::CodecType::Vendor)
	{
		// Every vendor codec shares this type; the IE says which one was negotiated.
		uint8_t capabilities = 0;
		uint8_t channelModes = 0;
		if (Ldac::ParseIe(ie, ieLength, capabilities, channelModes))
		{
			StartLdac(ctx, ie, ieLength);
		}
		else if (AptxHd::ParseIe(ie, ieLength, capabilities))
		{
			StartAptxHd(ctx, ie, ieLength);
		}
		else
		{
			StartAptx(ctx, ie, ieLength);
		}
		return;
	}

	SbcFixed::Params params;
	if (!SbcFixed::ParseIe(ie, ieLength, params))
	{
		OA2DP_WARN(ctx, "Media", "unsupported SBC configuration");
		AbandonStream(ctx);
		return;
	}
	// The encoder reads the PCM the ACX stream produced, so the negotiated format must be
	// the one Windows opened the endpoint with.
	if (params.sampleRate != pin.sampleRate || params.Channels() != pin.channels)
	{
		OA2DP_WARN(ctx, "Media", "SBC %u Hz %u ch does not match the endpoint format %u Hz %u ch",
			params.sampleRate, params.Channels(), pin.sampleRate, static_cast<unsigned>(pin.channels));
		AbandonStream(ctx);
		return;
	}

	WdfWaitLockAcquire(ctx->MediaLock, nullptr);
	ctx->Encoder.Reset(params);
	ctx->MediaBusySince = 0;
	ctx->MediaSendUnchecked = FALSE;
	ctx->ActiveCodec = Ioctl::kCodecSbc;
	ctx->ActiveSampleRate = params.sampleRate;
	ctx->ActiveChannels = params.Channels();
	ctx->ActiveBitrate = SbcFixed::Bitrate(params);
	ctx->ActiveObjectType = 0;
	ctx->RtpSequence = 0;
	ctx->PacketsSent = 0;
	ctx->RtpTimestamp = 0;
	ctx->MediaSamplesSent = 0;
	MediaStatsReset(ctx);
	ctx->MediaStartTime = KeQueryInterruptTime();
	// Hold the lead above one packet: the pacer otherwise asks for audio the copy path has not
	// had an interval to write, and every packet of the stream is padded with silence.
	if (ctx->MediaOutMtu > Rtp::kHeaderSize)
	{
		ApplyLeadFloor(ctx, SbcPacketSamples(ctx));
	}
	// The lead the sink starts with is silence in front of whatever audio the ring already holds,
	// so that audio plays straight into the live audio behind it.
	{
		const RingBufferView ring(ctx->PcmRingHeader, ctx->PcmRingData);
		const uint64_t buffered = pin.bytesPerFrame != 0 ? ring.ReadableBytes() / pin.bytesPerFrame : 0;
		const uint64_t leadSilence = LatencyBudget::LeadSilenceSamples(
			static_cast<uint64_t>(params.sampleRate) * ctx->PacingLeadMs / 1000, buffered);
		ctx->SbcLeadSilenceFrames = static_cast<ULONG>(leadSilence / params.SamplesPerFrame());
	}
	// The copy keeps the endpoint's audio from the first sample this run played and skips only
	// what came before it (CopyConsumedLocked), so what the wait for this stream cost is that
	// skipped audio plus anything flushed above. The run stamp is spent here: a later restart in
	// the same run is a renegotiation, whose wait is not measured from the run edge.
	ULONGLONG skipped = 0;
	ULONGLONG runStamp = 0;
	{
		KIRQL irql;
		KeAcquireSpinLock(&ctx->AudioLock, &irql);
		skipped = ctx->AudioSkippedBytes;
		ctx->AudioSkippedBytes = 0;
		ctx->PcmTrimStrikes = 0;
		ctx->WorstRenderGapMs = 0;
		ctx->LastRenderTime = 0;
		runStamp = ctx->AudioRunStamp;
		ctx->AudioRunStamp = 0;
		KeReleaseSpinLock(&ctx->AudioLock, irql);
	}
	const ULONGLONG lostBytes = static_cast<ULONGLONG>(flushed) + skipped;
	const ULONGLONG lostMs = pin.sampleRate != 0 && pin.bytesPerFrame != 0
		? lostBytes * 1000ull / (static_cast<ULONGLONG>(pin.sampleRate) * pin.bytesPerFrame)
		: 0;
	const ULONGLONG started = KeQueryInterruptTime();
	if (runStamp != 0 && started > runStamp)
	{
		OA2DP_INFO(ctx, "Media", "stream started %I64u ms after the endpoint ran, %I64u ms of endpoint PCM lost",
			(started - runStamp) / 10000ull, lostMs);
	}
	else
	{
		OA2DP_INFO(ctx, "Media", "stream restarted, %I64u ms of endpoint PCM lost", lostMs);
	}
	const ULONG packetBytes = static_cast<ULONG>(SbcFixed::FramesPerPacket(params,
		MediaPayloadCapacity(ctx), ctx->SbcFramesPerPacket) * params.SamplesPerFrame() *
		pin.bytesPerFrame);
	InterlockedExchange(&ctx->MediaPacketPcmBytes, static_cast<LONG>(packetBytes));
	InterlockedExchange(&ctx->StreamFault, 0);
	InterlockedExchange(&ctx->MediaMode, MediaModeSbc);
	SetMediaActive(ctx, true);
	WdfWaitLockRelease(ctx->MediaLock);

	OA2DP_INFO(ctx, "Media",
		"endpoint PCM, SBC %u Hz blocks %u subbands %u mode %u bitpool %u, frame %u bytes, %u bps",
		params.sampleRate, params.blocks, params.subbands, static_cast<unsigned>(params.channelMode),
		params.bitpool, static_cast<unsigned>(SbcFixed::FrameLength(params)), ctx->ActiveBitrate);
	WdfTimerStart(ctx->MediaTimer, 0);
}

void MediaStop(_In_ DEVICE_CONTEXT* ctx)
{
	// Clear the flag first, atomically and without the lock: the media work item and
	// its per-packet checks read it, so the stream stops within one packet and the
	// work item stops taking MediaLock. Taking the lock first would leave this thread
	// behind a work item that keeps running every 10 ms while it holds it.
	const bool wasActive = InterlockedExchange8(reinterpret_cast<volatile CHAR*>(&ctx->MediaActive), 0) != 0;
	InterlockedExchange(&ctx->MediaPacketPcmBytes, 0);
	// A tick that fired while this stop ran belongs to the stream that just ended; the next
	// stream must not treat it as its own clock tick.
	InterlockedExchange(&ctx->MediaTickPending, 0);
	WdfTimerStop(ctx->MediaTimer, TRUE);
	// A run holds the lock only for one bounded packet, so the wait normally ends at once. A run
	// stuck inside the encoder, or in a completion that never arrives, must not turn teardown
	// into a hang: the wait is bounded and the stop then goes ahead without the lock. MediaActive
	// is already clear, so a run that outlives this still sends nothing, and MediaMode is cleared
	// atomically below whatever that run happens to be doing.
	bool locked = false;
	const ULONGLONG deadline = KeQueryInterruptTime() + MediaStopLockLimit;
	for (;;)
	{
		const ULONGLONG now = KeQueryInterruptTime();
		if (now >= deadline)
		{
			break;
		}
		LONGLONG lockTimeout = -static_cast<LONGLONG>(deadline - now);
		if (WdfWaitLockAcquire(ctx->MediaLock, &lockTimeout) == STATUS_SUCCESS)
		{
			locked = true;
			break;
		}
	}
	const LONG mode = InterlockedExchange(&ctx->MediaMode, MediaModeNone);
	if (locked)
	{
		if (wasActive)
		{
			OA2DP_INFO(ctx, "Media", "stopped after %I64u packets", ctx->PacketsSent);
		}
		WdfWaitLockRelease(ctx->MediaLock);
	}
	else
	{
		OA2DP_WARN(ctx, "Media",
			"stop: the media work item still holds the lock after %u ms; stopping without it",
			MediaStopLockLimitMs);
	}
	// Send completions may still enqueue the work item; such a run sees MediaActive
	// clear and returns without a lock. Flushing here, with no lock held and never from
	// the media work item itself, guarantees no run is left once the stream is stopped.
	WdfWorkItemFlush(ctx->MediaWorkItem);

	InterlockedExchange(&ctx->StreamFault, 0);
	if (mode == MediaModeService)
	{
		// The driver consumes the frame ring, so it can discard what is left.
		ServiceFlushRings(ctx);
		ServiceStreamStop(ctx);
	}
}
