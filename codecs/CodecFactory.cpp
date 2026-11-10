#include <OpenA2dp/IA2dpCodec.h>
#include <OpenA2dp/SbcCodec.h>
#include <OpenA2dp/AacCodec.h>
#include <OpenA2dp/AptxHdCodec.h>

namespace OpenA2dp {

namespace {
constexpr CodecType kPriority[] = {CodecType::Aac, CodecType::Sbc};
}

std::unique_ptr<IA2dpCodec> CreateCodec(CodecType type)
{
	switch (type) {
	case CodecType::Sbc:
		return std::make_unique<SbcCodec>();
	case CodecType::Aac:
		return std::make_unique<AacCodec>();
	default:
		return nullptr;
	}
}

std::unique_ptr<IA2dpCodec> CreateVendorCodec(uint32_t vendorId, uint16_t codecId)
{
	if (vendorId == AptxHd::kVendorId && codecId == AptxHd::kCodecId) {
		return std::make_unique<AptxHdCodec>();
	}
	return nullptr;
}

size_t GetSupportedCodecs(CodecType* types, size_t capacity) noexcept
{
	size_t count = 0;
	for (CodecType t : kPriority) {
		if (count < capacity) {
			types[count] = t;
		}
		++count;
	}
	return count;
}

bool NegotiateConfiguration(const A2dpCodecConfig* remoteCapabilities, size_t remoteCount,
							const CodecPreference& preference, A2dpCodecConfig& selected)
{
	auto trySelect = [&](CodecType type) {
		auto codec = CreateCodec(type);
		if (!codec) {
			return false;
		}
		for (size_t i = 0; i < remoteCount; ++i) {
			const auto& remote = remoteCapabilities[i];
			if (remote.codec == type &&
				codec->SelectConfiguration(remote.ie, remote.ieLength, preference, selected)) {
				return true;
			}
		}
		return false;
	};

	if (preference.hasForcedCodec && trySelect(preference.forcedCodec)) {
		return true;
	}
	for (CodecType t : kPriority) {
		if (trySelect(t)) {
			return true;
		}
	}
	return false;
}

} // namespace OpenA2dp
