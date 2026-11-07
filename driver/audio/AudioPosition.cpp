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

	const ULONGLONG completed = position / ctx->AudioPacketSize;
	const BOOLEAN notify = completed > ctx->AudioCompletedPackets;
	ctx->AudioCompletedPackets = completed;
	ACXSTREAM stream = ctx->AudioStream;
	static ULONG ticks;
	const BOOLEAN report = (++ticks % 1000) == 0;
	const ULONG readable = ring.ReadableBytes();
	const ULONGLONG dropped = ctx->AudioDroppedBytes;
	KeReleaseSpinLock(&ctx->AudioLock, irql);

	if (report)
	{
		OA2DP_INFO(ctx, "Audio", "position %llu, %u PCM bytes buffered, %llu bytes dropped so far", position, readable, dropped);
	}

	if (notify && stream != nullptr)
	{
		(void)AcxRtStreamNotifyPacketComplete(stream, completed - 1, qpc);
	}
}
} // namespace OpenA2dpDriver::Audio
