#include <loopz/application_processes.hpp>

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace loopz {
namespace {
bool valid(const ProcessInstance& process) {
    return process.id > 1 && process.generation;
}
bool valid(const std::string& application, bool allowEmpty = false) {
    return (allowEmpty || !application.empty()) && application.size() <= 4096 &&
           application.find('\0') == std::string::npos;
}
} // namespace
std::vector<uint64_t> selectApplicationProcesses(const std::string& application,
                                                 std::span<const ApplicationProcessRoot> roots,
                                                 std::span<const AudioProcessAttribution> audio) {
    if (!valid(application) || roots.size() > 4096 || audio.size() > 4096)
        throw std::invalid_argument("Invalid bounded application process snapshot");
    std::unordered_map<uint64_t, const ApplicationProcessRoot*> applications;
    for (const auto& root : roots) {
        if (!valid(root.process) || !valid(root.application) ||
            !applications.emplace(root.process.id, &root).second)
            throw std::invalid_argument("Ambiguous application process instance");
    }
    std::unordered_set<uint64_t> objects;
    std::vector<uint64_t> selected;
    for (const auto& process : audio) {
        if (!process.object || !valid(process.process) || !valid(process.application, true) ||
            !valid(process.containingApplication, true) ||
            (process.responsible && !valid(*process.responsible)) ||
            !objects.insert(process.object).second)
            throw std::invalid_argument("Invalid or duplicate audio process identity");
        bool match =
            process.application == application || process.containingApplication == application;
        if (!match && !process.independentApplication && process.responsible) {
            const auto root = applications.find(process.responsible->id);
            match = root != applications.end() && root->second->process == *process.responsible &&
                    root->second->application == application;
        }
        if (match)
            selected.push_back(process.object);
    }
    std::sort(selected.begin(), selected.end());
    return selected;
}
} // namespace loopz
