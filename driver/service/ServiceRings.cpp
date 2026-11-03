#include "ServiceInternal.h"

namespace OpenA2dpDriver::Service
{
// Releases the user-mode mapping. With owner set, only when that file object made it
// (a handle's cleanup must not unmap another handle's rings); nullptr unmaps always.
void UnmapRings(DEVICE_CONTEXT* ctx, PVOID owner)
{
	KeWaitForSingleObject(&ctx->RingLock, Executive, KernelMode, FALSE, nullptr);
	if (ctx->RingMdl == nullptr || (owner != nullptr && ctx->RingOwner != owner))
	{
		KeReleaseMutex(&ctx->RingLock, FALSE);
		return;
	}
	// A user-mode mapping must be released in the context of the process that mapped
	// it; device teardown runs in a system thread, so attach when needed.
	auto* process = static_cast<PEPROCESS>(ctx->RingProcess);
	if (ctx->RingUserVa != nullptr)
	{
		KAPC_STATE apc;
		bool attached = false;
		if (process != nullptr && process != IoGetCurrentProcess() &&
			PsGetProcessExitStatus(process) == STATUS_PENDING)
		{
			KeStackAttachProcess(process, &apc);
			attached = true;
		}
		// If the process already exited its address space is gone, so there is
		// nothing to unmap and touching it would hit another process.
		if (process == nullptr || process == IoGetCurrentProcess() || attached)
		{
			MmUnmapLockedPages(ctx->RingUserVa, ctx->RingMdl);
		}
		if (attached)
		{
			KeUnstackDetachProcess(&apc);
		}
	}
	IoFreeMdl(ctx->RingMdl);
	ctx->RingMdl = nullptr;
	ctx->RingUserVa = nullptr;
	ctx->RingProcess = nullptr;
	ctx->RingOwner = nullptr;
	InterlockedExchange(&ctx->RingsMapped, 0);
	KeReleaseMutex(&ctx->RingLock, FALSE);
	if (process != nullptr)
	{
		ObDereferenceObject(process);
	}
}

NTSTATUS MapRings(DEVICE_CONTEXT* ctx, PVOID owner)
{
	if (ctx->ServiceRawBase == nullptr)
	{
		return STATUS_INVALID_DEVICE_STATE;
	}
	KeWaitForSingleObject(&ctx->RingLock, Executive, KernelMode, FALSE, nullptr);
	if (ctx->RingMdl != nullptr)
	{
		KeReleaseMutex(&ctx->RingLock, FALSE);
		return STATUS_DEVICE_BUSY;
	}

	PMDL mdl = IoAllocateMdl(ctx->ServiceRawBase, kAllocationBytes, FALSE, FALSE, nullptr);
	if (mdl == nullptr)
	{
		KeReleaseMutex(&ctx->RingLock, FALSE);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	MmBuildMdlForNonPagedPool(mdl);

	// IOCTLs run in the caller's process context, so the pages land in the service.
	// A user-mode mapping raises on failure instead of returning nullptr.
	PVOID userVa = nullptr;
	__try
	{
		userVa = MmMapLockedPagesSpecifyCache(mdl, UserMode, MmCached, nullptr, FALSE, NormalPagePriority | MdlMappingNoExecute);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		userVa = nullptr;
	}
	if (userVa == nullptr)
	{
		IoFreeMdl(mdl);
		KeReleaseMutex(&ctx->RingLock, FALSE);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	// Keep the mapping process alive so teardown can attach and unmap.
	PEPROCESS process = IoGetCurrentProcess();
	ObReferenceObject(process);
	ctx->RingMdl = mdl;
	ctx->RingUserVa = userVa;
	ctx->RingProcess = process;
	ctx->RingOwner = owner;
	InterlockedExchange(&ctx->RingsMapped, 1);
	KeReleaseMutex(&ctx->RingLock, FALSE);
	return STATUS_SUCCESS;
}
} // namespace OpenA2dpDriver::Service
