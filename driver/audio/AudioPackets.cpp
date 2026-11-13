#include "AudioInternal.h"

namespace OpenA2dpDriver::Audio
{
NTSTATUS OnAllocateRtPackets(_In_ ACXSTREAM stream, _In_ ULONG packetCount, _In_ ULONG packetSize, _Out_ PACX_RTPACKET* packets)
{
	*packets = nullptr;
	DEVICE_CONTEXT* ctx = GetStreamContext(stream)->Device;
	const ULONGLONG total = static_cast<ULONGLONG>(packetSize) * packetCount;
	if (packetCount == 0 || packetCount > AudioMaxPacketCount || packetSize == 0 ||
		ctx->PinBytesPerFrame == 0 || packetSize % ctx->PinBytesPerFrame != 0 ||
		total > AudioMaxBufferBytes)
	{
		OA2DP_WARN(ctx, "Audio", "allocate %u packets of %u bytes rejected", packetCount, packetSize);
		return STATUS_NOT_SUPPORTED;
	}

	// ACX frees the packets before it asks for new ones. A second allocation would overwrite the
	// live MDL and its pages, leaking them and handing the next free the wrong buffer, so it is
	// refused rather than trusted to a pin setting.
	{
		KIRQL irql;
		KeAcquireSpinLock(&ctx->AudioLock, &irql);
		const bool live = ctx->AudioMdl != nullptr || ctx->AudioBuffer != nullptr;
		KeReleaseSpinLock(&ctx->AudioLock, irql);
		if (live)
		{
			OA2DP_ERROR(ctx, "Audio", "RT packets requested while a set is still live; refusing");
			return STATUS_DEVICE_BUSY;
		}
	}

	// Whole pages, zeroed, so no stale kernel memory is mapped to user mode.
	const SIZE_T bufferSize = ROUND_TO_PAGES(static_cast<SIZE_T>(total));

	auto* rtPackets = static_cast<PACX_RTPACKET>(
		ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(ACX_RTPACKET) * packetCount, OpenA2dpPoolTag));
	if (rtPackets == nullptr)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	PHYSICAL_ADDRESS low = {};
	PHYSICAL_ADDRESS high;
	high.QuadPart = MAXLONGLONG;
	PHYSICAL_ADDRESS skip = {};
	PMDL mdl = MmAllocatePagesForMdlEx(low, high, skip, bufferSize, MmCached, MM_ALLOCATE_FULLY_REQUIRED);
	if (mdl == nullptr)
	{
		OA2DP_ERROR(ctx, "Audio", "render buffer MDL allocation failed");
		ExFreePoolWithTag(rtPackets, OpenA2dpPoolTag);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	auto* buffer = static_cast<PUCHAR>(MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority | MdlMappingNoExecute));
	if (buffer == nullptr)
	{
		MmFreePagesFromMdl(mdl);
		ExFreePool(mdl);
		ExFreePoolWithTag(rtPackets, OpenA2dpPoolTag);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	RtlZeroMemory(buffer, bufferSize);

	for (ULONG i = 0; i < packetCount; ++i)
	{
		ACX_RTPACKET_INIT(&rtPackets[i]);
		WDF_MEMORY_DESCRIPTOR_INIT_MDL(&rtPackets[i].RtPacketBuffer, mdl, static_cast<ULONG>(bufferSize));
		rtPackets[i].RtPacketOffset = packetSize * i;
		rtPackets[i].RtPacketSize = packetSize;
	}

	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	ctx->AudioMdl = mdl;
	ctx->AudioBuffer = buffer;
	ctx->AudioPacketSize = packetSize;
	ctx->AudioRtPacketCount = packetCount;
	KeReleaseSpinLock(&ctx->AudioLock, irql);

	*packets = rtPackets;
	return STATUS_SUCCESS;
}

void OnFreeRtPackets(_In_ ACXSTREAM stream, _In_ PACX_RTPACKET packets, _In_ ULONG)
{
	DEVICE_CONTEXT* ctx = GetStreamContext(stream)->Device;
	WdfTimerStop(ctx->AudioTimer, TRUE);

	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	PMDL mdl = ctx->AudioMdl;
	PUCHAR buffer = ctx->AudioBuffer;
	ctx->AudioMdl = nullptr;
	ctx->AudioBuffer = nullptr;
	ctx->AudioPacketSize = 0;
	ctx->AudioRtPacketCount = 0;
	KeReleaseSpinLock(&ctx->AudioLock, irql);

	if (mdl != nullptr)
	{
		if (buffer != nullptr)
		{
			MmUnmapLockedPages(buffer, mdl);
		}
		MmFreePagesFromMdl(mdl);
		ExFreePool(mdl);
	}
	ExFreePoolWithTag(packets, OpenA2dpPoolTag);
}

// ACX defines the delay as the time between NotifyPacketComplete and the moment that packet's
// last sample is rendered. The stream is sent PacingLeadMs ahead of the media clock, so a packet
// reported complete is heard that much later; a stream that claimed 0 made Windows' own latency
// estimate disagree with the position the driver reports. There is no DMA FIFO behind the copy,
// so no bytes are claimed for one - the buffered audio is the delay.
NTSTATUS OnGetHwLatency(_In_ ACXSTREAM stream, _Out_ ULONG* fifoSize, _Out_ ULONG* delay)
{
	DEVICE_CONTEXT* ctx = GetStreamContext(stream)->Device;
	*fifoSize = 0;
	*delay = ctx->PacingLeadMs * 10000; // 100 ns units
	return STATUS_SUCCESS;
}

// The engine released a packet, so its data is queued and part of it is due now. Copying here
// keeps the PCM ring current without waiting for the packet boundary the timer is armed for, and
// it is what makes a low-latency engine buffer harmless: the copy is still bounded by the media
// clock, so it never reads a packet the engine has not written yet.
NTSTATUS OnSetRenderPacket(_In_ ACXSTREAM stream, _In_ ULONG, _In_ ULONG, _In_ ULONG)
{
	DEVICE_CONTEXT* ctx = GetStreamContext(stream)->Device;
	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	if (ctx->AudioRunning && ctx->AudioBuffer != nullptr)
	{
		CopyConsumedLocked(ctx, CurrentPositionLocked(ctx, nullptr));
	}
	KeReleaseSpinLock(&ctx->AudioLock, irql);
	return STATUS_SUCCESS;
}

NTSTATUS OnGetCurrentPacket(_In_ ACXSTREAM stream, _Out_ PULONG currentPacket)
{
	DEVICE_CONTEXT* ctx = GetStreamContext(stream)->Device;
	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	const ULONGLONG position = CurrentPositionLocked(ctx, nullptr);
	*currentPacket = ctx->AudioPacketSize == 0 ? 0 : static_cast<ULONG>(position / ctx->AudioPacketSize);
	KeReleaseSpinLock(&ctx->AudioLock, irql);
	return STATUS_SUCCESS;
}

NTSTATUS OnGetPresentationPosition(_In_ ACXSTREAM stream, _Out_ PULONGLONG positionInBlocks, _Out_ PULONGLONG qpcPosition)
{
	DEVICE_CONTEXT* ctx = GetStreamContext(stream)->Device;
	KIRQL irql;
	KeAcquireSpinLock(&ctx->AudioLock, &irql);
	const USHORT bytesPerFrame = ctx->PinBytesPerFrame;
	ULONGLONG position = CurrentPositionLocked(ctx, qpcPosition);
	// The stream is sent PacingLeadMs ahead of the media clock, so the listener is that far
	// behind the position the driver has consumed. Report what is being heard, or an app that
	// syncs to the position leads the audio by the lead.
	const ULONGLONG lead =
		static_cast<ULONGLONG>(ctx->PinSampleRate) * ctx->PacingLeadMs / 1000 * bytesPerFrame;
	position = position > lead ? position - lead : 0;
	*positionInBlocks = bytesPerFrame == 0 ? 0 : position / bytesPerFrame;
	KeReleaseSpinLock(&ctx->AudioLock, irql);
	return STATUS_SUCCESS;
}
} // namespace OpenA2dpDriver::Audio
