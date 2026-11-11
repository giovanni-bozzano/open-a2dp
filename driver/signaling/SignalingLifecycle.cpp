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

	// One shot: armed while a provider is missing, disarmed again by its registration, so a
	// service restart never reaches the renegotiation.
	WDF_TIMER_CONFIG lossConfig;
	WDF_TIMER_CONFIG_INIT(&lossConfig, OnProviderLossTimeout);
	status = WdfTimerCreate(&lossConfig, &attrs, &ctx->ProviderLossTimer);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	WDF_TIMER_CONFIG graceConfig;
	WDF_TIMER_CONFIG_INIT(&graceConfig, OnPauseGraceTimeout);
	status = WdfTimerCreate(&graceConfig, &attrs, &ctx->PauseGraceTimer);
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

static void RequestFormatRebuild(DEVICE_CONTEXT* ctx)
{
	// Rebuilding notifies ACX, and this can be called with the control lock held (the encoder
	// service's cleanup), so the work item does it at PASSIVE with no lock of ours held.
	InterlockedExchange(&ctx->FormatsPending, 1);
	WdfWorkItemEnqueue(ctx->SignalingWorkItem);
}

static void RequestCodecSwitch(DEVICE_CONTEXT* ctx)
{
	// The work item recomputes the desired codec from the current inputs, so events
	// that arrive while it is already queued are safely coalesced.
	InterlockedExchange(&ctx->CodecSwitchPending, 1);
	WdfWorkItemEnqueue(ctx->SignalingWorkItem);
}

namespace OpenA2dpDriver::Signaling
{
void OnProviderLossTimeout(_In_ WDFTIMER timer)
{
	DEVICE_CONTEXT* ctx = GetDeviceContext(WdfTimerGetParentObject(timer));
	// The provider did not come back: the codec it served can no longer run, so let the work
	// item rebuild the stream around one the driver encodes itself. No log here; the codec
	// switch that follows reports what it negotiated.
	InterlockedExchange(&ctx->ProviderFallback, 1);
	InterlockedExchange(&ctx->CodecSwitchPending, 1);
	WdfWorkItemEnqueue(ctx->SignalingWorkItem);
}

void OnPauseGraceTimeout(_In_ WDFTIMER timer)
{
	// The endpoint stayed paused for the whole grace: now the stream is suspended.
	DEVICE_CONTEXT* ctx = GetDeviceContext(WdfTimerGetParentObject(timer));
	InterlockedExchange(&ctx->PauseGrace, 0);
	InterlockedExchange(&ctx->EndpointStreamPending, 1);
	WdfWorkItemEnqueue(ctx->SignalingWorkItem);
}
} // namespace OpenA2dpDriver::Signaling

void SignalingOnEncoderRegistered(_In_ DEVICE_CONTEXT* ctx)
{
	// A provider that came back cancels a pending fallback: a service restart must not cost a
	// renegotiation, and the stored preference applies again.
	WdfTimerStop(ctx->ProviderLossTimer, FALSE);
	InterlockedExchange(&ctx->ProviderFallback, 0);
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

void SignalingOnEncoderUnregistered(_In_ DEVICE_CONTEXT* ctx)
{
	// The 24-bit formats are listed only while an aptX HD provider is registered; without the
	// rebuild the endpoint keeps offering a depth nothing can encode after the service exits.
	// The work item also checks whether the configured stream was served by the provider that
	// left, and arms the fallback for it.
	InterlockedExchange(&ctx->ProviderLossCheck, 1);
	RequestFormatRebuild(ctx);
}

void SignalingOnPreferenceChanged(_In_ DEVICE_CONTEXT* ctx)
{
	// A new selection replaces the fallback decision the old one produced.
	WdfTimerStop(ctx->ProviderLossTimer, FALSE);
	InterlockedExchange(&ctx->ProviderFallback, 0);
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

void SignalingOnStreamFormatChanged(_In_ DEVICE_CONTEXT* ctx)
{
	// The listing already follows the preference; only the codec choice has to be
	// reconsidered, because a different depth makes a different codec usable.
	RequestCodecSwitch(ctx);
}

void SignalingOnAudioRun(_In_ DEVICE_CONTEXT* ctx)
{
	InterlockedExchange(&ctx->AudioRunPending, 1);
	// A resume inside the pause grace finds the stream still running; nothing has to start.
	InterlockedExchange(&ctx->PauseGrace, 0);
	WdfTimerStop(ctx->PauseGraceTimer, FALSE);
	// A new run earns the stream a fresh set of START attempts. The START itself waits for the
	// first audio (SignalingOnAudioHeard); the edge still settles a session out of step with it.
	InterlockedExchange(&ctx->EndpointStartAttempts, 0);
	InterlockedExchange(&ctx->EndpointStreamPending, 1);
	WdfWorkItemEnqueue(ctx->SignalingWorkItem);
}

void SignalingOnAudioHeard(_In_ DEVICE_CONTEXT* ctx)
{
	InterlockedExchange(&ctx->EndpointStreamPending, 1);
	WdfWorkItemEnqueue(ctx->SignalingWorkItem);
}

void SignalingOnAudioPause(_In_ DEVICE_CONTEXT* ctx)
{
	// The endpoint is the reason the stream exists, so a pause suspends it - after a grace, so a
	// short pause resumes without a START round trip. The work item sends SUSPEND when the timer
	// fires, which is legal from Streaming alone.
	InterlockedExchange(&ctx->PauseGrace, 1);
	WdfTimerStart(ctx->PauseGraceTimer, WDF_REL_TIMEOUT_IN_MS(PauseGraceMs));
	InterlockedExchange(&ctx->EndpointStreamPending, 1);
	WdfWorkItemEnqueue(ctx->SignalingWorkItem);
}

void SignalingOnMediaChannelOpen(_In_ DEVICE_CONTEXT* ctx)
{
	// Opening the transport says nothing about whether audio is playing: the endpoint edge is
	// what decides START, and the work item reads this through the same check as every other
	// edge. A sink that opens the channel while Windows plays finds the stream already running.
	InterlockedExchange(&ctx->EndpointStreamPending, 1);
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
