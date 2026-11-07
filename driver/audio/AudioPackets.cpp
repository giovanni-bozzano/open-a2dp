#include "AudioInternal.h"

namespace OpenA2dpDriver::Audio
{
NTSTATUS OnAllocateRtPackets(_In_ ACXSTREAM stream, _In_ ULONG packetCount, _In_ ULONG packetSize, _Out_ PACX_RTPACKET* packets)
{
	*packets = nullptr;
	DEVICE_CONTEXT* ctx = GetStreamContext(stream)->Device;
	if (packetCount == 0 || packetCount > AudioMaxPacketCount || packetSize == 0 ||
		ctx->PinBytesPerFrame == 0 || packetSize % ctx->PinBytesPerFrame != 0)
	{
		OA2DP_WARN(ctx, "Audio", "allocate %u packets of %u bytes rejected", packetCount, packetSize);
		return STATUS_NOT_SUPPORTED;
	}

	// Whole pages, zeroed, so no stale kernel memory is mapped to user mode.
	const SIZE_T bufferSize = ROUND_TO_PAGES(static_cast<SIZE_T>(packetSize) * packetCount);

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

NTSTATUS OnGetHwLatency(_In_ ACXSTREAM, _Out_ ULONG* fifoSize, _Out_ ULONG* delay)
{
	*fifoSize = 128;
	*delay = 0;
	return STATUS_SUCCESS;
}

NTSTATUS OnSetRenderPacket(_In_ ACXSTREAM, _In_ ULONG, _In_ ULONG, _In_ ULONG)
{
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
	*positionInBlocks = bytesPerFrame == 0 ? 0 : CurrentPositionLocked(ctx, qpcPosition) / bytesPerFrame;
	KeReleaseSpinLock(&ctx->AudioLock, irql);
	return STATUS_SUCCESS;
}
} // namespace OpenA2dpDriver::Audio
