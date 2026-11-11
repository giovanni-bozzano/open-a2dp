#include "SinkPlayback.h"

#include "Text.h"

#include <windows.h>

#include <audiopolicy.h>
#include <cfgmgr32.h>
#include <mmdeviceapi.h>

#include <cwchar>

namespace OpenA2dp::Tools {
namespace {

// The render endpoint a device node owns: Windows creates one per audio devnode, as
// SWD\MMDEVAPI\<endpoint id>, under the node that supplies the audio function. The endpoint id
// is the form the audio APIs hand out, so this rebuilds the devnode id from it.
bool EndpointDevNode(const std::wstring& endpointId, DEVINST& node)
{
	const std::wstring instanceId = L"SWD\\MMDEVAPI\\" + endpointId;
	node = 0;
	return CM_Locate_DevNodeW(&node, const_cast<DEVINSTID_W>(instanceId.c_str()),
							  CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS;
}

// True when `device` is `node` or one of its parents: the endpoint sits below the A2DP node,
// possibly through a filter the stack inserts between them.
bool DescendsFrom(DEVINST device, const std::wstring& nodeInstanceId)
{
	DEVINST current = device;
	for (int level = 0; level < 8; ++level)
	{
		wchar_t id[MAX_DEVICE_ID_LEN] = {};
		if (CM_Get_Device_IDW(current, id, ARRAYSIZE(id), 0) == CR_SUCCESS &&
			_wcsicmp(id, nodeInstanceId.c_str()) == 0)
		{
			return true;
		}
		DEVINST parent = 0;
		if (CM_Get_Parent(&parent, current, 0) != CR_SUCCESS)
		{
			return false;
		}
		current = parent;
	}
	return false;
}

// True when any session on that endpoint is active, which is audio being rendered. A paused
// player leaves its session inactive, and the sink can be switched then.
bool EndpointPlaying(IMMDevice* device)
{
	IAudioSessionManager2* manager = nullptr;
	if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
								reinterpret_cast<void**>(&manager))) ||
		manager == nullptr)
	{
		return false;
	}
	bool playing = false;
	IAudioSessionEnumerator* sessions = nullptr;
	if (SUCCEEDED(manager->GetSessionEnumerator(&sessions)) && sessions != nullptr)
	{
		int count = 0;
		if (SUCCEEDED(sessions->GetCount(&count)))
		{
			for (int index = 0; index < count && !playing; ++index)
			{
				IAudioSessionControl* control = nullptr;
				if (FAILED(sessions->GetSession(index, &control)) || control == nullptr)
				{
					continue;
				}
				AudioSessionState state = AudioSessionStateInactive;
				if (SUCCEEDED(control->GetState(&state)) && state == AudioSessionStateActive)
				{
					playing = true;
				}
				control->Release();
			}
		}
		sessions->Release();
	}
	manager->Release();
	return playing;
}

} // namespace

bool SinkPlaysAudio(const std::string& nodeInstanceId, bool& plays)
{
	plays = false;
	const std::wstring node = Widen(nodeInstanceId);

	// The window may already own the apartment; either way COM has to be initialised here.
	const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	const bool uninitialize = initialized == S_OK || initialized == S_FALSE;
	if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
	{
		return false;
	}

	bool read = false;
	IMMDeviceEnumerator* enumerator = nullptr;
	if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
								   __uuidof(IMMDeviceEnumerator),
								   reinterpret_cast<void**>(&enumerator))) &&
		enumerator != nullptr)
	{
		IMMDeviceCollection* endpoints = nullptr;
		if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &endpoints)) &&
			endpoints != nullptr)
		{
			UINT count = 0;
			if (SUCCEEDED(endpoints->GetCount(&count)))
			{
				for (UINT index = 0; index < count && !plays; ++index)
				{
					IMMDevice* device = nullptr;
					if (FAILED(endpoints->Item(index, &device)) || device == nullptr)
					{
						continue;
					}
					LPWSTR id = nullptr;
					if (SUCCEEDED(device->GetId(&id)) && id != nullptr)
					{
						DEVINST endpointNode = 0;
						if (EndpointDevNode(id, endpointNode) &&
							DescendsFrom(endpointNode, node))
						{
							read = true;
							plays = EndpointPlaying(device);
						}
						CoTaskMemFree(id);
					}
					device->Release();
				}
			}
			endpoints->Release();
		}
		enumerator->Release();
	}

	if (uninitialize)
	{
		CoUninitialize();
	}
	return read;
}

} // namespace OpenA2dp::Tools
