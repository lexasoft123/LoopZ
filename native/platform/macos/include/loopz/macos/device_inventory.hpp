#pragma once

#import <Foundation/Foundation.h>
#include <zcore/device/audio_host.h>
#include <functional>
#include <memory>
#include <string_view>

namespace loopz::macos {
struct DeviceBufferRequest {
    std::string uid;
    uint32_t expectedFrames, frames;
};
DeviceBufferRequest decodeDeviceBufferRequest(std::string_view request);
// A serial background owner. Discovery failure must not invalidate the engine
// connection or erase the last known endpoint names.
class DeviceInventory {
  public:
    enum class DiscoveryMode { Automatic, Synchronous, Background };
    using Provider = std::function<singz::AudioHostInventory()>;
    using BufferWriter = std::function<void(const std::string&, uint32_t, uint32_t)>;
    // Automatic uses background discovery for native metadata and synchronous
    // discovery for injected providers. A background provider must own its
    // captures and support a simultaneous explicit buffer-validation read.
    explicit DeviceInventory(Provider provider = {}, BufferWriter bufferWriter = {},
                             DiscoveryMode mode = DiscoveryMode::Automatic);
    NSDictionary* snapshot(uint64_t nowMilliseconds);
    void invalidate() noexcept;
    // Explicit user action only. Fresh metadata and expectedFrames prevent a
    // stale settings view from overwriting a newer shared-device selection.
    std::string applyBufferRequest(std::string_view request);
    void validateBufferFrames(const std::string& uid, uint32_t expectedFrames, uint32_t frames);
    void setBufferFrames(const std::string& uid, uint32_t expectedFrames, uint32_t frames);

  private:
    struct AsyncDiscovery;
    std::shared_ptr<AsyncDiscovery> async_;
    Provider provider_;
    BufferWriter bufferWriter_;
    NSDictionary* cached_{nil};
    uint64_t nextRefresh_{0};
};
} // namespace loopz::macos
