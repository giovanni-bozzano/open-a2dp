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
	if (codec == Avdtp::CodecType::Aac)
	{
		StartAac(ctx, ie, ieLength);
		return;
	}
	if (codec == Avdtp::CodecType::Vendor)
	{
		// Every vendor codec shares this type; the IE says which one was negotiated.
		uint8_t capabilities = 0;
		if (AptxHd::ParseIe(ie, ieLength, capabilities))
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
		return;
	}
	// The encoder reads the PCM the ACX stream produced, so the negotiated format must be
	// the one Windows opened the endpoint with.
	if (params.sampleRate != ctx->PinSampleRate || params.Channels() != ctx->PinChannels)
	{
		OA2DP_WARN(ctx, "Media", "SBC %u Hz %u ch does not match the endpoint format %u Hz %u ch",
			params.sampleRate, params.Channels(), ctx->PinSampleRate, ctx->PinChannels);
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
	ctx->MediaCaughtUp = FALSE;
	// Per stream, like the counters next to it: a status line that mixes a stream's packet
	// count with a device's lifetime underrun total cannot say whether a gap just happened.
	InterlockedExchange64(&ctx->Underruns, 0);
	ctx->MediaStartTime = KeQueryInterruptTime();
	// The PCM the endpoint queued before MediaStart predates this stream, exactly as the service
	// encoders treat their own start: drop it, so the pacer fills the lead with silence rather
	// than sending audio captured while the sink was not listening.
	ULONG flushed = 0;
	{
		RingBufferView ring(ctx->PcmRingHeader, ctx->PcmRingData);
		flushed = ring.ReadableBytes();
		if (flushed != 0)
		{
			ring.Skip(flushed);
		}
	}
	// The endpoint starts running the moment Windows begins playback, but this stream only starts
	// once AVDTP accepts START, and until then nothing drains the ring: the audio Windows queues in
	// between is trimmed by the ring bound, then flushed above. That span is the start latency the
	// listener hears, and nothing else reports it, so it is logged with the audio it cost.
	const ULONGLONG started = KeQueryInterruptTime();
	const ULONGLONG waited = ctx->AudioRunStamp != 0 && started > ctx->AudioRunStamp
		? (started - ctx->AudioRunStamp) / 10000ull
		: 0;
	const ULONGLONG lostBytes = static_cast<ULONGLONG>(flushed) + ctx->AudioTrimmedBytes;
	const ULONGLONG lostMs = ctx->PinSampleRate != 0 && ctx->PinBytesPerFrame != 0
		? lostBytes * 1000ull / (static_cast<ULONGLONG>(ctx->PinSampleRate) * ctx->PinBytesPerFrame)
		: 0;
	OA2DP_INFO(ctx, "Media", "stream started %I64u ms after the endpoint ran, %I64u ms of queued PCM lost",
		waited, lostMs);
	InterlockedExchange(&ctx->StreamFault, 0);
	InterlockedExchange(&ctx->MediaMode, MediaModeSbc);
	ctx->MediaActive = TRUE;
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
	// work item stops taking MediaLock. In the failing hang the lock was held by a work
	// item that kept running every 10 ms for two minutes while this thread waited.
	const bool wasActive = InterlockedExchange8(reinterpret_cast<volatile CHAR*>(&ctx->MediaActive), 0) != 0;
	WdfTimerStop(ctx->MediaTimer, TRUE);
	// MediaLock is only ever held for one bounded work item run, so this wait ends. It
	// is retried rather than abandoned: stopping without the lock would race a send.
	for (;;)
	{
		LONGLONG lockTimeout = WDF_REL_TIMEOUT_IN_MS(1000);
		if (WdfWaitLockAcquire(ctx->MediaLock, &lockTimeout) == STATUS_SUCCESS)
		{
			break;
		}
		OA2DP_WARN(ctx, "Media", "stop: still waiting for the media work item to release the lock");
	}
	const LONG mode = InterlockedExchange(&ctx->MediaMode, MediaModeNone);
	if (wasActive)
	{
		OA2DP_INFO(ctx, "Media", "stopped after %I64u packets", ctx->PacketsSent);
	}
	WdfWaitLockRelease(ctx->MediaLock);
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
