// Allocation-free control-time registry contract shared by kernel/setup tests.
#pragma once
#include "audio_format.hpp"

namespace loopz_driver {
enum class AdapterEndpointMode : unsigned { DuplexMix = 0, AppRoutingSink = 1 };
struct AdapterConfiguration {
    AudioFormat format{};
    AdapterEndpointMode mode = AdapterEndpointMode::DuplexMix;
    bool exposesMicrophone() const noexcept {
        return mode == AdapterEndpointMode::DuplexMix;
    }
};
inline constexpr unsigned routingSinkRecordVersion = 2;
inline constexpr unsigned routingSinkRecordTag = 0x53494e4b;
inline AudioFormatRecord appRoutingSinkRecord() noexcept {
    return {routingSinkRecordVersion, routingSinkRecordTag, {48000, 2}};
}
// Keep the legacy record's valid() unchanged: old setup must not mistake this
// new configuration for a normal microphone format. Unknown modes fail closed.
inline bool decodeAdapterConfiguration(const AudioFormatRecord& record,
                                       AdapterConfiguration& output) noexcept {
    if (record.valid()) {
        output = {record.format, AdapterEndpointMode::DuplexMix};
        return true;
    }
    if (record.version == routingSinkRecordVersion && record.reserved == routingSinkRecordTag &&
        record.format.sampleRate == 48000 && record.format.channels == 2) {
        output = {record.format, AdapterEndpointMode::AppRoutingSink};
        return true;
    }
    return false;
}
} // namespace loopz_driver
