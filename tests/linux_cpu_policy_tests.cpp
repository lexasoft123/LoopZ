#include "cpu_policy.hpp"
#include <array>
#include <cassert>
#include <sched.h>
#include <stdexcept>

int main() {
    using loopz::platform::CpuCapacity;
    using loopz::platform::performanceCpus;
    // Selection is constrained to the inherited mask, not hardcoded core IDs.
    const std::array mixed{CpuCapacity{2, 397}, CpuCapacity{5, 1024}, CpuCapacity{7, 1024}};
    assert((performanceCpus(mixed) == std::vector<unsigned>{5, 7}));
    const std::array homogeneous{CpuCapacity{1, 397}, CpuCapacity{3, 397}};
    assert((performanceCpus(homogeneous) == std::vector<unsigned>{1, 3}));
    const std::array unknown{CpuCapacity{1, 0}, CpuCapacity{5, 1024}};
    assert((performanceCpus(unknown) == std::vector<unsigned>{1, 5}));
    assert(performanceCpus({}).empty());
    cpu_set_t before, after;
    assert(sched_getaffinity(0, sizeof(before), &before) == 0);
    const auto summary = loopz::platform::configureEngineCpuPolicy("inherit");
    assert(summary.find("unchanged") != std::string::npos);
    bool rejected = false;
    try {
        loopz::platform::configureEngineCpuPolicy("invalid");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
    assert(sched_getaffinity(0, sizeof(after), &after) == 0 && CPU_EQUAL(&before, &after));
}
