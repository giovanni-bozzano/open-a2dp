#include "AudioInternal.h"

using namespace OpenA2dpDriver::Audio;

ULONG AudioReadPcm(_In_ DEVICE_CONTEXT* ctx, _Out_writes_(frames * AudioChannels) INT16* pcm, _In_ ULONG frames)
{
	RingBufferView ring(ctx->PcmRingHeader, ctx->PcmRingData);
	const ULONG bytesPerFrame = ctx->PinBytesPerFrame;
	if (bytesPerFrame == 0)
	{
		return 0;
	}
	const ULONG wanted = frames * bytesPerFrame;
	ULONG available = ring.ReadableBytes();
	available -= available % bytesPerFrame;
	const ULONG bytes = available < wanted ? available : wanted;
	if (bytes != 0)
	{
		(void)ring.Read(pcm, bytes);
	}
	if (bytes < wanted)
	{
		RtlZeroMemory(reinterpret_cast<PUCHAR>(pcm) + bytes, wanted - bytes);
	}
	return bytes / bytesPerFrame;
}
