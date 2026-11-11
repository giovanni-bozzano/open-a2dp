#include "AudioInternal.h"

namespace OpenA2dpDriver::Audio
{
ULONGLONG QpcNow(ULONGLONG* frequency)
{
	LARGE_INTEGER freq;
	const LARGE_INTEGER now = KeQueryPerformanceCounter(&freq);
	if (frequency != nullptr)
	{
		*frequency = static_cast<ULONGLONG>(freq.QuadPart);
	}
	return static_cast<ULONGLONG>(now.QuadPart);
}

// Bytes the virtual device has consumed from the RT buffer. Caller holds AudioLock.
ULONGLONG CurrentPositionLocked(DEVICE_CONTEXT* ctx, ULONGLONG* qpc)
{
	ULONGLONG frequency;
	const ULONGLONG now = QpcNow(&frequency);
	if (qpc != nullptr)
	{
		*qpc = now;
	}
	if (!ctx->AudioRunning)
	{
		return ctx->AudioBasePosition;
	}
	const ULONGLONG frames = (now - ctx->AudioStartQpc) * ctx->PinSampleRate / frequency;
	return ctx->AudioBasePosition + frames * ctx->PinBytesPerFrame;
}

void CopyConsumedLocked(DEVICE_CONTEXT* ctx, ULONGLONG position)
{
	const ULONG bufferSize = ctx->AudioPacketSize * ctx->AudioRtPacketCount;
	if (bufferSize == 0 || ctx->AudioBuffer == nullptr)
	{
		return;
	}
	// With media stopped nothing consumes the ring, and the start that follows flushes it: copying
	// now only fills it until the bound trims the whole of it away, which is a trim per tick that
	// means nothing. Move the reference instead, the same way the flush drops this audio once.
	if (!ctx->MediaActive)
	{
		ctx->AudioCopiedPosition = position;
		return;
	}
	RingBufferView ring(ctx->PcmRingHeader, ctx->PcmRingData);

	// Never copy more than one buffer: anything older was already overwritten.
	ULONGLONG copied = ctx->AudioCopiedPosition;
	if (position - copied > bufferSize)
	{
		ctx->AudioDroppedBytes += position - copied - bufferSize;
		copied = position - bufferSize;
	}
	while (copied < position)
	{
		const ULONG offset = static_cast<ULONG>(copied % bufferSize);
		ULONG chunk = bufferSize - offset;
		if (chunk > position - copied)
		{
			chunk = static_cast<ULONG>(position - copied);
		}
		// Drop audio when the encoder fell behind and the ring is full.
		if (!ring.Write(ctx->AudioBuffer + offset, chunk))
		{
			ctx->AudioDroppedBytes += position - copied;
			copied = position;
			break;
		}
		copied += chunk;
	}
	ctx->AudioCopiedPosition = copied;

	// Bound the backlog: the ring is a FIFO with no other trim, so after a stall (transport,
	// service or DPC) it holds old audio that the encoder keeps draining at real time. Drop the
	// oldest frames instead: one skipped moment is better than a permanent offset.
	const ULONG frameBytes = ctx->PinBytesPerFrame;
	if (frameBytes != 0 && ctx->PinSampleRate != 0)
	{
		const uint32_t limitMs =
			LatencyBudget::RingLimitMs(ctx->PacingLeadMs, LatencyBudget::kPcmSlackMs);
		const uint32_t limit = PcmBacklog::LimitBytes(ctx->PinSampleRate, frameBytes, limitMs);
		ctx->AudioTrimmedBytes += PcmBacklog::Trim(ring, limit, frameBytes);

		// Peak driver-side latency for this stream. An instantaneous reading hides the spike a
		// stall caused, so the worst the lead and both rings have been is kept until the next
		// stream starts. Sampled wherever the PCM ring moves.
		const uint32_t pcmMs = static_cast<uint32_t>(static_cast<uint64_t>(ring.ReadableBytes()) * 1000 /
												 (static_cast<uint64_t>(ctx->PinSampleRate) * frameBytes));
		uint32_t frameMs = 0;
		const uint32_t bitrate = ctx->ActiveBitrate;
		if (bitrate != 0 && ctx->FrameRingHeader != nullptr)
		{
			frameMs = static_cast<uint32_t>(
				static_cast<uint64_t>(RingBufferView(ctx->FrameRingHeader, ctx->FrameRingData).ReadableBytes()) *
				8000 / bitrate);
		}
		const uint32_t latency = ctx->PacingLeadMs + pcmMs + frameMs;
		if (latency > static_cast<uint32_t>(ctx->LatencyPeakMs))
		{
			ctx->LatencyPeakMs = static_cast<LONG>(latency);
		}
	}
}

// Reports completed packets to ACX. The copy itself also runs when the engine releases a packet
// (see OnSetRenderPacket): an engine buffer shorter than AudioPeriodMs would otherwise fall more
// than a buffer behind between ticks, and the oldest audio would be dropped every tick.
void OnAudioTimer(_In_ WDFTIMER timer)
{
	DEVICE_CONTEXT* ctx = GetDeviceContext(WdfTimerGetParentObject(timer));

	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	if (!ctx->AudioRunning || ctx->AudioBuffer == nullptr)
	{
		KeReleaseSpinLock(&ctx->AudioLock, irql);
		return;
	}

	ULONGLONG qpc;
	const ULONGLONG position = CurrentPositionLocked(ctx, &qpc);
	CopyConsumedLocked(ctx, position);

	const ULONGLONG completed = position / ctx->AudioPacketSize;
	const BOOLEAN notify = completed > ctx->AudioCompletedPackets;
	ctx->AudioCompletedPackets = completed;
	ACXSTREAM stream = ctx->AudioStream;
	KeReleaseSpinLock(&ctx->AudioLock, irql);

	if (notify && stream != nullptr)
	{
		(void)AcxRtStreamNotifyPacketComplete(stream, completed - 1, qpc);
	}
}
} // namespace OpenA2dpDriver::Audio
