#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace loopz {
struct ProcessInstance {
    uint64_t id, generation;
    bool operator==(const ProcessInstance&) const = default;
};
struct ApplicationProcessRoot {
    ProcessInstance process;
    std::string application;
};
struct AudioProcessAttribution {
    uint64_t object;
    ProcessInstance process;
    std::string application;
    // A platform provider verifies containment; string prefixes of application
    // identifiers are never evidence that a helper belongs to an application.
    std::string containingApplication;
    std::optional<ProcessInstance> responsible;
    bool independentApplication{false};
};
// Control-only policy. Providers supply one coherent, same-user snapshot and
// stable process instances. Unknown ownership never means "capture all".
std::vector<uint64_t> selectApplicationProcesses(const std::string& application,
                                                 std::span<const ApplicationProcessRoot> roots,
                                                 std::span<const AudioProcessAttribution> audio);
} // namespace loopz
