#pragma once

#include <loopz/input_timing.hpp>
#include <functional>
#include <unordered_map>

namespace loopz::macos {
// Control-owned, read-only HAL inspection. Facts belong to this Engine process;
// another process's buffer property does not establish its capture period.
class PhysicalInputTiming {
  public:
    using Probe = std::function<std::optional<InputTimingFacts>(const std::string& selector)>;
    explicit PhysicalInputTiming(SceneTiming timing, uint32_t renderLeadFrames, Probe probe = {});
    PhysicalInputTiming(SceneTiming timing, uint32_t renderLeadFrames, Probe probe,
                        ResamplingFilter filter);
    std::vector<DeviceInputDelay> delays(const RouteDocument& document,
                                         const RouteDocument* active = nullptr);

  private:
    SceneTiming timing_;
    uint32_t lead_;
    const ResamplingFilter filter_;
    Probe probe_;
    std::unordered_map<std::string, uint32_t> retained_;
};
} // namespace loopz::macos
