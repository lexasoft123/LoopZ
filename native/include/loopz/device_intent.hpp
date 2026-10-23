#pragma once
#include <loopz/document_json.hpp>
#include <cstdint>
#include <string>
#include <string_view>

namespace loopz {
// Platform-neutral control-thread setup intent. Ownership comes from the platform
// authentication layer, never from this payload. No device operations happen here;
// persist intent before reconciling it through a platform device adapter.
inline constexpr size_t kMaximumDeviceIntentBytes = 64 * 1024;
enum class DeviceIntentAction { Upsert, Remove };
enum class DeviceIntentKind { MicrophoneMix, AppRoutingSink };
struct DeviceIntentRequest {
    std::string operationId; // Canonical lower-case UUID; reused for a retry.
    uint64_t expectedRevision{};
    DeviceIntentAction action{DeviceIntentAction::Upsert};
    std::string mixId;
    std::string name;
    uint32_t sampleRate{48000};
    uint32_t channels{2};
    DeviceIntentKind kind{DeviceIntentKind::MicrophoneMix};
};
// Internal database/cache key. Mix keys remain byte-identical to legacy IDs.
// Sink keys occupy a NUL-prefixed domain that public mix IDs cannot enter.
std::string deviceIntentStorageKey(DeviceIntentKind kind, std::string_view mixId);
DeviceIntentRequest decodeDeviceIntent(std::string_view bytes);
JSON encodeDeviceIntent(const DeviceIntentRequest& request);
} // namespace loopz
