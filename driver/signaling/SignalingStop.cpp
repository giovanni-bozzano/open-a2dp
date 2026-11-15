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
	// A normal CLOSE lets the sink release its SEP; ABORT alone can leave the sink reporting it in use.
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

	// Give the sink up to 1 s in total to answer, so the command takes effect before the
	// channel closes. The read work item applies the response and sets SessionChanged;
	// this thread waits for that without holding SessionLock. Clearing the event before
	// the check means a transition after the check sets it again, so none is missed.
	// Every wait is limited to what is left of the second.
	const ULONGLONG deadline = KeQueryInterruptTime() + 1000ull * 10000ull;
	for (;;)
	{
		KeClearEvent(&ctx->SessionChanged);
		ULONGLONG now = KeQueryInterruptTime();
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
		now = KeQueryInterruptTime();
		if (now >= deadline)
		{
			break;
		}
		LARGE_INTEGER wait;
		wait.QuadPart = -static_cast<LONGLONG>(deadline - now);
		KeWaitForSingleObject(&ctx->SessionChanged, Executive, KernelMode, FALSE, &wait);
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
	// A fallback armed for this link must not fire into a teardown that already stopped media.
	WdfTimerStop(ctx->ProviderLossTimer, TRUE);
	InterlockedExchange(&ctx->ProviderLossCheck, 0);
	// Nor may the pause grace suspend a stream this teardown already ended.
	WdfTimerStop(ctx->PauseGraceTimer, TRUE);
	InterlockedExchange(&ctx->PauseGrace, 0);
	// The wake belongs to that stream: the next session asks for its own.
	InterlockedExchange(&ctx->LinkWakePending, 0);
	// The wake slot belongs to that stream too, even if its completion never came.
	InterlockedExchange(&ctx->ActiveWakeInFlight, 0);
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
