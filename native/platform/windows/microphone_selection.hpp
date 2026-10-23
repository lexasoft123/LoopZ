#pragma once
#include <map>
#include <string>
namespace loopz::windows {
using MicrophoneSelections = std::map<std::string, std::string>;
// Only a changed selection for an existing mix needs reselection. New/deleted
// mixes and unavailable snapshots do not prove that a saved selection changed.
inline bool microphoneSelectionsChanged(const MicrophoneSelections& before,
                                       const MicrophoneSelections& after) {
    for (const auto& [mix, endpoint] : before) {
        const auto found = after.find(mix);
        if (!endpoint.empty() && found != after.end() && !found->second.empty() &&
            endpoint != found->second)
            return true;
    }
    return false;
}
} // namespace loopz::windows
