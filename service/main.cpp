#include <OpenA2dp/Ioctl.h>
#include <OpenA2dp/RingBuffer.h>
#include <OpenA2dp/AacConfig.h>
#include <OpenA2dp/IA2dpCodec.h>
#include <OpenA2dp/AacStreamEncoder.h>

#include <windows.h>
#include <winioctl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

using namespace OpenA2dp;

namespace {

// QueryPerformanceCounter-based clock in microseconds.
static uint64_t ClockUs()
{
	static LARGE_INTEGER freq = {};
	if (freq.QuadPart == 0) {
		QueryPerformanceFrequency(&freq);
	}
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return static_cast<uint64_t>(now.QuadPart) * 1000000ULL / static_cast<uint64_t>(freq.QuadPart);
}

// Enumerate control device links (OpenA2dpCtl-<address>).
std::vector<std::wstring> EnumerateControlDevices()
{
	std::vector<std::wstring> result;
	// QueryDosDevice with a null prefix lists every DOS device. The list changes
	// while devices come and go, so grow the buffer instead of giving up once.
	std::vector<wchar_t> buffer(32768);
	for (;;) {
		const DWORD len = QueryDosDeviceW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (len == 0) {
			if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
				return result;
			}
			buffer.resize(buffer.size() * 2);
			continue;
		}

		const wchar_t* p = buffer.data();
		while (*p != L'\0') {
			std::wstring name(p);
			if (name.rfind(Ioctl::kControlLinkPrefix, 0) == 0) {
				result.push_back(name);
			}
			p += name.size() + 1;
		}
		return result;
	}
}

std::string WstrToUtf8(const std::wstring& w)
{
	if (w.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
	if (n <= 0) return {};
	std::string s(n - 1, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
	return s;
}

// Build an AAC IE from WaitEventOut params.
void BuildIe(uint32_t sampleRate, uint32_t channels, uint32_t bitrate, uint32_t objectType, uint8_t ie[6])
{
	AacConfig::Params p;
	p.sampleRate = sampleRate;
	p.channels = static_cast<uint8_t>(channels);
	p.bitrate = bitrate;
	p.objectType = static_cast<uint8_t>(objectType);
	p.vbr = false;
	AacConfig::WriteIe(p, ie);
}

// One device's event loop.
void DeviceLoop(const std::wstring& linkName)
{
	std::string linkUtf8 = WstrToUtf8(linkName);
	printf("[svc] opening %s\n", linkUtf8.c_str());
	fflush(stdout);

	std::wstring path = L"\\\\.\\" + linkName;
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
	                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
	                      FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		printf("[svc] CreateFile failed: %lu\n", GetLastError());
		return;
	}

	// Register as AAC encoder.
	Ioctl::RegisterEncoderIn regIn = {};
	regIn.abiVersion = Ioctl::kAbiVersion;
	regIn.codec = Ioctl::kCodecAac;
	regIn.maxFramePayload = Ioctl::kMaxFramePayload;

	Ioctl::RegisterEncoderOut regOut = {};
	DWORD bytes = 0;
	if (!DeviceIoControl(h, Ioctl::kIoctlRegisterEncoder, &regIn, sizeof(regIn),
	                     &regOut, sizeof(regOut), &bytes, nullptr)) {
		printf("[svc] REGISTER_ENCODER failed: %lu\n", GetLastError());
		CloseHandle(h);
		return;
	}
	if (regOut.abiVersion != Ioctl::kAbiVersion) {
		printf("[svc] ABI mismatch: %u vs %u\n", regOut.abiVersion, Ioctl::kAbiVersion);
		CloseHandle(h);
		return;
	}
	printf("[svc] registered (max frame %u, %u Hz, %u ch)\n", regOut.maxFramePayload,
	       regOut.pcmSampleRate, regOut.pcmChannels);
	fflush(stdout);

	// Map rings.
	Ioctl::SharedRings rings = {};
	if (!DeviceIoControl(h, Ioctl::kIoctlMapRings, nullptr, 0, &rings, sizeof(rings), &bytes, nullptr)) {
		printf("[svc] MAP_RINGS failed: %lu\n", GetLastError());
		CloseHandle(h);
		return;
	}
	if (rings.magic != Ioctl::kRingsMagic || rings.abiVersion != Ioctl::kAbiVersion) {
		printf("[svc] bad rings magic/abi: 0x%08X / %u\n", rings.magic, rings.abiVersion);
		CloseHandle(h);
		return;
	}

	// Build RingBufferViews from the mapped memory.
	RingHeader* pcmHeader = reinterpret_cast<RingHeader*>(rings.userBase + rings.pcmHeaderOffset);
	uint8_t* pcmData = reinterpret_cast<uint8_t*>(rings.userBase + rings.pcmDataOffset);
	RingHeader* frameHeader = reinterpret_cast<RingHeader*>(rings.userBase + rings.frameHeaderOffset);
	uint8_t* frameData = reinterpret_cast<uint8_t*>(rings.userBase + rings.frameDataOffset);

	RingBufferView pcmRing(pcmHeader, pcmData);
	RingBufferView frameRing(frameHeader, frameData);

	printf("[svc] rings mapped: PCM %u B, frames %u B\n", rings.pcmCapacity, rings.frameCapacity);
	fflush(stdout);

	AacStreamEncoder streamEncoder;

	// Main event loop.
	for (;;) {
		Ioctl::WaitEventOut ev = {};
		DWORD evBytes = 0;
		if (!DeviceIoControl(h, Ioctl::kIoctlWaitEvent, nullptr, 0, &ev, sizeof(ev), &evBytes, nullptr)) {
			printf("[svc] WAIT_EVENT failed: %lu\n", GetLastError());
			break;
		}

		uint64_t nowUs = ClockUs();

		if (ev.events & Ioctl::kEventDeviceGone) {
			printf("[svc] device gone (events=0x%08X)\n", ev.events);
			fflush(stdout);
			break;
		}
		// Ticks arrive every 10 ms; logging them would grow the redirected stdout
		// without bound, so only lifecycle changes are printed.
		if ((ev.events & ~Ioctl::kEventTick) != 0) {
			printf("[svc] event: 0x%08X state=%u codec=%u\n", ev.events, (unsigned)ev.state, ev.codec);
			fflush(stdout);
		}

		if (ev.events & Ioctl::kEventStreamStart) {
			printf("[svc] stream start: %u Hz, %u ch, %u bps, obj 0x%02X\n",
			       ev.sampleRate, ev.channels, ev.bitrate, ev.objectType);
			fflush(stdout);

			// Build IE and create encoder.
			uint8_t ie[6] = {};
			BuildIe(ev.sampleRate, ev.channels, ev.bitrate, ev.objectType, ie);

			A2dpCodecConfig config = {};
			config.codec = CodecType::Aac;
			std::memcpy(config.ie, ie, 6);
			config.ieLength = 6;

			auto codec = CreateCodec(CodecType::Aac);
			std::unique_ptr<IA2dpEncoder> encoder;
			HRESULT hr = codec->CreateEncoder(config, encoder);
			if (FAILED(hr)) {
				printf("[svc] CreateEncoder failed: 0x%08X\n", (unsigned)hr);
				fflush(stdout);
				continue;
			}

			AacStreamParams params;
			params.sampleRate = ev.sampleRate;
			params.channels = static_cast<uint16_t>(ev.channels);
			params.bitrate = ev.bitrate;
			params.objectType = ev.objectType;

			if (!streamEncoder.Start(std::move(encoder), params, pcmRing, nowUs)) {
				printf("[svc] AacStreamEncoder::Start failed\n");
				fflush(stdout);
				continue;
			}
		}

		if (ev.events & Ioctl::kEventTick) {
			if (streamEncoder.Active()) {
				if (!streamEncoder.Pump(nowUs, pcmRing, frameRing, 8)) {
					printf("[svc] Pump failed, stopping\n");
					fflush(stdout);
					streamEncoder.Stop();
				}
			}
		}

		if (ev.events & Ioctl::kEventStreamStop) {
			printf("[svc] stream stop\n");
			fflush(stdout);
			streamEncoder.Stop();
		}
	}

	streamEncoder.Stop();
	CloseHandle(h);
	printf("[svc] %s done\n", linkUtf8.c_str());
	fflush(stdout);
}

} // namespace

int main()
{
	printf("OpenA2dpSvc starting\n");
	fflush(stdout);

	// Main loop: enumerate devices and run a thread per device.
	for (;;) {
		auto devices = EnumerateControlDevices();
		if (devices.empty()) {
			printf("[svc] no control devices found, waiting...\n");
			fflush(stdout);
			Sleep(2000);
			continue;
		}

		std::vector<std::thread> threads;
		for (const auto& dev : devices) {
			threads.emplace_back(DeviceLoop, dev);
		}
		for (auto& t : threads) {
			if (t.joinable()) t.join();
		}

		// After all devices are done (device gone), wait and re-enumerate.
		Sleep(1000);
	}
}
