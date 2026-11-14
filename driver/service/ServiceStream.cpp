#include "ServiceInternal.h"

using namespace OpenA2dpDriver::Service;

void ServiceStreamStart(_In_ DEVICE_CONTEXT* ctx)
{
	KIRQL irql;
	KeAcquireSpinLock(&ctx->WaitLock, &irql);
	ctx->WaitState = {};
	ctx->WaitState.state = Ioctl::MediaState::Streaming;
	ctx->WaitState.codec = ctx->ActiveCodec;
	ctx->WaitState.sampleRate = ctx->ActiveSampleRate;
	ctx->WaitState.channels = ctx->ActiveChannels;
	ctx->WaitState.bitrate = ctx->ActiveBitrate;
	ctx->WaitState.objectType = ctx->ActiveObjectType;
	ctx->WaitState.pcmValidBits = ctx->PinValidBits;
	// The service centres its records on the same lead the kernel paces SBC with.
	ctx->WaitState.prerollMs = ctx->PacingLeadMs;
	// LDAC's quality mode is local, so the service gets it from the preference rather than
	// from a negotiation.
	ctx->WaitState.ldacQuality = ctx->ActiveLdacQuality;
	KeReleaseSpinLock(&ctx->WaitLock, irql);

	LatchEvents(ctx, Ioctl::kEventStreamStart);
}

void ServiceStreamStop(_In_ DEVICE_CONTEXT* ctx)
{
	KIRQL irql;
	KeAcquireSpinLock(&ctx->WaitLock, &irql);
	ctx->WaitState = {};
	ctx->WaitState.state = Ioctl::MediaState::Idle;
	KeReleaseSpinLock(&ctx->WaitLock, irql);

	LatchEvents(ctx, Ioctl::kEventStreamStop);
}

// A tick is a lifecycle event like any other for the parked waiter: it gets the latched
// events plus kEventTick, which is exactly what ReleaseWait does.
void ServiceTick(_In_ DEVICE_CONTEXT* ctx)
{
	ReleaseWait(ctx, Ioctl::kEventTick);
}

ULONG ServiceReadFrame(_In_ DEVICE_CONTEXT* ctx, _Out_ const UCHAR** payload, _Out_ uint32_t* samples,
	_Out_ uint32_t* silenceSamples, _Out_ uint32_t* skippedSamples, _Out_ uint32_t* flags)
{
	*payload = nullptr;
	*samples = 0;
	*silenceSamples = 0;
	*skippedSamples = 0;
	*flags = 0;
	if (ctx->FrameRingHeader == nullptr)
	{
		return 0;
	}

	RingBufferView frames(ctx->FrameRingHeader, ctx->FrameRingData);
	FrameRecord::Record record;
	const FrameRecord::ReadResult result = FrameRecord::Read(frames, ctx->FrameRecord, sizeof(ctx->FrameRecord), record);
	if (result == FrameRecord::ReadResult::Corrupt)
	{
		// Not repairable from the consumer side: stop consuming and let media stop.
		OA2DP_ERROR(ctx, "Service", "corrupt frame record");
		InterlockedExchange(&ctx->LastMediaError, STATUS_DATA_ERROR);
		InterlockedExchange(&ctx->StreamFault, 1);
		return 0;
	}
	if (result == FrameRecord::ReadResult::Dropped)
	{
		InterlockedIncrement64(&ctx->FramesDropped);
		return 0;
	}
	if (result != FrameRecord::ReadResult::Record)
	{
		return 0;
	}

	*payload = record.payload;
	*samples = record.samples;
	// A record can claim more silence than PCM it covers only if the producer is broken; the
	// counter is clamped so one bad record cannot move it by more than the audio it stood for.
	*silenceSamples = record.silenceSamples > record.samples ? record.samples : record.silenceSamples;
	*skippedSamples = record.skippedSamples;
	*flags = record.flags;
	return record.payloadLength;
}

void ServiceFlushRings(_In_ DEVICE_CONTEXT* ctx)
{
	// Only the frame ring: the driver is its consumer. Which PCM is still playback is decided
	// once, where the stream starts (AudioDropStalePcm).
	if (ctx->FrameRingHeader != nullptr)
	{
		RingBufferView frames(ctx->FrameRingHeader, ctx->FrameRingData);
		frames.Skip(frames.ReadableBytes());
	}
}
