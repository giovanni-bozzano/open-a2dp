#include "AudioInternal.h"

using namespace OpenA2dpDriver::Audio;

ULONG AudioReadPcm(_In_ DEVICE_CONTEXT* ctx, _Out_writes_(frames * AudioChannels) INT16* pcm, _In_ ULONG frames)
{
	RingBufferView ring(ctx->PcmRingHeader, ctx->PcmRingData);
	const ULONG wanted = frames * AudioBytesPerFrame;
	ULONG available = ring.ReadableBytes();
	available -= available % AudioBytesPerFrame;
	const ULONG bytes = available < wanted ? available : wanted;
	if (bytes != 0)
	{
		(void)ring.Read(pcm, bytes);
	}
	if (bytes < wanted)
	{
		RtlZeroMemory(reinterpret_cast<PUCHAR>(pcm) + bytes, wanted - bytes);
	}
	return bytes / AudioBytesPerFrame;
}
