#include "SignalingInternal.h"

using namespace OpenA2dpDriver::Signaling;

NTSTATUS SignalingInitialize(_In_ DEVICE_CONTEXT* ctx)
{
	KeInitializeEvent(&ctx->ReadIdle, NotificationEvent, TRUE);
	KeInitializeEvent(&ctx->SessionChanged, NotificationEvent, FALSE);
	ResetSession(ctx);

	WDF_OBJECT_ATTRIBUTES attrs;
	WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
	attrs.ParentObject = ctx->Device;

	NTSTATUS status = WdfWaitLockCreate(&attrs, &ctx->SessionLock);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	WDF_WORKITEM_CONFIG workConfig;
	WDF_WORKITEM_CONFIG_INIT(&workConfig, OnSignalingWorkItem);
	status = WdfWorkItemCreate(&workConfig, &attrs, &ctx->SignalingWorkItem);
	if (!NT_SUCCESS(status))
	{
		return status;
	}
	ctx->SignalSend.CompletionWorkItem = ctx->SignalingWorkItem;

	WDF_TIMER_CONFIG timerConfig;
	WDF_TIMER_CONFIG_INIT(&timerConfig, OnResponseTimeout);
	status = WdfTimerCreate(&timerConfig, &attrs, &ctx->ResponseTimer);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	status = WdfRequestCreate(&attrs, ctx->IoTarget, &ctx->ReadRequest);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	return WdfMemoryCreatePreallocated(&attrs, &ctx->ReadBrb, sizeof(ctx->ReadBrb), &ctx->ReadBrbMemory);
}

void SignalingRequestSuspend(_In_ DEVICE_CONTEXT* ctx)
{
	InterlockedExchange(&ctx->SuspendPending, 1);
	WdfWorkItemEnqueue(ctx->SignalingWorkItem);
}

void SignalingRequestMtuRenegotiation(_In_ DEVICE_CONTEXT* ctx)
{
	// One attempt per connection: the configuration is rebuilt from the same session, so a
	// second mismatch cannot be fixed by negotiating again.
	if (InterlockedIncrement(&ctx->MtuRenegotiationAttempts) > 1)
	{
		OA2DP_WARN(ctx, "Signaling", "the AAC bitrate still does not fit the media MTU; leaving the stream silent");
		return;
	}
	InterlockedExchange(&ctx->MtuRenegotiate, 1);
	WdfWorkItemEnqueue(ctx->SignalingWorkItem);
}

static void RequestCodecSwitch(DEVICE_CONTEXT* ctx)
{
	// The work item recomputes the desired codec from the current inputs, so events
	// that arrive while it is already queued are safely coalesced.
	InterlockedExchange(&ctx->CodecSwitchPending, 1);
	WdfWorkItemEnqueue(ctx->SignalingWorkItem);
}

void SignalingOnEncoderRegistered(_In_ DEVICE_CONTEXT* ctx)
{
	// The endpoint's 24-bit formats exist only while an aptX HD provider is registered, so the
	// listing is rebuilt before the work item considers which codec to negotiate.
	const NTSTATUS status = AudioApplyEndpointFormats(ctx);
	if (!NT_SUCCESS(status))
	{
		OA2DP_WARN(ctx, "Signaling", "applying the endpoint formats after registration failed 0x%08X",
			status);
	}
	RequestCodecSwitch(ctx);
}

void SignalingOnPreferenceChanged(_In_ DEVICE_CONTEXT* ctx)
{
	// The listed formats follow the preference: a forced 16-bit codec drops the 24-bit
	// entries, and forcing aptX HD restores them. Without this the stale listing keeps the
	// pin at its old depth and the work item refuses the requested codec.
	const NTSTATUS status = AudioApplyEndpointFormats(ctx);
	if (!NT_SUCCESS(status))
	{
		OA2DP_WARN(ctx, "Signaling", "applying the endpoint formats after a preference change failed "
			"0x%08X", status);
	}
	RequestCodecSwitch(ctx);
}

void SignalingOnAudioRun(_In_ DEVICE_CONTEXT* ctx)
{
	InterlockedExchange(&ctx->AudioRunPending, 1);
	WdfWorkItemEnqueue(ctx->SignalingWorkItem);
}

void SignalingOnMediaChannelOpen(_In_ DEVICE_CONTEXT* ctx)
{
	InterlockedExchange(&ctx->StartPending, 1);
	WdfWorkItemEnqueue(ctx->SignalingWorkItem);
}

void SignalingOnChannelOpen(_In_ DEVICE_CONTEXT* ctx)
{
	InterlockedExchange(&ctx->ConnectPending, 1);
	WdfWorkItemEnqueue(ctx->SignalingWorkItem);
}

void SignalingResetSession(_In_ DEVICE_CONTEXT* ctx)
{
	// Called after the signaling channel closed. The session is otherwise left in
	// whatever state the link died in (Open, Streaming), which both misreports the
	// connection and refuses a fresh SET_CONFIGURATION on reconnect. The signaling
	// work item reaches here through the media-open failure teardown while it holds
	// SessionLock, so only take the lock when this thread is not that work item.
	const bool lockHeld = ctx->SignalingWorkItemThread == KeGetCurrentThread();
	if (!lockHeld)
	{
		WdfWaitLockAcquire(ctx->SessionLock, nullptr);
	}
	ResetSession(ctx);
	if (!lockHeld)
	{
		WdfWaitLockRelease(ctx->SessionLock);
	}
}
