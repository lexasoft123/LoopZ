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
struct ProcessTreeNode {
    ProcessInstance process;
    uint64_t parent{};
};
// Selected instances and ancestry are authenticated by the provider. Drop a
// selected descendant only when its chain reaches another selected instance.
// Missing, cyclic or reused ancestry fails rather than double-capturing a tree.
std::vector<ProcessInstance> selectProcessCaptureRoots(std::span<const ProcessInstance> selected,
                                                       std::span<const ProcessTreeNode> ancestry);
// Control-only policy. Providers supply one coherent, same-user snapshot and
// stable process instances. Unknown ownership never means "capture all".
std::vector<uint64_t> selectApplicationProcesses(const std::string& application,
                                                 std::span<const ApplicationProcessRoot> roots,
                                                 std::span<const AudioProcessAttribution> audio);
} // namespace loopz
