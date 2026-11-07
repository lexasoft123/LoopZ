#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace loopz::platform {
struct CpuCapacity {
    unsigned cpu;
    uint64_t capacity; // Zero means unavailable; never exclude an unknown CPU.
};
// Input contains only CPUs already allowed by the caller's affinity mask.
std::vector<unsigned> performanceCpus(std::span<const CpuCapacity> allowed);
// Startup only, before Engine workers exist. Changes the calling thread's
// affinity, not host scheduling policy. Later threads inherit it; the reusable
// Runtime does not call it.
std::string configureEngineCpuPolicy(std::string_view policy);
} // namespace loopz::platform
