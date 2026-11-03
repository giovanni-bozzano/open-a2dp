#include "L2capInternal.h"

namespace OpenA2dpDriver::L2cap
{
void BrbOpRelease(_In_ BRB_OP* op)
{
	if (InterlockedDecrement(&op->Refs) == 0)
	{
		WdfObjectDelete(op->Request); // also deletes the memory object parented to it
		ExFreePoolWithTag(op, OpenA2dpPoolTag);
	}
}

BRB_OP* BrbOpCreate(_In_ DEVICE_CONTEXT* ctx, _In_ BRB_TYPE type)
{
	auto* op = static_cast<BRB_OP*>(ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(BRB_OP), OpenA2dpPoolTag));
	if (op == nullptr)
	{
		return nullptr;
	}
	op->Ctx = ctx;
	op->Refs = 2;
	op->IsOpen = type == BRB_L2CA_OPEN_CHANNEL;
	op->IsRegister = type == BRB_L2CA_REGISTER_SERVER;
	op->NotifyFlag = nullptr;
	op->NotifyWorkItem = nullptr;
	KeInitializeEvent(&op->Done, NotificationEvent, FALSE);

	WDF_OBJECT_ATTRIBUTES attrs;
	WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
	attrs.ParentObject = ctx->Device;
	if (!NT_SUCCESS(WdfRequestCreate(&attrs, ctx->IoTarget, &op->Request)))
	{
		ExFreePoolWithTag(op, OpenA2dpPoolTag);
		return nullptr;
	}
	attrs.ParentObject = op->Request;
	if (!NT_SUCCESS(WdfMemoryCreatePreallocated(&attrs, &op->Brb, sizeof(op->Brb), &op->Memory)))
	{
		WdfObjectDelete(op->Request);
		ExFreePoolWithTag(op, OpenA2dpPoolTag);
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
	const LONG previous = InterlockedCompareExchange(&op->State, 1, 0);
	if (previous == 2 && NT_SUCCESS(op->Status))
	{
		// The waiter gave up and nobody will ever use this result; do not leak it.
		if (op->IsOpen)
		{
			CloseOrphanedChannel(op->Ctx, op->Brb.Open.ChannelHandle);
		}
		else if (op->IsRegister)
		{
			UnregisterOrphanedServer(op->Ctx, op->Brb.Register.ServerHandle);
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
