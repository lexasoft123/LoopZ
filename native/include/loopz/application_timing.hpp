#pragma once

#include <loopz/input_timing.hpp>
#include <functional>
#include <unordered_map>

namespace loopz {
// An externally requested mix must keep its committed origin while recording.
// Editor preview alone may optimize timing before a recording client opens.
void retainActiveApplicationTiming(const RouteDocument& desired, const AudioScene& active,
                                   std::span<DeviceInputDelay> delays);

// Control-only timing for admitted process captures. Unknown/new/partial
// owners retain conservative history; all streams of a sum must be verified.
// A backend reports actual private capture periods, never a requested setting
// on the app's physical playback device. Does not acquire capture or permission.
class ApplicationInputTiming {
  public:
    using Probe = std::function<std::vector<InputTimingFacts>(const std::string& selector)>;
    using Reject = std::function<void(const std::string& selector)>;
    ApplicationInputTiming(SceneTiming timing, uint32_t renderLeadFrames, Probe probe,
                           Reject rejectUnsupported = {});
    std::vector<DeviceInputDelay> delays(const RouteDocument& document,
                                         const RouteDocument* active = nullptr);

  private:
    const SceneTiming timing_;
    const uint32_t lead_;
    const Probe probe_;
    const Reject reject_;
    std::unordered_map<std::string, uint32_t> retained_;
};
} // namespace loopz
