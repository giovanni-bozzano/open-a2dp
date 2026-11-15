#include "AudioInternal.h"

namespace OpenA2dpDriver::Audio
{
// The PCM this run lost and a new baseline for the next pause. A lifetime counter cannot tell
// an old boundary discard from loss in the stream that just played. Caller holds AudioLock.
static ULONGLONG TakeRunDroppedLocked(DEVICE_CONTEXT* ctx)
{
	const ULONGLONG total = ctx->AudioDroppedBytes;
	const ULONGLONG dropped =
		total > ctx->AudioRunDroppedBytes ? total - ctx->AudioRunDroppedBytes : 0;
	ctx->AudioRunDroppedBytes = total;
	return dropped;
}

NTSTATUS OnCreateStream(_In_ WDFDEVICE device, _In_ ACXCIRCUIT circuit, _In_ ACXPIN, _In_ PACXSTREAM_INIT streamInit,
	_In_ ACXDATAFORMAT format, _In_ const GUID*, _In_ ACXOBJECTBAG)
{
	DEVICE_CONTEXT* ctx = GetDeviceContext(device);
	NTSTATUS status = AudioValidatePinFormat(ctx, format);
	if (!NT_SUCCESS(status))
	{
		return status;
	}
	ACX_STREAM_CALLBACKS streamCallbacks;
	ACX_STREAM_CALLBACKS_INIT(&streamCallbacks);
	streamCallbacks.EvtAcxStreamPrepareHardware = OnStreamPrepareHardware;
	streamCallbacks.EvtAcxStreamReleaseHardware = OnStreamReleaseHardware;
	streamCallbacks.EvtAcxStreamRun = OnStreamRun;
	streamCallbacks.EvtAcxStreamPause = OnStreamPause;
	status = AcxStreamInitAssignAcxStreamCallbacks(streamInit, &streamCallbacks);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	ACX_RT_STREAM_CALLBACKS rtCallbacks;
	ACX_RT_STREAM_CALLBACKS_INIT(&rtCallbacks);
	rtCallbacks.EvtAcxStreamGetHwLatency = OnGetHwLatency;
	rtCallbacks.EvtAcxStreamAllocateRtPackets = OnAllocateRtPackets;
	rtCallbacks.EvtAcxStreamFreeRtPackets = OnFreeRtPackets;
	rtCallbacks.EvtAcxStreamSetRenderPacket = OnSetRenderPacket;
	rtCallbacks.EvtAcxStreamGetCurrentPacket = OnGetCurrentPacket;
	rtCallbacks.EvtAcxStreamGetPresentationPosition = OnGetPresentationPosition;
	status = AcxStreamInitAssignAcxRtStreamCallbacks(streamInit, &rtCallbacks);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	AcxStreamInitSetAcxRtStreamSupportsNotifications(streamInit);

	WDF_OBJECT_ATTRIBUTES attrs;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, STREAM_CONTEXT);
	attrs.EvtDestroyCallback = OnStreamDestroy;
	ACXSTREAM stream;
	status = AcxRtStreamCreate(device, circuit, &attrs, &streamInit, &stream);
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(GetDeviceContext(device), "Audio", "AcxRtStreamCreate failed 0x%08X", status);
		return status;
	}

	STREAM_CONTEXT* streamContext = GetStreamContext(stream);
	streamContext->Device = ctx;
	const auto* wave = static_cast<const KSDATAFORMAT_WAVEFORMATEX*>(AcxDataFormatGetKsDataFormat(format));
	streamContext->SampleRate = wave->WaveFormatEx.nSamplesPerSec;
	streamContext->Channels = wave->WaveFormatEx.nChannels;
	streamContext->ValidBits = PcmValidBits(*wave);

	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	ctx->AudioStream = stream;
	KeReleaseSpinLock(&ctx->AudioLock, irql);
	return STATUS_SUCCESS;
}

void OnStreamDestroy(_In_ WDFOBJECT object)
{
	DEVICE_CONTEXT* ctx = GetStreamContext(static_cast<ACXSTREAM>(object))->Device;
	if (ctx == nullptr)
	{
		return;
	}
	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	if (ctx->AudioStream == static_cast<ACXSTREAM>(object))
	{
		ctx->AudioStream = nullptr;
	}
	KeReleaseSpinLock(&ctx->AudioLock, irql);
}

NTSTATUS OnStreamPrepareHardware(_In_ ACXSTREAM stream)
{
	// Release forgets the stream, and ACX can prepare the same one again without destroying it,
	// so it is recorded here too: the pin default must not move under a stream that lives.
	DEVICE_CONTEXT* ctx = GetStreamContext(stream)->Device;
	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	ctx->AudioStream = stream;
	KeReleaseSpinLock(&ctx->AudioLock, irql);
	// Prepared, so Windows may run it at any moment: the earliest point to ask the link to leave
	// sniff. The signaling work item sends the request at PASSIVE_LEVEL.
	SignalingOnAudioImminent(ctx);
	return STATUS_SUCCESS;
}

NTSTATUS OnStreamReleaseHardware(_In_ ACXSTREAM stream)
{
	DEVICE_CONTEXT* ctx = GetStreamContext(stream)->Device;
	KIRQL irql;
	ULONGLONG dropped = 0;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	const BOOLEAN wasRunning = ctx->AudioRunning;
	if (wasRunning != FALSE)
	{
		dropped = TakeRunDroppedLocked(ctx);
	}
	ctx->AudioRunning = FALSE;
	ctx->AudioBasePosition = 0;
	ctx->AudioCopiedPosition = 0;
	ctx->AudioCompletedPackets = 0;
	ctx->AudioStream = nullptr;
	KeReleaseSpinLock(&ctx->AudioLock, irql);
	// A wake that already read the stream reports completion to ACX after it drops the lock, so it
	// has to finish before ACX frees the stream; one that starts later sees AudioRunning clear
	// and neither notifies nor re-arms.
	WdfTimerStop(ctx->AudioTimer, TRUE);
	if (wasRunning != FALSE)
	{
		if (dropped != 0)
		{
			OA2DP_INFO(ctx, "Audio",
					"stream released, %llu bytes of endpoint PCM not copied during this run", dropped);
		}
		// Released without a pause - a playback that stopped, or the format change that rebuilds
		// the endpoint - leaves a stream nothing plays for. A stream Windows only enumerated and
		// released never ran, so it must not suspend the one that does.
		SignalingOnAudioPause(ctx);
	}
	return STATUS_SUCCESS;
}

NTSTATUS OnStreamRun(_In_ ACXSTREAM stream)
{
	// This stream carries audio, so its format is the one the encoder and the position math
	// follow, and it is committed before the clock starts: the copy reads it on its first wake, and
	// a wake that landed between the two would size this stream's audio with the previous one's
	// frame. The offered format list is not touched: it belongs to the endpoint setting
	// (AudioApplyEndpointFormats), not to whatever Windows opened.
	DEVICE_CONTEXT* ctx = GetStreamContext(stream)->Device;
	const STREAM_CONTEXT* streamContext = GetStreamContext(stream);
	AudioSetPinFormat(ctx, streamContext->SampleRate, streamContext->Channels, streamContext->ValidBits);

	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	if (ctx->AudioHeard != 0)
	{
		// The previous run played, so what the ring still holds is audio of a session that already
		// ended - a pause it was suspended with, or the stream before this run. Keeping it would
		// replay audio from before this run's first sample; the audio captured from here on is the
		// beginning of playback and is kept (CopyConsumedLocked). This is boundary cleanup, not a
		// copy loss, so it does not move AudioDroppedBytes.
		RingBufferView ring(ctx->PcmRingHeader, ctx->PcmRingData);
		(void)ring.Discard(ring.ReadableBytes());
	}
	if (ctx->AudioRunning)
	{
		// Run without a pause: continue from where the clock already reached instead of
		// restarting it at the old base, which would move the position backwards and make the
		// copy state wrap.
		ctx->AudioBasePosition = CurrentPositionLocked(ctx, nullptr);
	}
	ctx->AudioStartQpc = QpcNow(nullptr);
	ctx->AudioRunStamp = KeQueryInterruptTime();
	ctx->AudioRunning = TRUE;
	ctx->AudioHeard = 0;
	ctx->AudioSkippedBytes = 0;
	ctx->AudioRunDroppedBytes = ctx->AudioDroppedBytes;
	ArmAudioTimerLocked(ctx, ctx->AudioBasePosition);
	KeReleaseSpinLock(&ctx->AudioLock, irql);

	SignalingOnAudioRun(ctx);
	return STATUS_SUCCESS;
}

NTSTATUS OnStreamPause(_In_ ACXSTREAM stream)
{
	DEVICE_CONTEXT* ctx = GetStreamContext(stream)->Device;
	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	ctx->AudioBasePosition = CurrentPositionLocked(ctx, nullptr);
	ctx->AudioRunning = FALSE;
	const ULONGLONG dropped = TakeRunDroppedLocked(ctx);
	KeReleaseSpinLock(&ctx->AudioLock, irql);
	WdfTimerStop(ctx->AudioTimer, TRUE);

	// The lifecycle line stays; only this run's loss is added, and only when there is some. A
	// pause that discarded the pre-roll at a session boundary is normal and must not look like a
	// new fault in the log.
	if (dropped != 0)
	{
		OA2DP_INFO(ctx, "Audio", "stream paused, %llu bytes of endpoint PCM not copied during this "
				"run", dropped);
	}
	else
	{
		OA2DP_INFO(ctx, "Audio", "stream paused");
	}
	// Nothing is playing, so the stream the sink keeps buffering is silence the listener does not
	// need: suspend it and start again on the next run.
	SignalingOnAudioPause(ctx);
	return STATUS_SUCCESS;
}
} // namespace OpenA2dpDriver::Audio

// Declared in Driver.h beside AudioStreamRunning, because a stream start on the media side is
// what decides whether the ring is kept. A stream that is about to start can keep the audio
// already in the ring - it is the beginning of playback, captured after the endpoint's first
// sample, not audio from before the session - as long as it was captured at the format this
// stream plays. Anything else is stale and dropped.
ULONG AudioDropStalePcm(_In_ DEVICE_CONTEXT* ctx, ULONG sampleRate, USHORT bytesPerFrame)
{
	const ULONG playbackFormat = (sampleRate << 16) | bytesPerFrame;
	ULONG dropped = 0;
	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	const bool keep = ctx->AudioHeard != 0 && sampleRate != 0 && bytesPerFrame != 0 &&
		ctx->PcmRingFormat == playbackFormat;
	if (!keep)
	{
		RingBufferView ring(ctx->PcmRingHeader, ctx->PcmRingData);
		dropped = ring.Discard(ring.ReadableBytes());
	}
	KeReleaseSpinLock(&ctx->AudioLock, irql);
	return dropped;
}

bool AudioStreamRunning(_In_ DEVICE_CONTEXT* ctx)
{
	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	const bool running = ctx->AudioRunning != FALSE;
	KeReleaseSpinLock(&ctx->AudioLock, irql);
	return running;
}

bool AudioStreamPlaying(_In_ DEVICE_CONTEXT* ctx)
{
	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	const bool playing = ctx->AudioRunning != FALSE && ctx->AudioHeard != 0;
	KeReleaseSpinLock(&ctx->AudioLock, irql);
	return playing;
}
