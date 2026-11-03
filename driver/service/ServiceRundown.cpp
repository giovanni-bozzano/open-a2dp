#include "ServiceInternal.h"

namespace OpenA2dpDriver::Service
{
// Enters a handler: returns the device, or nullptr once ServiceStop has begun.
DEVICE_CONTEXT* EnterControl(WDFDEVICE control)
{
	CONTROL_DEVICE_CONTEXT* cc = GetControlDeviceContext(control);
	if (!ExAcquireRundownProtection(&cc->Rundown))
	{
		return nullptr;
	}
	if (cc->Device == nullptr)
	{
		ExReleaseRundownProtection(&cc->Rundown);
		return nullptr;
	}
	return cc->Device;
}

void LeaveControl(WDFDEVICE control)
{
	ExReleaseRundownProtection(&GetControlDeviceContext(control)->Rundown);
}
} // namespace OpenA2dpDriver::Service
