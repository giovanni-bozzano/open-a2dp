#include "Driver.h"

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

NTSTATUS OnPrepareHardware(_In_ WDFDEVICE device, _In_ WDFCMRESLIST, _In_ WDFCMRESLIST)
{
	return L2capRetrieveRemoteAddress(GetDeviceContext(device));
}

NTSTATUS OnSelfManagedIoStart(_In_ WDFDEVICE device)
{
	// Failing to reach the headset is not fatal; the device stays started.
	(void)L2capOpenSignalingChannel(GetDeviceContext(device));
	return STATUS_SUCCESS;
}

NTSTATUS OnSelfManagedIoRestart(_In_ WDFDEVICE device)
{
	return OnSelfManagedIoStart(device);
}

NTSTATUS OnSelfManagedIoSuspend(_In_ WDFDEVICE device)
{
	DEVICE_CONTEXT* ctx = GetDeviceContext(device);
	WdfWorkItemFlush(ctx->DisconnectWorkItem);
	L2capCloseSignalingChannel(ctx);
	return STATUS_SUCCESS;
}

void OnDisconnectWorkItem(_In_ WDFWORKITEM workItem)
{
	L2capCloseSignalingChannel(GetDeviceContext(WdfWorkItemGetParentObject(workItem)));
}

void OnDeviceCleanup(_In_ WDFOBJECT object)
{
	DEVICE_CONTEXT* ctx = GetDeviceContext(object);
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

	WDF_OBJECT_ATTRIBUTES attrs;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, DEVICE_CONTEXT);
	attrs.EvtCleanupCallback = OnDeviceCleanup;

	WDFDEVICE device;
	NTSTATUS status = WdfDeviceCreate(&deviceInit, &attrs, &device);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	DEVICE_CONTEXT* ctx = GetDeviceContext(device);
	ctx->Device = device;

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

	return L2capInitialize(ctx);
}

} // namespace

extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT driverObject, _In_ PUNICODE_STRING registryPath)
{
	WDF_DRIVER_CONFIG config;
	WDF_DRIVER_CONFIG_INIT(&config, OnDeviceAdd);
	config.DriverPoolTag = OpenA2dpPoolTag;

	NTSTATUS status = WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
	OA2DP_LOG("DriverEntry 0x%08X", status);
	return status;
}
