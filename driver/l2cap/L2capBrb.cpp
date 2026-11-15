#include "L2capInternal.h"

namespace OpenA2dpDriver::L2cap
{
void BrbOpRelease(_In_ BRB_OP* op)
{
	if (InterlockedDecrement(&op->Refs) != 0)
	{
		return;
	}
	// The operation lives inside Block, so deleting it frees op: nothing may touch op after.
	const WDFMEMORY block = op->Block;
	WdfObjectDelete(op->Request); // also deletes the memory object parented to it
	WdfObjectDelete(block);
}

BRB_OP* BrbOpCreate(_In_ DEVICE_CONTEXT* ctx, _In_ BRB_TYPE type)
{
	// The operation is carried in a WDF memory object parented to the device, so it cannot
	// outlive the device even when a transport never completes the request and the completion
	// routine never releases it.
	WDF_OBJECT_ATTRIBUTES blockAttrs;
	WDF_OBJECT_ATTRIBUTES_INIT(&blockAttrs);
	blockAttrs.ParentObject = ctx->Device;
	WDFMEMORY block = nullptr;
	PVOID raw = nullptr;
	if (!NT_SUCCESS(WdfMemoryCreate(&blockAttrs, NonPagedPoolNx, OpenA2dpPoolTag, sizeof(BRB_OP), &block, &raw)))
	{
		return nullptr;
	}
	auto* op = static_cast<BRB_OP*>(raw);
	RtlZeroMemory(op, sizeof(*op));
	op->Ctx = ctx;
	op->Block = block;
	op->Refs = 2;
	op->IsOpen = type == BRB_L2CA_OPEN_CHANNEL;
	op->IsRegister = type == BRB_L2CA_REGISTER_SERVER;
	op->IsActiveMode = type == BRB_ACL_ENTER_ACTIVE_MODE;
	op->NotifyFlag = nullptr;
	op->NotifyWorkItem = nullptr;
	KeInitializeEvent(&op->Done, NotificationEvent, FALSE);

	WDF_OBJECT_ATTRIBUTES attrs;
	WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
	attrs.ParentObject = ctx->Device;
	if (!NT_SUCCESS(WdfRequestCreate(&attrs, ctx->IoTarget, &op->Request)))
	{
		WdfObjectDelete(block); // also frees op
		return nullptr;
	}
	attrs.ParentObject = op->Request;
	if (!NT_SUCCESS(WdfMemoryCreatePreallocated(&attrs, &op->Brb, sizeof(op->Brb), &op->Memory)))
	{
		WdfObjectDelete(op->Request);
		WdfObjectDelete(block);
		return nullptr;
	}
	ctx->Profile.BthInitializeBrb(reinterpret_cast<PBRB>(&op->Brb), type);
	return op;
}

void CloseOrphanedChannel(_In_ DEVICE_CONTEXT* ctx, _In_ L2CAP_CHANNEL_HANDLE channel);
void UnregisterOrphanedServer(_In_ DEVICE_CONTEXT* ctx, _In_ L2CAP_SERVER_HANDLE server);

void OnBrbOpComplete(_In_ WDFREQUEST, _In_ WDFIOTARGET, _In_ PWDF_REQUEST_COMPLETION_PARAMS params, _In_ WDFCONTEXT context)
{
	auto* op = static_cast<BRB_OP*>(context);
	op->Status = params->IoStatus.Status;
	if (op->IsActiveMode && NT_SUCCESS(op->Status) && op->StartedAt != 0)
	{
		// How long the stack took to bring the link out of sniff: the status report prints it
		// next to the link's state, so a slow start can be attributed without a log capture.
		const ULONGLONG elapsed = (KeQueryInterruptTime() - op->StartedAt) / 10000ull;
		const LONG ms = elapsed > 0x7FFFFFFFull ? 0x7FFFFFFF : static_cast<LONG>(elapsed);
		InterlockedExchange(&op->Ctx->LinkWakeMs, ms);
	}
	const LONG previous = InterlockedCompareExchange(&op->State, 1, 0);
	if (previous == 2 && NT_SUCCESS(op->Status))
	{
		// The waiter gave up and nobody will ever use this result; do not leak it. This is a
		// completion routine, so it may run at DISPATCH_LEVEL and must not create the cleanup
		// request itself: the work item does that at PASSIVE_LEVEL.
		if (op->IsOpen)
		{
			RequestOrphanCleanup(op->Ctx, op->Brb.Open.ChannelHandle, false);
		}
		else if (op->IsRegister)
		{
			RequestOrphanCleanup(op->Ctx, op->Brb.Register.ServerHandle, true);
		}
	}
	KeSetEvent(&op->Done, IO_NO_INCREMENT, FALSE);
	if (previous == 0 && op->NotifyWorkItem != nullptr)
	{
		// This routine's reference keeps op alive until the release below.
		InterlockedExchange(op->NotifyFlag, 1);
		WdfWorkItemEnqueue(op->NotifyWorkItem);
	}
	BrbOpRelease(op);
}

// Sends the operation. On failure the completion routine will not run, so the
// completion reference is dropped here.
NTSTATUS BrbOpStart(_In_ BRB_OP* op)
{
	DEVICE_CONTEXT* ctx = op->Ctx;
	op->StartedAt = KeQueryInterruptTime();
	NTSTATUS status = WdfIoTargetFormatRequestForInternalIoctlOthers(ctx->IoTarget, op->Request,
		IOCTL_INTERNAL_BTH_SUBMIT_BRB, op->Memory, nullptr, nullptr, nullptr, nullptr, nullptr);
	if (NT_SUCCESS(status))
	{
		WdfRequestSetCompletionRoutine(op->Request, OnBrbOpComplete, op);
		if (WdfRequestSend(op->Request, ctx->IoTarget, WDF_NO_SEND_OPTIONS))
		{
			return STATUS_SUCCESS;
		}
		status = WdfRequestGetStatus(op->Request);
	}
	op->Status = status;
	InterlockedExchange(&op->State, 1);
	KeSetEvent(&op->Done, IO_NO_INCREMENT, FALSE);
	BrbOpRelease(op);
	return status;
}

NTSTATUS InitOrphanCleanup(_In_ DEVICE_CONTEXT* ctx)
{
	KeInitializeSpinLock(&ctx->OrphanLock);
	ctx->OrphanCount = 0;
	WDF_OBJECT_ATTRIBUTES attrs;
	WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
	attrs.ParentObject = ctx->Device;
	WDF_WORKITEM_CONFIG config;
	WDF_WORKITEM_CONFIG_INIT(&config, OnOrphanWorkItem);
	return WdfWorkItemCreate(&config, &attrs, &ctx->OrphanWorkItem);
}

// Called from a completion routine, which may run at DISPATCH_LEVEL: only the queue is
// touched here. A handle that does not fit is reported rather than dropped silently, since
// the channel then stays open on the sink.
void RequestOrphanCleanup(_In_ DEVICE_CONTEXT* ctx, _In_ PVOID handle, _In_ bool server)
{
	bool queued = false;
	KIRQL irql;
	KeAcquireSpinLock(&ctx->OrphanLock, &irql);
	if (handle != nullptr && ctx->OrphanCount < OrphanQueueDepth)
	{
		ORPHAN_ENTRY& entry = ctx->OrphanQueue[ctx->OrphanCount++];
		entry.Handle = handle;
		entry.Server = server ? TRUE : FALSE;
		queued = true;
	}
	KeReleaseSpinLock(&ctx->OrphanLock, irql);
	if (!queued)
	{
		OA2DP_ERROR(ctx, "L2CAP", "cannot queue the cleanup of %s %p; it stays open on the sink",
			server ? "server" : "channel", handle);
		return;
	}
	WdfWorkItemEnqueue(ctx->OrphanWorkItem);
}

void OnOrphanWorkItem(_In_ WDFWORKITEM workItem)
{
	DEVICE_CONTEXT* ctx = GetDeviceContext(WdfWorkItemGetParentObject(workItem));
	for (;;)
	{
		ORPHAN_ENTRY entry = {};
		KIRQL irql;
		KeAcquireSpinLock(&ctx->OrphanLock, &irql);
		if (ctx->OrphanCount != 0)
		{
			entry = ctx->OrphanQueue[0];
			for (ULONG i = 1; i < ctx->OrphanCount; ++i)
			{
				ctx->OrphanQueue[i - 1] = ctx->OrphanQueue[i];
			}
			--ctx->OrphanCount;
		}
		KeReleaseSpinLock(&ctx->OrphanLock, irql);
		if (entry.Handle == nullptr)
		{
			return;
		}
		if (entry.Server)
		{
			UnregisterOrphanedServer(ctx, static_cast<L2CAP_SERVER_HANDLE>(entry.Handle));
		}
		else
		{
			CloseOrphanedChannel(ctx, static_cast<L2CAP_CHANNEL_HANDLE>(entry.Handle));
		}
	}
}

void CloseOrphanedChannel(_In_ DEVICE_CONTEXT* ctx, _In_ L2CAP_CHANNEL_HANDLE channel)
{
	BRB_OP* op = BrbOpCreate(ctx, BRB_L2CA_CLOSE_CHANNEL);
	if (op == nullptr)
	{
		return;
	}
	op->Brb.Close.BtAddress = ctx->RemoteAddress;
	op->Brb.Close.ChannelHandle = channel;
	op->State = 2; // nobody waits for this one
	(void)BrbOpStart(op);
	BrbOpRelease(op);
	OA2DP_WARN(ctx, "L2CAP", "closed a channel whose open finished after its waiter gave up");
}

void UnregisterOrphanedServer(_In_ DEVICE_CONTEXT* ctx, _In_ L2CAP_SERVER_HANDLE server)
{
	BRB_OP* op = BrbOpCreate(ctx, BRB_L2CA_UNREGISTER_SERVER);
	if (op == nullptr)
	{
		return;
	}
	op->Brb.Unregister.BtAddress = ctx->RemoteAddress;
	op->Brb.Unregister.Psm = AvdtpPsm;
	op->Brb.Unregister.ServerHandle = server;
	op->State = 2;
	(void)BrbOpStart(op);
	BrbOpRelease(op);
	OA2DP_WARN(ctx, "L2CAP", "unregistered a server whose registration finished after its waiter gave up");
}

// Runs a prepared BRB operation and waits at most timeoutMs. With abortable the wait
// also ends when teardown begins (StopEvent). When the wait ends early the request is
// cancelled and, if the transport still does not complete it, abandoned: the operation
// outlives this call and the completion routine cleans up. The caller still owns its
// reference and must call BrbOpRelease. Results in op->Brb are valid only when the
// returned status is a success.
NTSTATUS BrbOpRun(_In_ BRB_OP* op, _In_ ULONG timeoutMs, _In_ bool abortable)
{
	DEVICE_CONTEXT* ctx = op->Ctx;
	NTSTATUS status = BrbOpStart(op);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	LARGE_INTEGER timeout;
	timeout.QuadPart = -static_cast<LONGLONG>(timeoutMs) * 10000;
	NTSTATUS wait;
	if (abortable)
	{
		PVOID objects[2] = {&op->Done, &ctx->StopEvent};
		wait = KeWaitForMultipleObjects(2, objects, WaitAny, Executive, KernelMode, FALSE, &timeout, nullptr);
	}
	else
	{
		wait = KeWaitForSingleObject(&op->Done, Executive, KernelMode, FALSE, &timeout);
	}
	if (wait == STATUS_WAIT_0)
	{
		return op->Status;
	}

	WdfRequestCancelSentRequest(op->Request);
	timeout.QuadPart = -1000000LL; // 100 ms for a well-behaved transport to complete the cancel
	if (KeWaitForSingleObject(&op->Done, Executive, KernelMode, FALSE, &timeout) == STATUS_SUCCESS)
	{
		return op->Status;
	}
	if (InterlockedCompareExchange(&op->State, 2, 0) == 1)
	{
		return op->Status; // completed in the instant between the wait and the abandon
	}
	OA2DP_WARN(ctx, "L2CAP", "BRB did not complete (%s); abandoning it", wait == STATUS_TIMEOUT ? "timeout" : "teardown");
	return wait == STATUS_TIMEOUT ? STATUS_IO_TIMEOUT : STATUS_CANCELLED;
}
} // namespace OpenA2dpDriver::L2cap

// Asks the stack to bring the ACL out of sniff before a stream start: a link in sniff only
// carries packets in its sniff slots, so the AVDTP START and the audio behind it wait for the
// next one. Fire and forget - nothing a start depends on comes back, and a stack that refuses
// the request still sends everything that follows.
void L2capRequestActiveMode(_In_ DEVICE_CONTEXT* ctx)
{
	using namespace OpenA2dpDriver::L2cap;
	BRB_OP* op = BrbOpCreate(ctx, BRB_ACL_ENTER_ACTIVE_MODE);
	if (op == nullptr)
	{
		return;
	}
	op->Brb.Active.BtAddress = ctx->RemoteAddress;
	op->State = 2; // nobody waits for this one
	const NTSTATUS status = BrbOpStart(op);
	BrbOpRelease(op);
	if (!NT_SUCCESS(status))
	{
		OA2DP_INFO(ctx, "L2CAP", "the link keeps its power state; active mode was refused 0x%08X", status);
	}
}

// Reads the link's power state for the status report. The query is bounded, and a stack that does
// not answer leaves the report saying so instead of holding the status read: a status poll must
// never wait on a transport that is gone.
uint32_t L2capQueryLinkMode(_In_ DEVICE_CONTEXT* ctx)
{
	using namespace OpenA2dpDriver::L2cap;
	constexpr ULONG LinkModeQueryMs = 150;
	BRB_OP* op = BrbOpCreate(ctx, BRB_ACL_GET_MODE);
	if (op == nullptr)
	{
		return static_cast<uint32_t>(OpenA2dp::Ioctl::LinkMode::Unknown);
	}
	op->Brb.Mode.BtAddress = ctx->RemoteAddress;
	const NTSTATUS status = BrbOpRun(op, LinkModeQueryMs, true);
	OpenA2dp::Ioctl::LinkMode mode = OpenA2dp::Ioctl::LinkMode::Unknown;
	if (NT_SUCCESS(status))
	{
		switch (op->Brb.Mode.AclMode)
		{
		case ACL_MODE_ACTIVE: mode = OpenA2dp::Ioctl::LinkMode::Active; break;
		case ACL_MODE_HOLD: mode = OpenA2dp::Ioctl::LinkMode::Hold; break;
		case ACL_MODE_SNIFF: mode = OpenA2dp::Ioctl::LinkMode::Sniff; break;
		case ACL_MODE_PARK: mode = OpenA2dp::Ioctl::LinkMode::Park; break;
		default: break; // a transitional or disconnected state has no steady mode to report
		}
	}
	BrbOpRelease(op);
	return static_cast<uint32_t>(mode);
}
