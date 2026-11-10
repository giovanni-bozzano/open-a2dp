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

// Moves newly consumed bytes from the RT buffer into the PCM ring and reports
// completed packets to ACX.
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
	const ULONG bufferSize = ctx->AudioPacketSize * ctx->AudioRtPacketCount;
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

	// Bound the backlog. The ring is a FIFO with no other trim, so after a stall (transport,
	// service or DPC) it holds old audio that the encoder keeps draining at real time and the
	// stream stays that far behind live for the rest of its life. Drop the oldest frames
	// instead: one skipped moment is better than a permanent offset.
	const ULONG frameBytes = ctx->PinBytesPerFrame;
	if (frameBytes != 0 && ctx->PinSampleRate != 0)
	{
		const ULONGLONG limit =
			static_cast<ULONGLONG>(ctx->PinSampleRate) * frameBytes * AudioPcmBacklogMs / 1000;
		const uint32_t readable = ring.ReadableBytes();
		if (readable > limit + frameBytes)
		{
			uint32_t trim = readable - static_cast<uint32_t>(limit);
			trim += frameBytes - 1;
			trim -= trim % frameBytes;
			ring.Skip(trim);
			ctx->AudioTrimmedBytes += trim;
		}
	}

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
