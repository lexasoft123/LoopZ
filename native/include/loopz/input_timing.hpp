#pragma once

#include <loopz/audio_scene.hpp>
#include <loopz/audio_resampler.hpp>
#include <optional>

namespace loopz {
struct InputTimingFacts {
    double sampleRate;
    uint32_t bufferFrames;
    // Device, stream, safety and observed delivery latency in the source clock.
    uint32_t latencyFrames;
};
struct DeviceInputFilter {
    std::string device, source;
    ResamplingFilter filter;
};
// Control-only DAG analysis. Independent sources in the same mix must not
// acquire different filter phase. Propagate linear reads into physical inputs
// of upstream virtual dependencies; unrelated solo routes can remain causal.
std::vector<DeviceInputFilter> planPhysicalInputFilters(const RouteDocument& document,
                                                        ResamplingFilter preferred);

// Converts backend facts into a bounded render-clock budget. Includes a native
// delivery period, future filter support, publication lead and two render blocks
// (the rendered interval and one scheduling reserve). No IO or allocation.
// An unknown/unsupported budget must not be treated as a zero-delay input.
std::optional<uint32_t> inputTimingBudget(InputTimingFacts input, SceneTiming render,
                                          uint32_t renderLeadFrames) noexcept;
// The minimum-phase reader requires only retained past support. Every native
// delivery, publication and scheduling reserve remains in its budget.
std::optional<uint32_t> inputTimingBudget(InputTimingFacts input, SceneTiming render,
                                          uint32_t renderLeadFrames,
                                          ResamplingFilter filter) noexcept;
} // namespace loopz
