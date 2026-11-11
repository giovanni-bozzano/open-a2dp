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
	_Out_ uint32_t* flags)
{
	*payload = nullptr;
	*samples = 0;
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
		InterlockedIncrement(&ctx->FramesDropped);
		return 0;
	}
	if (result != FrameRecord::ReadResult::Record)
	{
		return 0;
	}

	*payload = record.payload;
	*samples = record.samples;
	*flags = record.flags;
	return record.payloadLength;
}

void ServiceFlushRings(_In_ DEVICE_CONTEXT* ctx)
{
	// Only the frame ring: the driver is its consumer. The PCM ring's consumer is the
	// service, which discards buffered audio itself when it sees kEventStreamStart.
	if (ctx->FrameRingHeader != nullptr)
	{
		RingBufferView frames(ctx->FrameRingHeader, ctx->FrameRingData);
		frames.Skip(frames.ReadableBytes());
	}
}
