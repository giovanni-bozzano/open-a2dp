#include "Driver.h"
#include "Service.h"
#include <devpkey.h>
#include <ntstrsafe.h>

extern "C" DRIVER_INITIALIZE DriverEntry;

namespace
{

EVT_WDF_DRIVER_DEVICE_ADD OnDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE OnPrepareHardware;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT OnSelfManagedIoStart;
EVT_WDF_DEVICE_SELF_MANAGED_IO_RESTART OnSelfManagedIoRestart;
EVT_WDF_DEVICE_SELF_MANAGED_IO_SUSPEND OnSelfManagedIoSuspend;
EVT_WDF_OBJECT_CONTEXT_CLEANUP OnDeviceCleanup;
EVT_WDF_WORKITEM OnDisconnectWorkItem;

// Names the device after the sink: "<name> [OpenA2DP]".
void AssignFriendlyName(_In_ DEVICE_CONTEXT* ctx)
{
	WCHAR friendlyName[BTH_MAX_NAME_SIZE + 32];
	NTSTATUS status = ctx->RemoteName[0] != L'\0'
		? RtlStringCchPrintfW(friendlyName, ARRAYSIZE(friendlyName), L"%ws [OpenA2DP]", ctx->RemoteName)
		: RtlStringCchPrintfW(friendlyName, ARRAYSIZE(friendlyName), L"%012I64X [OpenA2DP]", ctx->RemoteAddress);
	if (!NT_SUCCESS(status))
	{
		return;
	}

	WDF_DEVICE_PROPERTY_DATA property;
	WDF_DEVICE_PROPERTY_DATA_INIT(&property, &DEVPKEY_Device_FriendlyName);
	const ULONG bytes = static_cast<ULONG>((wcslen(friendlyName) + 1) * sizeof(WCHAR));
	status = WdfDeviceAssignProperty(ctx->Device, &property, DEVPROP_TYPE_STRING, bytes, friendlyName);
	OA2DP_INFO(ctx, "Device", "friendly name '%ws' 0x%08X", friendlyName, status);
}

NTSTATUS OnPrepareHardware(_In_ WDFDEVICE device, _In_ WDFCMRESLIST, _In_ WDFCMRESLIST)
{
	DEVICE_CONTEXT* ctx = GetDeviceContext(device);
	NTSTATUS status = L2capRetrieveRemoteAddress(ctx);
	if (NT_SUCCESS(status))
	{
		AssignFriendlyName(ctx);
	}
	return status;
}

NTSTATUS OnSelfManagedIoStart(_In_ WDFDEVICE device)
{
	// Failing to register or reach the sink is not fatal; the device stays started.
	DEVICE_CONTEXT* ctx = GetDeviceContext(device);
	KeClearEvent(&ctx->StopEvent);
	// A recovery budget belongs to the connection that armed it.
	ctx->CodecSwitchRecover = FALSE;
	ctx->CodecSwitchReconnects = 0;
	OA2DP_INFO(ctx, "Device", "starting: registering the AVDTP server and connecting to the sink");
	ServiceStart(ctx);
	(void)L2capRegisterServer(ctx);
	(void)L2capOpenSignalingChannel(ctx);
	return STATUS_SUCCESS;
}

NTSTATUS OnSelfManagedIoRestart(_In_ WDFDEVICE device)
{
	return OnSelfManagedIoStart(device);
}

NTSTATUS OnSelfManagedIoSuspend(_In_ WDFDEVICE device)
{
	DEVICE_CONTEXT* ctx = GetDeviceContext(device);
	OA2DP_INFO(ctx, "Device", "stopping: closing the AVDTP session, then the channels");
	if (!SignalingAbortSession(ctx))
	{
		// The signaling work item is busy, typically waiting for the encoder service.
		// Cut its waits short so the CLOSE can still go out, then retry once.
		KeSetEvent(&ctx->StopEvent, IO_NO_INCREMENT, FALSE);
		(void)SignalingAbortSession(ctx);
	}
	KeSetEvent(&ctx->StopEvent, IO_NO_INCREMENT, FALSE);
	MediaStop(ctx);
	ServiceStop(ctx);
	L2capUnregisterServer(ctx);
	WdfWorkItemFlush(ctx->DisconnectWorkItem);
	L2capCloseSignalingChannel(ctx);
	// Completions that arrived during the close (a late accept, a remote disconnect)
	// may have queued work; let it run out now, against a closed session, so nothing
	// of ours runs after suspend returns. Neither work item waits on this thread.
	WdfWorkItemFlush(ctx->SignalingWorkItem);
	WdfWorkItemFlush(ctx->DisconnectWorkItem);
	MediaStop(ctx);
	OA2DP_INFO(ctx, "Device", "stopped");
	return STATUS_SUCCESS;
}

void OnDisconnectWorkItem(_In_ WDFWORKITEM workItem)
{
	L2capCloseSignalingChannel(GetDeviceContext(WdfWorkItemGetParentObject(workItem)));
}

void OnDeviceCleanup(_In_ WDFOBJECT object)
{
	DEVICE_CONTEXT* ctx = GetDeviceContext(object);
	if (ctx->JackFlapTimer != nullptr)
	{
		WdfTimerStop(ctx->JackFlapTimer, TRUE);
	}
	// The timer may already have handed over to its work item, which rebuilds the format lists.
	if (ctx->JackFlapWorkItem != nullptr)
	{
		WdfWorkItemFlush(ctx->JackFlapWorkItem);
	}
	ServiceCleanup(ctx);
	if (ctx->ProfileValid)
	{
		ctx->Profile.Interface.InterfaceDereference(ctx->Profile.Interface.Context);
		ctx->ProfileValid = FALSE;
	}
}

NTSTATUS OnDeviceAdd(_In_ WDFDRIVER, _Inout_ PWDFDEVICE_INIT deviceInit)
{
	WDF_PNPPOWER_EVENT_CALLBACKS pnp;
	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
	pnp.EvtDevicePrepareHardware = OnPrepareHardware;
	pnp.EvtDeviceSelfManagedIoInit = OnSelfManagedIoStart;
	pnp.EvtDeviceSelfManagedIoRestart = OnSelfManagedIoRestart;
	pnp.EvtDeviceSelfManagedIoSuspend = OnSelfManagedIoSuspend;
	WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &pnp);

	ACX_DEVICEINIT_CONFIG acxInitConfig;
	ACX_DEVICEINIT_CONFIG_INIT(&acxInitConfig);
	NTSTATUS status = AcxDeviceInitInitialize(deviceInit, &acxInitConfig);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	WDF_OBJECT_ATTRIBUTES attrs;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, DEVICE_CONTEXT);
	attrs.EvtCleanupCallback = OnDeviceCleanup;

	WDFDEVICE device;
	status = WdfDeviceCreate(&deviceInit, &attrs, &device);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	ACX_DEVICE_CONFIG acxConfig;
	ACX_DEVICE_CONFIG_INIT(&acxConfig);
	status = AcxDeviceInitialize(device, &acxConfig);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	DEVICE_CONTEXT* ctx = GetDeviceContext(device);
	ctx->Device = device;

	// Arrival signal for the encoder service; ServiceStart/ServiceStop toggle it, so
	// the framework must not enable it on its own at start.
	status = WdfDeviceCreateDeviceInterface(device, &OpenA2dp::Ioctl::kControlArrivalInterface, nullptr);
	if (!NT_SUCCESS(status))
	{
		return status;
	}
	WdfDeviceSetDeviceInterfaceStateEx(device, &OpenA2dp::Ioctl::kControlArrivalInterface, nullptr, FALSE);

	WDF_WORKITEM_CONFIG workConfig;
	WDF_WORKITEM_CONFIG_INIT(&workConfig, OnDisconnectWorkItem);
	WDF_OBJECT_ATTRIBUTES workAttrs;
	WDF_OBJECT_ATTRIBUTES_INIT(&workAttrs);
	workAttrs.ParentObject = device;
	status = WdfWorkItemCreate(&workConfig, &workAttrs, &ctx->DisconnectWorkItem);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	status = L2capInitialize(ctx);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	status = MediaInitialize(ctx);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	// Allocates the shared PCM/frame block; must precede AudioInitialize.
	status = ServiceInitialize(ctx);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	// The endpoint type depends on the sink's Class of Device; fall back to speakers if unavailable.
	// The stored preference keys off the sink address, so the address is read first; a failed
	// query leaves only the parameters default.
	(void)L2capRetrieveRemoteAddress(ctx);
	ConfigLoadSettings(ctx);

	status = AudioInitialize(ctx);
	if (!NT_SUCCESS(status))
	{
		OA2DP_ERROR(ctx, "Device", "audio endpoint creation failed 0x%08X", status);
		return status;
	}

	return SignalingInitialize(ctx);
}

} // namespace

extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT driverObject, _In_ PUNICODE_STRING registryPath)
{
	// Once, before any device can be added: the table is shared by every endpoint.
	BuildRenderFormats();

	WDF_DRIVER_CONFIG config;
	WDF_DRIVER_CONFIG_INIT(&config, OnDeviceAdd);
	config.DriverPoolTag = OpenA2dpPoolTag;

	WDFDRIVER driver;
	NTSTATUS status = WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, &driver);
	if (NT_SUCCESS(status))
	{
		ACX_DRIVER_CONFIG acxConfig;
		ACX_DRIVER_CONFIG_INIT(&acxConfig);
		status = AcxDriverInitialize(driver, &acxConfig);
	}
	OA2DP_INFO(nullptr, "Driver", "DriverEntry 0x%08X, built " __DATE__ " " __TIME__, status);
	return status;
}
