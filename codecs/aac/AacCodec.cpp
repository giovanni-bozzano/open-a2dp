#include <OpenA2dp/AacCodec.h>

#include "LatmWriter.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace OpenA2dp {

namespace Aac {

int SamplingFrequencyIndex(uint32_t sampleRate) noexcept
{
	constexpr uint32_t rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
	for (int i = 0; i < static_cast<int>(sizeof(rates) / sizeof(rates[0])); ++i) {
		if (rates[i] == sampleRate) {
			return i;
		}
	}
	return -1;
}

} // namespace Aac

using namespace Aac;
using AacConfig::ClampBitrate;
using AacConfig::EncoderSupportsRate;

size_t AacCodec::BuildLocalCapabilities(uint8_t* ie, size_t capacity) const noexcept
{
	if (capacity < kIeLength) {
		return 0;
	}
	std::memcpy(ie, AacConfig::kLocalCapabilities, kIeLength);
	return kIeLength;
}

bool AacCodec::SelectConfiguration(const uint8_t* remoteIe, size_t remoteIeLength,
								   const CodecPreference& preference, A2dpCodecConfig& out) const noexcept
{
	uint8_t ie[kIeLength] = {};
	if (!AacConfig::Select(remoteIe, remoteIeLength, preference.preferredSampleRate, preference.aacBitrate, ie,
						  0, preference.mediaPayloadBytes)) {
		return false;
	}
	out = {};
	out.codec = CodecType::Aac;
	std::memcpy(out.ie, ie, kIeLength);
	out.ieLength = kIeLength;
	return true;
}

uint8_t AacCodec::ValidateConfiguration(const uint8_t* ie, size_t ieLength) const noexcept
{
	return AacConfig::Validate(ie, ieLength);
}

HRESULT AacCodec::CreateEncoder(const A2dpCodecConfig& config, std::unique_ptr<IA2dpEncoder>& encoder) const
{
	Params p;
	if (config.codec != CodecType::Aac || !ParseConfiguredIe(config.ie, config.ieLength, p) ||
		!EncoderSupportsRate(p.sampleRate)) {
		return E_INVALIDARG;
	}
	const uint32_t bitrate = ClampBitrate(p.bitrate == 0 ? kSupportedBitrates[std::size(kSupportedBitrates) - 1] : p.bitrate, 0);
	p.bitrate = bitrate == 0 ? kSupportedBitrates[0] : bitrate;

	auto aac = std::make_unique<AacEncoder>(p);
	const HRESULT hr = aac->Initialize();
	if (FAILED(hr)) {
		return hr;
	}
	encoder = std::move(aac);
	return S_OK;
}

struct AacEncoder::Impl {
	ComPtr<IMFTransform> transform;
	bool mfStarted = false;
	bool providesSamples = false;
	DWORD outputBufferSize = 0;
	LONGLONG nextTime = 0;
	std::deque<std::vector<uint8_t>> pending;

	~Impl()
	{
		transform.Reset();
		if (mfStarted) {
			MFShutdown();
		}
	}

	HRESULT DrainOutput() noexcept
	{
		for (;;) {
			MFT_OUTPUT_DATA_BUFFER output = {};
			ComPtr<IMFSample> sample;
			if (!providesSamples) {
				ComPtr<IMFMediaBuffer> buffer;
				HRESULT hr = MFCreateMemoryBuffer(outputBufferSize, &buffer);
				if (FAILED(hr)) {
					return hr;
				}
				hr = MFCreateSample(&sample);
				if (FAILED(hr)) {
					return hr;
				}
				sample->AddBuffer(buffer.Get());
				output.pSample = sample.Get();
			}

			DWORD status = 0;
			HRESULT hr = transform->ProcessOutput(0, 1, &output, &status);
			if (output.pEvents) {
				output.pEvents->Release();
			}
			if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
				return S_OK;
			}
			if (FAILED(hr)) {
				return hr;
			}
			if (providesSamples) {
				sample.Attach(output.pSample);
			}

			ComPtr<IMFMediaBuffer> contiguous;
			hr = sample->ConvertToContiguousBuffer(&contiguous);
			if (FAILED(hr)) {
				return hr;
			}
			BYTE* data = nullptr;
			DWORD length = 0;
			hr = contiguous->Lock(&data, nullptr, &length);
			if (FAILED(hr)) {
				return hr;
			}
			if (length > 0) {
				pending.emplace_back(data, data + length);
			}
			contiguous->Unlock();
		}
	}
};

AacEncoder::AacEncoder(const Aac::Params& params) noexcept : params_(params) {}

AacEncoder::~AacEncoder() = default;

HRESULT AacEncoder::Initialize() noexcept
{
	try {
		impl_ = std::make_unique<Impl>();
	} catch (...) {
		return E_OUTOFMEMORY;
	}

	HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
	if (FAILED(hr)) {
		return hr;
	}
	impl_->mfStarted = true;

	hr = CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&impl_->transform));
	if (FAILED(hr)) {
		return hr;
	}

	ComPtr<IMFMediaType> inputType;
	hr = MFCreateMediaType(&inputType);
	if (FAILED(hr)) {
		return hr;
	}
	inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
	inputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
	inputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
	inputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, params_.sampleRate);
	inputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, params_.channels);
	inputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2u * params_.channels);
	inputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 2u * params_.channels * params_.sampleRate);

	ComPtr<IMFMediaType> outputType;
	hr = MFCreateMediaType(&outputType);
	if (FAILED(hr)) {
		return hr;
	}
	outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
	outputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
	outputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
	outputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, params_.sampleRate);
	outputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, params_.channels);
	outputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, params_.bitrate / 8);
	outputType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0); // raw AAC access units
	outputType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);

	hr = impl_->transform->SetOutputType(0, outputType.Get(), 0);
	if (FAILED(hr)) {
		return hr;
	}
	hr = impl_->transform->SetInputType(0, inputType.Get(), 0);
	if (FAILED(hr)) {
		return hr;
	}

	MFT_OUTPUT_STREAM_INFO info = {};
	hr = impl_->transform->GetOutputStreamInfo(0, &info);
	if (FAILED(hr)) {
		return hr;
	}
	impl_->providesSamples = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
	impl_->outputBufferSize = std::max<DWORD>(info.cbSize, 8192);

	impl_->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
	impl_->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
	return S_OK;
}

PcmFormat AacEncoder::InputFormat() const noexcept
{
	return PcmFormat{params_.sampleRate, params_.channels};
}

uint32_t AacEncoder::SamplesPerFrame() const noexcept
{
	return kSamplesPerFrame;
}

size_t AacEncoder::MaxFrameBytes() const noexcept
{
	// ISO 14496-3: at most 6144 bits per channel per access unit.
	return 768u * params_.channels;
}

HRESULT AacEncoder::Encode(const uint8_t* pcm, uint8_t* out, size_t capacity, size_t& written) noexcept
{
	written = 0;
	if (!impl_ || !impl_->transform || pcm == nullptr || out == nullptr) {
		return E_INVALIDARG;
	}

	const DWORD bytes = kSamplesPerFrame * params_.channels * sizeof(int16_t);
	ComPtr<IMFMediaBuffer> buffer;
	HRESULT hr = MFCreateMemoryBuffer(bytes, &buffer);
	if (FAILED(hr)) {
		return hr;
	}
	BYTE* data = nullptr;
	hr = buffer->Lock(&data, nullptr, nullptr);
	if (FAILED(hr)) {
		return hr;
	}
	memcpy(data, pcm, bytes);
	buffer->Unlock();
	buffer->SetCurrentLength(bytes);

	ComPtr<IMFSample> sample;
	hr = MFCreateSample(&sample);
	if (FAILED(hr)) {
		return hr;
	}
	sample->AddBuffer(buffer.Get());
	const LONGLONG duration = static_cast<LONGLONG>(kSamplesPerFrame) * 10'000'000 / params_.sampleRate;
	sample->SetSampleTime(impl_->nextTime);
	sample->SetSampleDuration(duration);
	impl_->nextTime += duration;

	hr = impl_->transform->ProcessInput(0, sample.Get(), 0);
	if (hr == MF_E_NOTACCEPTING) {
		hr = impl_->DrainOutput();
		if (SUCCEEDED(hr)) {
			hr = impl_->transform->ProcessInput(0, sample.Get(), 0);
		}
	}
	if (FAILED(hr)) {
		return hr;
	}
	hr = impl_->DrainOutput();
	if (FAILED(hr)) {
		return hr;
	}

	if (impl_->pending.empty()) {
		return S_OK; // encoder priming
	}
	auto& front = impl_->pending.front();
	if (front.size() > capacity) {
		return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
	}
	memcpy(out, front.data(), front.size());
	written = front.size();
	impl_->pending.pop_front();
	return S_OK;
}

size_t AacEncoder::FramesPerPacket(size_t payloadCapacity) const noexcept
{
	// One access unit per packet. Average AU size is bitrate * 1024 / fs / 8 (~557 bytes at
	// 192 kbps / 44.1 kHz); BuildMediaPayload fails if a single AU exceeds the MTU
	// (fragmentation is not implemented yet).
	const size_t average = static_cast<size_t>(params_.bitrate) * kSamplesPerFrame / params_.sampleRate / 8;
	return payloadCapacity >= average + Latm::MaxOverhead(average) ? 1 : 0;
}

size_t AacEncoder::BuildMediaPayload(const uint8_t* frames, const size_t* frameSizes, size_t frameCount,
									 uint8_t* out, size_t capacity) noexcept
{
	if (frameCount != 1 || frames == nullptr || frameSizes == nullptr) {
		return 0;
	}
	Latm::StreamConfig config;
	config.audioObjectType = 2;
	config.sampleRate = params_.sampleRate;
	config.channels = params_.channels;
	return Latm::WriteAudioMuxElement(config, frames, frameSizes[0], out, capacity);
}

} // namespace OpenA2dp
