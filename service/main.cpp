#include <OpenA2dp/Ioctl.h>
#include <OpenA2dp/RingBuffer.h>
#include <OpenA2dp/AacConfig.h>
#include <OpenA2dp/IA2dpCodec.h>
#include <OpenA2dp/AacStreamEncoder.h>
#include <OpenA2dp/Aptx.h>
#include <OpenA2dp/AptxHd.h>
#include <OpenA2dp/AptxHdStreamEncoder.h>
#include <OpenA2dp/AptxStreamEncoder.h>
#include <OpenA2dp/LdacStreamEncoder.h>

#include <windows.h>
#include <avrt.h>
#include <winioctl.h>
#include <cfgmgr32.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

using namespace OpenA2dp;

namespace {

constexpr wchar_t kServiceName[] = L"OpenA2dpSvc";
// How long a service stop waits for the per-device workers before abandoning them.
constexpr DWORD kWorkerStopWaitMs = 3000;

// Set when the service (or Ctrl+C in console mode) asks the loop to stop.
HANDLE g_stopEvent = nullptr;
// Console mode writes to stdout (redirectable); service mode appends to a log file.
bool g_consoleMode = true;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
std::mutex g_logLock;

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

// One log line, timestamped. In service mode stdout does not exist, so the log
// goes to %ProgramData%\OpenA2dp\OpenA2dpSvc.log; a failure to open it must not
// stop the service.
void Log(const char* format, ...)
{
	char message[512];
	va_list args;
	va_start(args, format);
	_vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
	va_end(args);

	SYSTEMTIME st{};
	GetLocalTime(&st);
	char line[600];
	_snprintf_s(line, sizeof(line), _TRUNCATE, "%04u-%02u-%02u %02u:%02u:%02u %s\r\n", st.wYear, st.wMonth, st.wDay,
				st.wHour, st.wMinute, st.wSecond, message);

	std::lock_guard<std::mutex> guard(g_logLock);
	if (g_consoleMode) {
		fputs(line, stdout);
		fflush(stdout);
		return;
	}
	if (g_logFile != INVALID_HANDLE_VALUE) {
		DWORD written = 0;
		WriteFile(g_logFile, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
	}
}

void OpenLog()
{
	wchar_t programData[MAX_PATH];
	const DWORD len = GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
	if (len == 0 || len >= MAX_PATH) {
		return;
	}
	std::wstring dir = std::wstring(programData) + L"\\OpenA2dp";
	CreateDirectoryW(dir.c_str(), nullptr);
	std::wstring path = dir + L"\\OpenA2dpSvc.log";

	// Keep the log from growing without bound across service restarts; it only
	// records lifecycle events, so a few hundred KB is a lot of history.
	WIN32_FILE_ATTRIBUTE_DATA info{};
	if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) &&
		info.nFileSizeHigh == 0 && info.nFileSizeLow > (1u << 20)) {
		DeleteFileW(path.c_str());
	}
	g_logFile = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
							OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// Synchronous IOCTL on an overlapped handle: the handle is opened with
// FILE_FLAG_OVERLAPPED so WAIT_EVENT can be cancelled, which makes every other
// call need an OVERLAPPED as well. The wait is bounded: a driver that stops
// answering must not take the service thread (and with it the service stop) along.
// The tools have their own wrapper because it reports through std::string, and the
// service must not pull that in; both use Ioctl::kIoctlTimeoutMs.
bool IoctlSync(HANDLE device, DWORD code, void* in, DWORD inLength, void* out, DWORD outLength, DWORD* bytes)
{
	OVERLAPPED ov = {};
	ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (ov.hEvent == nullptr) {
		return false;
	}
	DWORD returned = 0;
	BOOL ok = DeviceIoControl(device, code, in, inLength, out, outLength, &returned, &ov);
	if (!ok && GetLastError() == ERROR_IO_PENDING) {
		if (WaitForSingleObject(ov.hEvent, Ioctl::kIoctlTimeoutMs) == WAIT_OBJECT_0) {
			ok = GetOverlappedResult(device, &ov, &returned, FALSE);
		} else {
			CancelIoEx(device, &ov);
			WaitForSingleObject(ov.hEvent, Ioctl::kCancelGraceMs);
			SetLastError(ERROR_TIMEOUT);
			ok = FALSE;
		}
	}
	const DWORD error = GetLastError();
	CloseHandle(ov.hEvent);
	if (bytes != nullptr) {
		*bytes = returned;
	}
	SetLastError(error);
	return ok != FALSE;
}

enum class WaitResult { Ok, Stopped, Failed };

// WAIT_EVENT parks in the driver until the next media tick or stream event. It is
// issued asynchronously so a service stop can cancel it instead of waiting for
// the next tick (which does not come while no stream is active).
WaitResult WaitForEvent(HANDLE device, Ioctl::WaitEventOut& ev)
{
	OVERLAPPED ov = {};
	ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (ov.hEvent == nullptr) {
		return WaitResult::Failed;
	}

	DWORD bytes = 0;
	BOOL ok = DeviceIoControl(device, Ioctl::kIoctlWaitEvent, nullptr, 0, &ev, sizeof(ev), &bytes, &ov);
	if (!ok) {
		if (GetLastError() != ERROR_IO_PENDING) {
			CloseHandle(ov.hEvent);
			return WaitResult::Failed;
		}
		HANDLE waits[2] = {g_stopEvent, ov.hEvent};
		const DWORD which = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
		if (which == WAIT_OBJECT_0) {
			CancelIoEx(device, &ov);
			// Bounded: the caller closes the handle next, which cancels whatever is left.
			WaitForSingleObject(ov.hEvent, Ioctl::kCancelGraceMs);
			CloseHandle(ov.hEvent);
			return WaitResult::Stopped;
		}
		if (which != WAIT_OBJECT_0 + 1 || !GetOverlappedResult(device, &ov, &bytes, FALSE)) {
			CloseHandle(ov.hEvent);
			return WaitResult::Failed;
		}
	}
	CloseHandle(ov.hEvent);
	return WaitResult::Ok;
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

// Registers one encoder provider with the device. Each provider supplies one codec, so the
// driver can offer the codecs whose encoders are actually present.
bool RegisterEncoder(HANDLE h, uint32_t codec, const char* name)
{
	Ioctl::RegisterEncoderIn in = {};
	in.abiVersion = Ioctl::kAbiVersion;
	in.codec = codec;
	in.maxFramePayload = Ioctl::kMaxFramePayload;

	Ioctl::RegisterEncoderOut out = {};
	DWORD bytes = 0;
	if (!IoctlSync(h, Ioctl::kIoctlRegisterEncoder, &in, sizeof(in), &out, sizeof(out), &bytes)) {
		Log("[svc] REGISTER_ENCODER(%s) failed: %lu", name, GetLastError());
		return false;
	}
	if (out.abiVersion != Ioctl::kAbiVersion) {
		Log("[svc] %s ABI mismatch: %u vs %u", name, out.abiVersion, Ioctl::kAbiVersion);
		return false;
	}
	Log("[svc] registered %s (max frame %u, %u Hz, %u ch)", name, out.maxFramePayload,
		out.pcmSampleRate, out.pcmChannels);
	return true;
}

// The worker wakes every 10 ms and has to encode within real time. Ordinary-priority CPU load
// (a build, a scan, a game) can hold it off for hundreds of milliseconds, and the driver then
// reports a stream that lost its frames as an underrun. MMCSS reserves a pro-audio slot for this
// thread alone; the rest of the process keeps its normal priority. Registration is best effort:
// without it the thread still runs at the highest priority, which is what keeps it ahead of a
// loaded machine, and the log says which of the two is in force.
class ProAudioPriority
{
public:
	explicit ProAudioPriority(const char* which)
	{
		DWORD index = 0;
		task_ = AvSetMmThreadCharacteristicsA("Pro Audio", &index);
		if (task_ != nullptr) {
			Log("[svc] %s worker registered with MMCSS (Pro Audio)", which);
			return;
		}
		const DWORD error = GetLastError();
		const BOOL raised = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
		Log("[svc] MMCSS registration failed: %lu; %s worker at %s", error, which,
			raised ? "highest priority" : "normal priority");
	}

	~ProAudioPriority()
	{
		if (task_ != nullptr) {
			AvRevertMmThreadCharacteristics(task_);
		}
	}

	ProAudioPriority(const ProAudioPriority&) = delete;
	ProAudioPriority& operator=(const ProAudioPriority&) = delete;

private:
	HANDLE task_ = nullptr;
};

// One device's event loop. Returns when the device is gone or the service stops.
void DeviceLoop(const std::wstring& linkName)
{
	const ProAudioPriority priority("encoder");
	std::string linkUtf8 = WstrToUtf8(linkName);
	Log("[svc] opening %s", linkUtf8.c_str());

	std::wstring path = L"\\\\.\\" + linkName;
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
	                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
	                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		Log("[svc] CreateFile failed: %lu", GetLastError());
		return;
	}

	// AAC is the codec Windows falls back to, so failing to register it means the stream would
	// have nothing to encode with; the aptX codecs are optional and only remove themselves from
	// negotiation.
	if (!RegisterEncoder(h, Ioctl::kCodecAac, "AAC")) {
		CloseHandle(h);
		return;
	}
	(void)RegisterEncoder(h, Ioctl::kCodecAptx, "aptX");
	(void)RegisterEncoder(h, Ioctl::kCodecAptxHd, "aptX HD");
	(void)RegisterEncoder(h, Ioctl::kCodecLdac, "LDAC");

	// Map rings.
	Ioctl::SharedRings rings = {};
	DWORD bytes = 0;
	if (!IoctlSync(h, Ioctl::kIoctlMapRings, nullptr, 0, &rings, sizeof(rings), &bytes)) {
		Log("[svc] MAP_RINGS failed: %lu", GetLastError());
		CloseHandle(h);
		return;
	}
	if (rings.magic != Ioctl::kRingsMagic || rings.abiVersion != Ioctl::kAbiVersion) {
		Log("[svc] bad rings magic/abi: 0x%08X / %u", rings.magic, rings.abiVersion);
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

	Log("[svc] rings mapped: PCM %u B, frames %u B", rings.pcmCapacity, rings.frameCapacity);

	AacStreamEncoder streamEncoder;
	AptxHdStreamEncoder hdStreamEncoder;
	AptxStreamEncoder aptxStreamEncoder;
	LdacStreamEncoder ldacStreamEncoder;

	// Main event loop.
	for (;;) {
		Ioctl::WaitEventOut ev = {};
		const WaitResult result = WaitForEvent(h, ev);
		if (result == WaitResult::Stopped) {
			break;
		}
		if (result == WaitResult::Failed) {
			Log("[svc] WAIT_EVENT failed: %lu", GetLastError());
			break;
		}

		uint64_t nowUs = ClockUs();

		if (ev.events & Ioctl::kEventDeviceGone) {
			Log("[svc] device gone (events=0x%08X)", ev.events);
			break;
		}
		// Ticks arrive every 10 ms; logging them would grow the log without bound,
		// so only lifecycle changes are printed.
		if ((ev.events & ~Ioctl::kEventTick) != 0) {
			Log("[svc] event: 0x%08X state=%u codec=%u", ev.events, (unsigned)ev.state, ev.codec);
		}

		if (ev.events & Ioctl::kEventStreamStart) {
			// One stream runs one codec; whatever the previous start left running is stale.
			streamEncoder.Stop();
			hdStreamEncoder.Stop();
			aptxStreamEncoder.Stop();
			ldacStreamEncoder.Stop();

			if (ev.codec == Ioctl::kCodecLdac) {
				Log("[svc] LDAC stream start: %u Hz, %u ch, %u-bit", ev.sampleRate, ev.channels,
					ev.pcmValidBits);
				// The encoder is fed 16-bit stereo and narrows a 24-in-32 ring itself.
				if (ev.channels != 2 ||
					(ev.pcmValidBits != Ldac::kBitsPerSample && ev.pcmValidBits != 24)) {
					Log("[svc] LDAC needs 16- or 24-bit stereo, got %u ch %u-bit", ev.channels,
						ev.pcmValidBits);
					continue;
				}

				uint8_t ie[Ldac::kIeLength] = {};
				Ldac::WriteIe(ev.sampleRate == 44100 ? Ldac::kSampleRate44100 : Ldac::kSampleRate48000,
					Ldac::kChannelStereo, ie);
				A2dpCodecConfig config = {};
				config.codec = CodecType::Vendor;
				std::memcpy(config.ie, ie, sizeof(ie));
				config.ieLength = static_cast<uint8_t>(sizeof(ie));

				// The quality mode is a local setting, so the encoder is built with whatever the
				// driver reported in the start event rather than negotiated again here.
				auto codec = CreateVendorCodec(Ldac::kVendorId, Ldac::kCodecId);
				if (!codec) {
					Log("[svc] LDAC codec is not available");
					continue;
				}
				std::unique_ptr<IA2dpEncoder> encoder;
				if (FAILED(codec->CreateEncoder(config, encoder))) {
					Log("[svc] LDAC CreateEncoder failed");
					continue;
				}

				LdacStreamParams params;
				params.sampleRate = ev.sampleRate;
				params.channels = static_cast<uint16_t>(ev.channels);
				params.bitsPerSample = static_cast<uint16_t>(ev.pcmValidBits);
				params.quality = static_cast<uint8_t>(ev.ldacQuality);
				params.prerollMs = ev.prerollMs;
				if (!ldacStreamEncoder.Start(std::move(encoder), params, pcmRing, nowUs)) {
					Log("[svc] LdacStreamEncoder::Start failed");
				}
				continue;
			}

			if (ev.codec == Ioctl::kCodecAptxHd) {
				Log("[svc] aptX HD stream start: %u Hz, %u ch, %u-bit", ev.sampleRate, ev.channels,
					ev.pcmValidBits);
				// The HD encoder reads 24-in-32 words; a narrower pin would hand it pairs of
				// samples as one word. Refuse instead of encoding noise.
				if (ev.channels != 2 || ev.pcmValidBits != AptxHd::kBitsPerSample) {
					Log("[svc] aptX HD needs 24-bit stereo, got %u ch %u-bit", ev.channels,
						ev.pcmValidBits);
					continue;
				}

				uint8_t ie[AptxHd::kIeLength] = {};
				AptxHd::WriteIe(ev.sampleRate == 44100 ? AptxHd::kSampleRate44100 : AptxHd::kSampleRate48000, ie);
				A2dpCodecConfig config = {};
				config.codec = CodecType::Vendor;
				std::memcpy(config.ie, ie, sizeof(ie));
				config.ieLength = static_cast<uint8_t>(sizeof(ie));

				auto codec = CreateVendorCodec(AptxHd::kVendorId, AptxHd::kCodecId);
				if (!codec) {
					Log("[svc] aptX HD codec is not available");
					continue;
				}
				std::unique_ptr<IA2dpEncoder> encoder;
				const HRESULT hr = codec->CreateEncoder(config, encoder);
				if (FAILED(hr)) {
					Log("[svc] aptX HD CreateEncoder failed: 0x%08X", (unsigned)hr);
					continue;
				}

				AptxHdStreamParams params;
				params.sampleRate = ev.sampleRate;
				params.channels = static_cast<uint16_t>(ev.channels);
				params.prerollMs = ev.prerollMs;
				if (!hdStreamEncoder.Start(std::move(encoder), params, pcmRing, nowUs)) {
					Log("[svc] AptxHdStreamEncoder::Start failed");
				}
				continue;
			}

			if (ev.codec == Ioctl::kCodecAptx) {
				Log("[svc] aptX stream start: %u Hz, %u ch, %u-bit", ev.sampleRate, ev.channels,
					ev.pcmValidBits);
				// The endpoint runs 24-in-32 while aptX HD can, even when classic is selected, so
				// the stream narrows it instead of failing: the sink then hears classic aptX.
				if (ev.channels != 2 ||
					(ev.pcmValidBits != Aptx::kBitsPerSample && ev.pcmValidBits != 24)) {
					Log("[svc] aptX needs 16- or 24-bit stereo, got %u ch %u-bit", ev.channels, ev.pcmValidBits);
					continue;
				}

				uint8_t ie[Aptx::kIeLength] = {};
				Aptx::WriteIe(ev.sampleRate == 44100 ? Aptx::kSampleRate44100 : Aptx::kSampleRate48000, ie);
				A2dpCodecConfig config = {};
				config.codec = CodecType::Vendor;
				std::memcpy(config.ie, ie, sizeof(ie));
				config.ieLength = static_cast<uint8_t>(sizeof(ie));

				auto codec = CreateVendorCodec(Aptx::kVendorId, Aptx::kCodecId);
				if (!codec) {
					Log("[svc] aptX codec is not available");
					continue;
				}
				std::unique_ptr<IA2dpEncoder> encoder;
				const HRESULT hr = codec->CreateEncoder(config, encoder);
				if (FAILED(hr)) {
					Log("[svc] aptX CreateEncoder failed: 0x%08X", (unsigned)hr);
					continue;
				}

				AptxStreamParams params;
				params.sampleRate = ev.sampleRate;
				params.channels = static_cast<uint16_t>(ev.channels);
				params.bitsPerSample = static_cast<uint16_t>(ev.pcmValidBits);
				params.prerollMs = ev.prerollMs;
				if (!aptxStreamEncoder.Start(std::move(encoder), params, pcmRing, nowUs)) {
					Log("[svc] AptxStreamEncoder::Start failed");
				}
				continue;
			}

			Log("[svc] stream start: %u Hz, %u ch, %u bps, obj 0x%02X",
				ev.sampleRate, ev.channels, ev.bitrate, ev.objectType);

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
				Log("[svc] CreateEncoder failed: 0x%08X", (unsigned)hr);
				continue;
			}

			AacStreamParams params;
			params.sampleRate = ev.sampleRate;
			params.channels = static_cast<uint16_t>(ev.channels);
			params.bitrate = ev.bitrate;
			params.objectType = ev.objectType;
			params.bitsPerSample = static_cast<uint16_t>(ev.pcmValidBits);
			params.prerollMs = ev.prerollMs;

			if (!streamEncoder.Start(std::move(encoder), params, pcmRing, nowUs)) {
				Log("[svc] AacStreamEncoder::Start failed");
				continue;
			}
		}

		if (ev.events & Ioctl::kEventTick) {
			if (streamEncoder.Active()) {
				if (!streamEncoder.Pump(nowUs, pcmRing, frameRing, 8)) {
					Log("[svc] AAC pump failed, stopping");
					streamEncoder.Stop();
				}
			}
			if (hdStreamEncoder.Active()) {
				if (!hdStreamEncoder.Pump(nowUs, pcmRing, frameRing, 8)) {
					Log("[svc] aptX HD pump failed, stopping");
					hdStreamEncoder.Stop();
				}
			}
			if (aptxStreamEncoder.Active()) {
				if (!aptxStreamEncoder.Pump(nowUs, pcmRing, frameRing, 8)) {
					Log("[svc] aptX pump failed, stopping");
					aptxStreamEncoder.Stop();
				}
			}
			if (ldacStreamEncoder.Active()) {
				if (!ldacStreamEncoder.Pump(nowUs, pcmRing, frameRing, 8)) {
					Log("[svc] LDAC pump failed, stopping");
					ldacStreamEncoder.Stop();
				}
			}
		}

		if (ev.events & Ioctl::kEventStreamStop) {
			Log("[svc] stream stop");
			streamEncoder.Stop();
			hdStreamEncoder.Stop();
			aptxStreamEncoder.Stop();
			ldacStreamEncoder.Stop();
		}
	}

	streamEncoder.Stop();
	hdStreamEncoder.Stop();
	aptxStreamEncoder.Stop();
	ldacStreamEncoder.Stop();
	CloseHandle(h);
	Log("[svc] %s done", linkUtf8.c_str());
}

// Device discovery and per-device threads. Returns when g_stopEvent is set.
// Enumeration runs at start, on every control device arrival (the driver enables
// kControlArrivalInterface once the link exists) and when a worker finishes.
DWORD CALLBACK OnControlArrival(HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA, DWORD)
{
	if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL) {
		SetEvent(static_cast<HANDLE>(context));
	}
	return ERROR_SUCCESS;
}

void RunLoop()
{
	Log("OpenA2dpSvc starting");

	// Auto-reset: a wake that arrives during an enumeration stays set and causes
	// another one, so no arrival is lost.
	HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (wake == nullptr) {
		Log("[svc] CreateEvent failed: %lu", GetLastError());
		return;
	}
	CM_NOTIFY_FILTER filter = {};
	filter.cbSize = sizeof(filter);
	filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
	filter.u.DeviceInterface.ClassGuid = Ioctl::kControlArrivalInterface;
	HCMNOTIFICATION notification = nullptr;
	const CONFIGRET cr = CM_Register_Notification(&filter, wake, OnControlArrival, &notification);
	if (cr != CR_SUCCESS) {
		// Without it only devices present now (or a worker exit) are picked up.
		Log("[svc] CM_Register_Notification failed: 0x%08X; new devices will not be detected", cr);
		notification = nullptr;
	}

	// One worker per control device. The enumeration loop must never block on a
	// worker: a sink stays connected for hours, and blocking here meant a second
	// sink that connected meanwhile was never registered, so it always fell back
	// to SBC even with the service running.
	struct Worker
	{
		std::wstring Link;
		std::shared_ptr<std::atomic<bool>> Done;
		std::thread Thread;
	};
	std::vector<Worker> workers;

	// Log the empty state once instead of once per wake.
	bool loggedEmpty = false;

	for (;;) {
		// Reap finished workers so a device that comes back gets a fresh thread.
		for (auto it = workers.begin(); it != workers.end();) {
			if (it->Done->load()) {
				if (it->Thread.joinable()) {
					it->Thread.join();
				}
				it = workers.erase(it);
			} else {
				++it;
			}
		}

		const auto devices = EnumerateControlDevices();
		for (const auto& dev : devices) {
			bool known = false;
			for (const auto& worker : workers) {
				if (worker.Link == dev) {
					known = true;
					break;
				}
			}
			if (known) {
				continue;
			}

			Worker worker;
			worker.Link = dev;
			worker.Done = std::make_shared<std::atomic<bool>>(false);
			auto done = worker.Done;
			worker.Thread = std::thread([dev, done, wake]() {
				DeviceLoop(dev);
				done->store(true);
				SetEvent(wake); // reap it now rather than at the next arrival
			});
			workers.push_back(std::move(worker));
		}

		if (devices.empty() && workers.empty()) {
			if (!loggedEmpty) {
				Log("[svc] no control devices found, waiting...");
				loggedEmpty = true;
			}
		} else {
			loggedEmpty = false;
		}

		const HANDLE waits[2] = {g_stopEvent, wake};
		if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
			break;
		}
	}

	// No arrival callback runs after this returns.
	if (notification != nullptr) {
		CM_Unregister_Notification(notification);
	}

	// A stop cancels the parked WAIT_EVENT, so the workers return promptly. A worker
	// stuck elsewhere (the driver or the encoder not answering) must not keep the
	// service from stopping: wait on each thread a bounded time, then detach what is left
	// and let the process exit take it down.
	const ULONGLONG joinDeadline = GetTickCount64() + kWorkerStopWaitMs;
	bool abandoned = false;
	for (auto& worker : workers) {
		if (!worker.Thread.joinable()) {
			continue;
		}
		const ULONGLONG now = GetTickCount64();
		const DWORD remaining = now < joinDeadline ? static_cast<DWORD>(joinDeadline - now) : 0;
		if (WaitForSingleObject(worker.Thread.native_handle(), remaining) == WAIT_OBJECT_0) {
			worker.Thread.join();
		} else {
			Log("[svc] %s did not stop within %lu ms; abandoning it", WstrToUtf8(worker.Link).c_str(),
				static_cast<unsigned long>(kWorkerStopWaitMs));
			worker.Thread.detach();
			abandoned = true;
		}
	}
	// An abandoned worker still signals wake when it ends; the process exit closes it then.
	if (!abandoned) {
		CloseHandle(wake);
	}

	Log("OpenA2dpSvc stopping");
}

BOOL WINAPI ConsoleCtrlHandler(DWORD type)
{
	switch (type) {
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
	case CTRL_CLOSE_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		SetEvent(g_stopEvent);
		return TRUE;
	default:
		return FALSE;
	}
}

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status = {};

void WINAPI ServiceCtrlHandler(DWORD control)
{
	switch (control) {
	case SERVICE_CONTROL_STOP:
	case SERVICE_CONTROL_SHUTDOWN:
		g_status.dwCurrentState = SERVICE_STOP_PENDING;
		g_status.dwWaitHint = 5000;
		SetServiceStatus(g_statusHandle, &g_status);
		SetEvent(g_stopEvent);
		break;
	case SERVICE_CONTROL_INTERROGATE:
		SetServiceStatus(g_statusHandle, &g_status);
		break;
	default:
		break;
	}
}

void WINAPI ServiceMain(DWORD, wchar_t**)
{
	g_consoleMode = false;
	OpenLog();

	g_statusHandle = RegisterServiceCtrlHandlerW(kServiceName, ServiceCtrlHandler);
	if (g_statusHandle == nullptr) {
		return;
	}
	g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
	g_status.dwCurrentState = SERVICE_RUNNING;
	g_status.dwWin32ExitCode = NO_ERROR;
	SetServiceStatus(g_statusHandle, &g_status);

	g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	RunLoop();

	g_status.dwControlsAccepted = 0;
	g_status.dwCurrentState = SERVICE_STOPPED;
	SetServiceStatus(g_statusHandle, &g_status);
}

} // namespace

// Two modes: run as a Windows service (default, started by the SCM at boot), or
// as a console program (--console) for interactive debugging.
int wmain(int argc, wchar_t** argv)
{
	bool console = false;
	for (int i = 1; i < argc; ++i) {
		if (wcscmp(argv[i], L"--console") == 0 || wcscmp(argv[i], L"-c") == 0) {
			console = true;
		}
	}

	if (!console) {
		SERVICE_TABLE_ENTRYW table[] = {
			{const_cast<wchar_t*>(kServiceName), ServiceMain},
			{nullptr, nullptr},
		};
		if (StartServiceCtrlDispatcherW(table)) {
			return 0;
		}
		if (GetLastError() != ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
			return 1;
		}
		fprintf(stderr, "Not started by the service control manager; running in console mode.\n");
	}

	g_consoleMode = true;
	SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
	g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	RunLoop();
	return 0;
}
