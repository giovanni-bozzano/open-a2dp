#pragma once

#include "../Driver.h"
#include <ntstrsafe.h>

// Subsystem-private declarations; not a public driver interface.
namespace OpenA2dpDriver::L2cap
{
// A control BRB (register/unregister server, open/close channel) in flight. The BRB
// lives here rather than on the caller's stack so a caller that stops waiting can
// return while the transport still owns it. Two references: the caller's and the
// completion routine's; the last one to let go frees the operation.
struct BRB_OP
{
	DEVICE_CONTEXT* Ctx;
	WDFREQUEST Request;
	WDFMEMORY Memory;
	// Storage for the operation itself, parented to the device: a waiter that abandons an
	// operation the transport never completes would otherwise leak this block, because only
	// the completion routine ever releases the last reference.
	WDFMEMORY Block;
	KEVENT Done;
	volatile LONG State; // 0 running, 1 completed, 2 abandoned by its waiter
	volatile LONG Refs;
	BOOLEAN IsOpen;
	BOOLEAN IsRegister;
	// Set for the active-mode BRB, whose completion time the status reports as the wake.
	BOOLEAN IsActiveMode;
	ULONGLONG StartedAt;
	NTSTATUS Status;
	// Optional completion notification for operations nobody waits for: the flag is set
	// and the work item enqueued, unless the owner abandoned the operation.
	volatile LONG* NotifyFlag;
	WDFWORKITEM NotifyWorkItem;
	union
	{
		struct _BRB_L2CA_OPEN_CHANNEL Open;
		struct _BRB_L2CA_CLOSE_CHANNEL Close;
		struct _BRB_L2CA_REGISTER_SERVER Register;
		struct _BRB_L2CA_UNREGISTER_SERVER Unregister;
		struct _BRB_ACL_ENTER_ACTIVE_MODE Active;
		struct _BRB_ACL_GET_MODE Mode;
	} Brb;
};

void BrbOpRelease(_In_ BRB_OP* op);
BRB_OP* BrbOpCreate(_In_ DEVICE_CONTEXT* ctx, _In_ BRB_TYPE type);
void OnBrbOpComplete(_In_ WDFREQUEST, _In_ WDFIOTARGET, _In_ PWDF_REQUEST_COMPLETION_PARAMS params, _In_ WDFCONTEXT context);
NTSTATUS BrbOpStart(_In_ BRB_OP* op);
void CloseOrphanedChannel(_In_ DEVICE_CONTEXT* ctx, _In_ L2CAP_CHANNEL_HANDLE channel);
void UnregisterOrphanedServer(_In_ DEVICE_CONTEXT* ctx, _In_ L2CAP_SERVER_HANDLE server);
// Orphan cleanup runs at PASSIVE_LEVEL from a work item, because a completion routine may
// run at DISPATCH_LEVEL and creating a WDF request there is not allowed.
NTSTATUS InitOrphanCleanup(_In_ DEVICE_CONTEXT* ctx);
void RequestOrphanCleanup(_In_ DEVICE_CONTEXT* ctx, _In_ PVOID handle, _In_ bool server);
void OnOrphanWorkItem(_In_ WDFWORKITEM workItem);
NTSTATUS BrbOpRun(_In_ BRB_OP* op, _In_ ULONG timeoutMs, _In_ bool abortable);
void OnSendComplete(_In_ WDFREQUEST, _In_ WDFIOTARGET, _In_ PWDF_REQUEST_COMPLETION_PARAMS params, _In_ WDFCONTEXT context);
NTSTATUS InitSendSlot(_In_ DEVICE_CONTEXT* ctx, _In_ WDF_OBJECT_ATTRIBUTES* attrs, _Inout_ SEND_SLOT* slot);
void OnIndication(_In_opt_ PVOID context, _In_ INDICATION_CODE indication, _In_ PINDICATION_PARAMETERS parameters);
void OnOpenComplete(
	_In_ WDFREQUEST, _In_ WDFIOTARGET, _In_ PWDF_REQUEST_COMPLETION_PARAMS params, _In_ WDFCONTEXT context);
void InitSignalingConfig(_In_ DEVICE_CONTEXT* ctx, _Inout_ struct _BRB_L2CA_OPEN_CHANNEL& brb);
NTSTATUS SendOpenRequest(_In_ DEVICE_CONTEXT* ctx);
void OnRejectComplete(_In_ WDFREQUEST, _In_ WDFIOTARGET, _In_ PWDF_REQUEST_COMPLETION_PARAMS params, _In_ WDFCONTEXT context);
void RespondIncoming(_In_ DEVICE_CONTEXT* ctx, _In_ L2CAP_CHANNEL_HANDLE handle, _In_ BTH_ADDR address, _In_ bool accept);
void OnServerIndication(_In_opt_ PVOID context, _In_ INDICATION_CODE indication, _In_ PINDICATION_PARAMETERS parameters);
} // namespace OpenA2dpDriver::L2cap
