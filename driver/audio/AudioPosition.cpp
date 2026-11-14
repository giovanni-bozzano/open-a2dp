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

// True when the RT buffer span [from, to) holds a sample that is not silence. Only the last
// buffer's worth is still there; silence is all-zero bytes at every depth the endpoint lists.
// Caller holds AudioLock.
static bool SpanHasAudio(const DEVICE_CONTEXT* ctx, ULONGLONG from, ULONGLONG to, ULONG bufferSize)
{
	if (to - from > bufferSize)
	{
		from = to - bufferSize;
	}
	for (ULONGLONG at = from; at < to;)
	{
		const ULONG offset = static_cast<ULONG>(at % bufferSize);
		ULONG chunk = bufferSize - offset;
		if (chunk > to - at)
		{
			chunk = static_cast<ULONG>(to - at);
		}
		const UCHAR* bytes = ctx->AudioBuffer + offset;
		for (ULONG i = 0; i < chunk; ++i)
		{
			if (bytes[i] != 0)
			{
				return true;
			}
		}
		at += chunk;
	}
	return false;
}

// The format a ring's audio was captured at, as one word, so a stream start can tell whether
// what the ring holds is still the format it is about to play.
static ULONG PackPcmFormat(const DEVICE_CONTEXT* ctx)
{
	return (ctx->PinSampleRate << 16) | ctx->PinBytesPerFrame;
}

// Copies the span the clock consumed since the last copy into the ring. Caller holds AudioLock
// and owns the ring view.
static void CopySpanLocked(DEVICE_CONTEXT* ctx, RingBufferView& ring, ULONGLONG position,
						   ULONG bufferSize)
{
	const ULONG format = PackPcmFormat(ctx);
	const ULONG pending = ring.ReadableBytes();
	if (pending != 0 && ctx->PcmRingFormat != format)
	{
		// Audio captured at another format cannot be played at this one: drop it before the two
		// are mixed into the same ring, which no consumer could ever read correctly. This is an
		// expected format boundary, not PCM the copy missed.
		(void)ring.Discard(pending);
	}
	// Never copy more than one buffer: anything older was already overwritten, so it is lost - the
	// clock was starved for longer than the engine buffers.
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
	// One format per ring: from here the ring holds only audio captured at this one.
	ctx->PcmRingFormat = format;
}

void CopyConsumedLocked(DEVICE_CONTEXT* ctx, ULONGLONG position)
{
	if (position < ctx->AudioCopiedPosition)
	{
		// The position is the copy reference and only moves forward: a clock that restarted
		// below it would make every difference here wrap and stop the copy for good.
		position = ctx->AudioCopiedPosition;
	}
	const ULONG bufferSize = ctx->AudioPacketSize * ctx->AudioRtPacketCount;
	if (bufferSize == 0 || ctx->AudioBuffer == nullptr)
	{
		return;
	}
	// The AVDTP stream starts on the first audio, not on the run edge: Windows also runs the
	// stream around an endpoint rebuild with nothing to play. Checked before the skip below,
	// because until START completes media is stopped. The first audio of a run is also the end
	// of the pre-roll window, so a trim there is startup cleanup rather than a playing stall.
	const bool firstAudio = ctx->AudioHeard == 0;
	if (firstAudio && ctx->AudioRunning &&
		SpanHasAudio(ctx, ctx->AudioCopiedPosition, position, bufferSize))
	{
		ctx->AudioHeard = 1;
		SignalingOnAudioHeard(ctx);
	}
	RingBufferView ring(ctx->PcmRingHeader, ctx->PcmRingData);
	const uint32_t frameBytes = ctx->PinBytesPerFrame;
	if (!ctx->MediaActive)
	{
		if (!ctx->AudioRunning || ctx->AudioHeard == 0)
		{
			// Nothing worth keeping: the endpoint has not played a sample of this run, or it is not
			// running at all. Move the reference, and count what the wait for the stream cost.
			if (ctx->AudioRunning && position > ctx->AudioCopiedPosition)
			{
				ctx->AudioSkippedBytes += position - ctx->AudioCopiedPosition;
			}
			ctx->AudioCopiedPosition = position;
			return;
		}
		// The endpoint is playing and the stream that carries this audio is starting: the span is
		// the beginning of what the listener asked for, so it is kept rather than thrown away.
		// Holding the ring to the lead keeps a long suspension from accumulating a backlog, and it
		// is done without counting a trim per tick: nothing is wrong here, the stream is simply not
		// running yet.
		if (frameBytes != 0 && ctx->PinSampleRate != 0)
		{
			CopySpanLocked(ctx, ring, position, bufferSize);
			const uint32_t keepMs =
				LatencyBudget::TrimTargetMs(ctx->PacingLeadMs, LatencyBudget::kPcmSlackMs);
			const uint32_t keep = PcmBacklog::LimitBytes(ctx->PinSampleRate, frameBytes, keepMs);
			(void)PcmBacklog::Trim(ring, keep, frameBytes);
		}
		else
		{
			ctx->AudioCopiedPosition = position;
		}
		return;
	}
	// The interval between two copies that carried audio is how long the endpoint was held off:
	// the lead has to cover one, or the pacer finds the ring empty between them.
	if (position > ctx->AudioCopiedPosition)
	{
		OA2dpNoteGapMs(ctx->LastRenderTime, ctx->WorstRenderGapMs, KeQueryInterruptTime());
	}
	const ULONG readableBefore = ring.ReadableBytes();
	CopySpanLocked(ctx, ring, position, bufferSize);

	// An SBC stream waits for a whole packet of PCM; waking it when the ring first holds one
	// means the wait ends with the audio instead of on the next timer tick.
	const LONG packetBytes = ctx->MediaPacketPcmBytes;
	if (packetBytes > 0 && readableBefore < static_cast<ULONG>(packetBytes) &&
		ring.ReadableBytes() >= static_cast<ULONG>(packetBytes))
	{
		WdfWorkItemEnqueue(ctx->MediaWorkItem);
	}

	// Bound the backlog: the ring is a FIFO with no other trim, so after a stall (transport,
	// service or DPC) it holds old audio that the encoder keeps draining at real time. Drop the
	// oldest frames instead: one skipped moment is better than a permanent offset.
	if (frameBytes != 0 && ctx->PinSampleRate != 0)
	{
		const uint32_t limitMs =
			LatencyBudget::RingLimitMs(ctx->PacingLeadMs, LatencyBudget::kPcmSlackMs);
		const uint32_t limit = PcmBacklog::LimitBytes(ctx->PinSampleRate, frameBytes, limitMs);
		// A backlog that shows up on one copy tick is a wobble, not a stall: the pacer may simply
		// be between packets. Only an excess that persists is dropped, and it is dropped back to
		// the lead rather than to the limit, so the one gap it costs buys an interval of headroom
		// instead of leaving the ring on the edge where the next burst trims again.
		const uint32_t targetMs =
			LatencyBudget::TrimTargetMs(ctx->PacingLeadMs, LatencyBudget::kPcmSlackMs);
		const uint32_t target = PcmBacklog::LimitBytes(ctx->PinSampleRate, frameBytes, targetMs);
		if (LatencyBudget::TrimNow(PcmBacklog::OverLimit(ring, limit, frameBytes),
								ctx->PcmTrimStrikes))
		{
			const uint32_t dropped = PcmBacklog::Trim(ring, target, frameBytes);
			if (!firstAudio)
			{
				ctx->AudioTrimmedBytes += dropped;
			}
		}

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

void ArmAudioTimerLocked(DEVICE_CONTEXT* ctx, ULONGLONG position)
{
	LONGLONG due = static_cast<LONGLONG>(AudioCopyIntervalMs) * 10000;
	const ULONGLONG bytesPerSecond = static_cast<ULONGLONG>(ctx->PinSampleRate) * ctx->PinBytesPerFrame;
	if (ctx->AudioPacketSize != 0 && bytesPerSecond != 0)
	{
		// In (0, packet]: a position on the boundary was completed by the caller, so the next
		// wake is the end of the packet that starts there. Rounded up, so it is never early.
		const ULONGLONG remaining = ctx->AudioPacketSize - position % ctx->AudioPacketSize;
		const LONGLONG boundary =
			static_cast<LONGLONG>((remaining * 10000000ull + bytesPerSecond - 1) / bytesPerSecond);
		if (boundary < due)
		{
			due = boundary;
		}
	}
	// Relative and never zero: a high-resolution timer requires a negative due time.
	WdfTimerStart(ctx->AudioTimer, -due);
}

// The QPC time the render clock crossed `boundary`, which is when that packet completed. A
// timer wakes after the boundary, so stamping the wake instead would report each completion
// late by a varying amount. Caller holds AudioLock while the stream runs.
static ULONGLONG BoundaryQpcLocked(const DEVICE_CONTEXT* ctx, ULONGLONG boundary, ULONGLONG now)
{
	ULONGLONG frequency;
	(void)QpcNow(&frequency);
	if (boundary < ctx->AudioBasePosition || ctx->PinBytesPerFrame == 0 || ctx->PinSampleRate == 0)
	{
		return now;
	}
	const ULONGLONG frames = (boundary - ctx->AudioBasePosition) / ctx->PinBytesPerFrame;
	const ULONGLONG qpc = ctx->AudioStartQpc + frames * frequency / ctx->PinSampleRate;
	return qpc < now ? qpc : now;
}

// The render clock: copies what the engine has played into the PCM ring and reports each packet
// that ended complete, which is what hands the engine its next buffer, then arms itself for the
// end of the next packet or the next copy, whichever is sooner. It re-arms unconditionally while
// the stream runs; pause and release clear AudioRunning under the lock and stop it with a wait.
void OnAudioTimer(_In_ WDFTIMER timer)
{
	DEVICE_CONTEXT* ctx = GetDeviceContext(WdfTimerGetParentObject(timer));
	ACXSTREAM stream = nullptr;
	ULONGLONG completed = 0;
	ULONGLONG completedQpc = 0;
	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	if (!ctx->AudioRunning)
	{
		KeReleaseSpinLock(&ctx->AudioLock, irql);
		return;
	}
	ULONGLONG qpc;
	const ULONGLONG position = CurrentPositionLocked(ctx, &qpc);
	CopyConsumedLocked(ctx, position);
	if (ctx->AudioPacketSize != 0)
	{
		const ULONGLONG due = position / ctx->AudioPacketSize;
		if (due > ctx->AudioCompletedPackets)
		{
			ctx->AudioCompletedPackets = due;
			completed = due;
			completedQpc = BoundaryQpcLocked(ctx, due * ctx->AudioPacketSize, qpc);
			stream = ctx->AudioStream;
		}
	}
	ArmAudioTimerLocked(ctx, position);
	KeReleaseSpinLock(&ctx->AudioLock, irql);

	if (stream != nullptr)
	{
		// Outside the lock: ACX sets the engine's notification event and takes its own locks.
		(void)AcxRtStreamNotifyPacketComplete(stream, completed - 1, completedQpc);
	}
}
} // namespace OpenA2dpDriver::Audio
