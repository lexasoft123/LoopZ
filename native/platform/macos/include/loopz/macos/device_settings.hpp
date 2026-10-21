#pragma once

#include <loopz/route_document.hpp>
#include <zcore/device/audio_host.h>
#include <filesystem>
#include <functional>
#include <memory>

namespace loopz::macos {
// Engine control-owner state. Empty path keeps private/test runtimes session-only.
// No work runs on an audio callback; no capture or output stream is opened here.
class DeviceSettings {
  public:
    using Provider = std::function<singz::AudioHostInventory()>;
    using Writer = std::function<void(const std::string&, uint32_t, uint32_t)>;
    explicit DeviceSettings(std::filesystem::path path = {}, Provider provider = {},
                            Writer writer = {});
    ~DeviceSettings();
    void reload();
    void setRoutingDocument(const RouteDocument& document);
    void poll(uint64_t nowMilliseconds);
    void retryNow() noexcept;
    std::string snapshot(uint64_t nowMilliseconds);
    std::string apply(std::string_view request);
    uint64_t hardwareGeneration() const noexcept;
    bool restoredFor(const RouteDocument& document) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz::macos
