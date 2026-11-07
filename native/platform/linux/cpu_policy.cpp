#include "cpu_policy.hpp"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fstream>
#include <sched.h>
#include <stdexcept>

namespace loopz::platform {
std::vector<unsigned> performanceCpus(std::span<const CpuCapacity> allowed) {
    std::vector<unsigned> selected;
    uint64_t maximum = 0;
    bool complete = true;
    for (const auto& entry : allowed) {
        maximum = std::max(maximum, entry.capacity);
        complete &= entry.capacity != 0;
    }
    for (const auto& entry : allowed)
        if (!complete || entry.capacity == maximum)
            selected.push_back(entry.cpu);
    return selected;
}
namespace {
std::string cpuList(std::span<const unsigned> cpus) {
    std::string result = "[";
    for (const auto cpu : cpus) {
        if (result.size() > 1)
            result += ',';
        result += std::to_string(cpu);
    }
    return result + ']';
}
uint64_t capacity(unsigned cpu) {
    std::ifstream input("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpu_capacity");
    std::string text;
    // Kernel capacity records are short decimal tokens. Reject malformed,
    // missing or unexpectedly large records instead of guessing from MHz.
    if (!(input >> text) || text.size() > 20)
        return 0;
    uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? value : 0;
}
std::vector<unsigned> cpus(const cpu_set_t& mask) {
    std::vector<unsigned> result;
    for (unsigned cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, &mask))
            result.push_back(cpu);
    return result;
}
} // namespace
std::string configureEngineCpuPolicy(std::string_view policy) {
    if (policy != "performance" && policy != "inherit")
        throw std::invalid_argument("LOOPZ_AUDIO_CPU_POLICY must be performance or inherit");
    cpu_set_t inherited;
    if (sched_getaffinity(0, sizeof(inherited), &inherited) != 0)
        return "CPU policy: inherit; affinity unavailable: " + std::string(std::strerror(errno));
    const auto before = cpus(inherited);
    const auto prefix = "CPU policy: " + std::string(policy) + "; inherited=" + cpuList(before);
    if (policy == "inherit")
        return prefix + "; unchanged";
    std::vector<CpuCapacity> allowed;
    for (const auto cpu : before)
        allowed.push_back({cpu, capacity(cpu)});
    const auto selected = performanceCpus(allowed);
    if (selected == before || selected.empty())
        return prefix + "; unchanged (uniform or unavailable capacity)";
    cpu_set_t preferred;
    CPU_ZERO(&preferred);
    for (const auto cpu : selected)
        CPU_SET(cpu, &preferred);
    if (sched_setaffinity(0, sizeof(preferred), &preferred) != 0)
        return prefix + "; unchanged; selection failed: " + std::string(std::strerror(errno));
    cpu_set_t effective;
    if (sched_getaffinity(0, sizeof(effective), &effective) != 0)
        return prefix +
               "; selection applied; readback unavailable: " + std::string(std::strerror(errno));
    return prefix + "; effective=" + cpuList(cpus(effective)) + "; highest reported capacity";
}
} // namespace loopz::platform
