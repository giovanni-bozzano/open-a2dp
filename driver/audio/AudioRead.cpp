#include "AudioInternal.h"

#include <OpenA2dp/PcmDepth.h>

using namespace OpenA2dpDriver::Audio;

// Reads `frames` interleaved frames into a 16-bit buffer. The endpoint can be opened at
// 24-in-32 (when aptX HD is available), in which case the kernel encoder still takes 16-bit
// samples: the top 16 bits of each left-justified sample are the value, so the wide form is
// truncated down rather than refused.
// Caller holds AudioLock, so the format is read as one set and the copy cannot trim the ring
// while this reads it.
static ULONG ReadPcmLocked(_In_ DEVICE_CONTEXT* ctx, _Out_writes_(frames * AudioChannels) INT16* pcm, _In_ ULONG frames)
{
	RingBufferView ring(ctx->PcmRingHeader, ctx->PcmRingData);
	const ULONG channels = ctx->PinChannels;
	const ULONG frameBytes = ctx->PinBytesPerFrame;
	const USHORT validBits = ctx->PinValidBits;
	if (frameBytes == 0 || channels == 0)
	{
		// Nothing can be read, but the caller encodes the whole buffer: leave silence rather than
		// the previous frame, or a format teardown replays audio it reports as a gap.
		RtlZeroMemory(pcm, frames * AudioChannels * sizeof(INT16));
		return 0;
	}

	if (validBits == 16)
	{
		const ULONG wanted = frames * frameBytes;
		ULONG available = ring.ReadableBytes();
		available -= available % frameBytes;
		const ULONG bytes = available < wanted ? available : wanted;
		if (bytes != 0)
		{
			(void)ring.Read(pcm, bytes);
		}
		if (bytes < wanted)
		{
			RtlZeroMemory(reinterpret_cast<PUCHAR>(pcm) + bytes, wanted - bytes);
		}
		return bytes / frameBytes;
	}

	const ULONG sourceFrameBytes = channels * sizeof(INT32);
	const ULONG destinationFrameBytes = channels * sizeof(INT16);
	ULONG available = ring.ReadableBytes();
	available -= available % sourceFrameBytes;
	const ULONG wanted = frames * sourceFrameBytes;
	ULONG consumed = 0;
	ULONG written = 0;
	UCHAR scratch[256];
	while (consumed < (available < wanted ? available : wanted))
	{
		ULONG chunk = (available < wanted ? available : wanted) - consumed;
		if (chunk > sizeof(scratch))
		{
			chunk = sizeof(scratch);
		}
		chunk -= chunk % sourceFrameBytes;
		if (chunk == 0 || !ring.Read(scratch, chunk))
		{
			break;
		}
		const ULONG chunkFrames = chunk / sourceFrameBytes;
		for (ULONG frame = 0; frame < chunkFrames; ++frame)
		{
			const UCHAR* source = scratch + frame * sourceFrameBytes;
			INT16* destination = pcm + (written + frame) * channels;
			for (ULONG channel = 0; channel < channels; ++channel)
			{
				INT32 sample = 0;
				RtlCopyMemory(&sample, source + channel * sizeof(INT32), sizeof(sample));
				destination[channel] = Pcm::Narrow24To16(sample);
			}
		}
		consumed += chunk;
		written += chunkFrames;
	}
	if (written < frames)
	{
		RtlZeroMemory(reinterpret_cast<PUCHAR>(pcm) + written * destinationFrameBytes,
			(frames - written) * destinationFrameBytes);
	}
	return written;
}

ULONG AudioReadPcm(_In_ DEVICE_CONTEXT* ctx, _Out_writes_(frames * AudioChannels) INT16* pcm, _In_ ULONG frames)
{
	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	const ULONG read = ReadPcmLocked(ctx, pcm, frames);
	KeReleaseSpinLock(&ctx->AudioLock, irql);
	return read;
}
