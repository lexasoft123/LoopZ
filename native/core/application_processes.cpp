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
std::vector<ProcessInstance> selectProcessCaptureRoots(std::span<const ProcessInstance> selected,
                                                       std::span<const ProcessTreeNode> ancestry) {
    if (selected.size() > 4096 || ancestry.size() > 32768)
        throw std::invalid_argument("Oversized capture process ancestry");
    std::unordered_map<uint64_t, ProcessInstance> wanted;
    uint64_t oldest = UINT64_MAX;
    for (const auto& process : selected) {
        if (!valid(process) || !wanted.emplace(process.id, process).second)
            throw std::invalid_argument("Invalid or duplicate capture process");
        oldest = std::min(oldest, process.generation);
    }
    std::unordered_map<uint64_t, const ProcessTreeNode*> nodes;
    for (const auto& node : ancestry)
        if (!valid(node.process) || !nodes.emplace(node.process.id, &node).second)
            throw std::invalid_argument("Ambiguous capture process ancestry");
    std::vector<ProcessInstance> roots;
    for (const auto& process : selected) {
        auto current = nodes.find(process.id);
        if (current == nodes.end() || current->second->process != process)
            throw std::runtime_error("Capture process ancestry changed");
        std::unordered_set<uint64_t> visited{process.id};
        bool contained = false;
        while (current->second->parent) {
            const auto parent = nodes.find(current->second->parent);
            if (parent == nodes.end() || !visited.insert(parent->first).second ||
                parent->second->process.generation > current->second->process.generation)
                throw std::runtime_error("Incomplete or reused capture process ancestry");
            const auto match = wanted.find(parent->first);
            if (match != wanted.end()) {
                if (match->second != parent->second->process)
                    throw std::runtime_error("Capture ancestor identity changed");
                contained = true;
            }
            if (parent->second->process.generation < oldest)
                break; // No selected ancestor can have been born before this parent.
            current = parent;
        }
        if (!contained)
            roots.push_back(process);
    }
    std::sort(roots.begin(), roots.end(), [](const auto& a, const auto& b) {
        return a.id < b.id;
    });
    return roots;
}
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
