#pragma once

#include <loopz/audio_scene.hpp>
#include <CoreFoundation/CoreFoundation.h>
#include <optional>
#include <memory>
#include <unordered_map>

namespace loopz::macos {
struct PublicDeviceTiming {
    uint32_t bufferFrames;
    bool running;
    uint32_t session{0};
    bool measured{true};
};
PublicDeviceTiming decodePublicDeviceTiming(CFPropertyListRef value);
struct PublicTimingObservation {
    std::optional<PublicDeviceTiming> value;
    std::string error;
};
struct PublicTimingObservationBatch {
    uint64_t revision{0};
    std::unordered_map<std::string, PublicTimingObservation> devices;
};
class PublicTimingObserver;
// Control-only native discovery. Cache immutable validation for a resolved
// object, never its running state, session or callback period. Every probe
// resolves the UID again; disappearance, replacement and read errors invalidate
// that identity. Audio-service reset explicitly invalidates all identities.
class PublicTimingCatalog {
  public:
    struct Operations {
        std::function<uint32_t(const std::string&)> resolve;
        std::function<bool(uint32_t)> validate;
        std::function<PublicDeviceTiming(uint32_t, bool)> read;
    };
    explicit PublicTimingCatalog(Operations operations);
    std::optional<PublicDeviceTiming> probe(const std::string& uid);
    void invalidate() noexcept;

  private:
    const Operations operations_;
    struct Identity {
        uint32_t object;
        bool reportsTiming;
    };
    std::unordered_map<std::string, Identity> identities_;
};
// One control owner. Reserve observed driver playback periods independently;
// keep a larger queue within a measured running session. A new measured session
// can replace the idle/pre-publication fallback. Querying never opens IO or
// changes defaults.
class PublicPassThroughTiming {
  public:
    using Probe = std::function<std::optional<PublicDeviceTiming>(const std::string& uid)>;
    explicit PublicPassThroughTiming(SceneTiming timing, Probe probe = {},
                                     uint32_t renderLeadFrames = 64);
    std::vector<DevicePassThroughDelay> floors(const RouteDocument& document);
    void invalidateNativeIdentities() noexcept;
    std::shared_ptr<const PublicTimingObservationBatch> observations() const noexcept;

  private:
    const SceneTiming timing_;
    const std::shared_ptr<PublicTimingObserver> observer_;
    const Probe probe_;
    const uint32_t lead_;
    struct RetainedTiming {
        uint32_t frames;
        bool running;
        uint32_t session;
        uint32_t measuredPeriod;
    };
    std::unordered_map<std::string, RetainedTiming> retained_;
};
} // namespace loopz::macos
