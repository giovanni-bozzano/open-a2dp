#include "SignalingInternal.h"

using namespace OpenA2dpDriver::Signaling;

bool SignalingAbortSession(_In_ DEVICE_CONTEXT* ctx)
{
	if (ctx->SignalingState != ChannelOpen || ctx->SignalingWorkItemThread == KeGetCurrentThread())
	{
		return true;
	}

	LONGLONG lockTimeout = WDF_REL_TIMEOUT_IN_MS(2000);
	if (WdfWaitLockAcquire(ctx->SessionLock, &lockTimeout) != STATUS_SUCCESS)
	{
		OA2DP_WARN(ctx, "AVDTP", "teardown: session is busy, skipping CLOSE");
		return false;
	}
	const auto state = ctx->Session.State();
	// A normal CLOSE lets the headset release its SEP; ABORT alone left PANTONE reporting it in use.
	size_t length = ctx->Session.Close(ctx->WriteBuffer, sizeof(ctx->WriteBuffer));
	const bool closing = length != 0;
	if (!closing)
	{
		length = ctx->Session.Abort(ctx->WriteBuffer, sizeof(ctx->WriteBuffer));
	}
	if (length != 0)
	{
		OA2DP_INFO(ctx, "AVDTP", "%s before teardown (state %s)", closing ? "closing session" : "aborting session",
			AvdtpStateName(state));
		Transmit(ctx, length);
	}
	WdfWaitLockRelease(ctx->SessionLock);

	// Give the headset up to 1 s in total to answer, so the command takes effect before the
	// channel closes. The read work item applies the response; this thread only polls the
	// session state. Every lock wait is limited to what is left of the second.
	const ULONGLONG deadline = KeQueryInterruptTime() + 1000ull * 10000ull;
	for (;;)
	{
		const ULONGLONG now = KeQueryInterruptTime();
		if (now >= deadline)
		{
			break;
		}
		LONGLONG remaining = -static_cast<LONGLONG>(deadline - now);
		if (WdfWaitLockAcquire(ctx->SessionLock, &remaining) != STATUS_SUCCESS)
		{
			OA2DP_WARN(ctx, "AVDTP", "teardown: session is busy, not waiting for the response");
			return true;
		}
		const auto sessionState = ctx->Session.State();
		const bool waiting = sessionState == Avdtp::SessionState::Aborting || sessionState == Avdtp::SessionState::Closing;
		WdfWaitLockRelease(ctx->SessionLock);
		if (!waiting)
		{
			return true;
		}
		LARGE_INTEGER delay;
		delay.QuadPart = -100000LL; // 10 ms
		KeDelayExecutionThread(KernelMode, FALSE, &delay);
	}
	OA2DP_WARN(ctx, "AVDTP", "%s response not received within 1 s", closing ? "CLOSE" : "ABORT");
	return true;
}

void SignalingStop(_In_ DEVICE_CONTEXT* ctx)
{
	LARGE_INTEGER timeout;
	timeout.QuadPart = -10000000LL; // 1 s

	WdfRequestCancelSentRequest(ctx->ReadRequest);
	if (KeWaitForSingleObject(&ctx->ReadIdle, Executive, KernelMode, FALSE, &timeout) != STATUS_SUCCESS)
	{
		OA2DP_WARN(ctx, "Signaling", "stop: read did not complete within 1 s");
	}
	WdfTimerStop(ctx->ResponseTimer, TRUE);
	WdfTimerStop(ctx->ConnectDelayTimer, TRUE);
	InterlockedExchange(&ctx->DeferredConnectPending, 0);
	if (ctx->SignalingWorkItemThread != KeGetCurrentThread())
	{
		WdfWorkItemFlush(ctx->SignalingWorkItem);
	}
	// A read may have been resubmitted by the work item before it saw the state change.
	WdfRequestCancelSentRequest(ctx->ReadRequest);
	timeout.QuadPart = -10000000LL;
	if (KeWaitForSingleObject(&ctx->ReadIdle, Executive, KernelMode, FALSE, &timeout) != STATUS_SUCCESS)
	{
		OA2DP_WARN(ctx, "Signaling", "stop: read did not complete within 1 s (2nd)");
	}
}
