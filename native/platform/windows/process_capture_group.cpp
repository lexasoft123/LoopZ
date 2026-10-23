#include "process_capture_group.hpp"
#include <loopz/capture_mix.hpp>
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
namespace loopz::windows {
namespace {
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
std::vector<ProcessInstance>
identities(const std::vector<std::shared_ptr<AppRoutingProcess>>& members) {
    if (members.empty() || members.size() > 4096)
        throw std::runtime_error("Invalid application capture membership");
    std::vector<ProcessInstance> result;
    std::string application;
    for (const auto& member : members) {
        if (!member)
            throw std::runtime_error("Missing authenticated capture process");
        member->validate();
        const auto& identity = member->identity();
        if (application.empty())
            application = identity.application;
        if (application != identity.application)
            throw std::runtime_error("Application capture identities differ");
        result.push_back({identity.pid, identity.created100ns});
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.id < b.id;
    });
    if (std::adjacent_find(result.begin(), result.end(), [](const auto& a, const auto& b) {
            return a.id == b.id;
        }) != result.end())
        throw std::runtime_error("Duplicate application capture process");
    return result;
}
struct AncestryNode {
    ProcessTreeNode tree;
    HANDLE process{};
    AncestryNode(ProcessTreeNode value, HANDLE handle) : tree(value), process(handle) {}
    AncestryNode(const AncestryNode&) = delete;
    AncestryNode& operator=(const AncestryNode&) = delete;
    ~AncestryNode() {
        CloseHandle(process);
    }
};
using AncestryNodes = std::map<uint64_t, std::shared_ptr<const AncestryNode>>;
struct PreparedAncestry {
    std::vector<ProcessInstance> roots;
    AncestryNodes nodes;
};
PreparedAncestry rootsFor(const std::vector<ProcessInstance>& selected,
                          const AncestryNodes* previous) {
    Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (snapshot.value == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot examine application capture ancestry");
    std::map<uint32_t, uint32_t> parents;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.value, &entry))
        throw std::runtime_error("Cannot begin application process snapshot");
    do {
        if (parents.size() >= 32768 ||
            !parents.emplace(entry.th32ProcessID, entry.th32ParentProcessID).second)
            throw std::runtime_error("Oversized or ambiguous application process snapshot");
    } while (Process32NextW(snapshot.value, &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES)
        throw std::runtime_error("Application process snapshot is incomplete");
    PreparedAncestry result;
    const auto resolve = [&](uint64_t id,
                             uint64_t expectedBirth = 0) -> std::shared_ptr<const AncestryNode> {
        if (const auto existing = result.nodes.find(id); existing != result.nodes.end()) {
            if (expectedBirth && existing->second->tree.process.generation != expectedBirth)
                throw std::runtime_error("Capture process ancestry changed");
            return existing->second;
        }
        if (result.nodes.size() >= 32768)
            throw std::runtime_error("Oversized retained application ancestry");
        if (previous) {
            const auto cached = previous->find(id);
            if (cached != previous->end()) {
                // The retained handle pins the original process object, even
                // after exit. Its ID cannot identify a different new process.
                if (GetProcessId(cached->second->process) != id ||
                    (expectedBirth && cached->second->tree.process.generation != expectedBirth))
                    throw std::runtime_error("Retained capture ancestry changed");
                result.nodes.emplace(id, cached->second);
                return cached->second;
            }
        }
        const auto found = parents.find(static_cast<uint32_t>(id));
        if (found == parents.end())
            throw std::runtime_error("Application ancestor is unavailable");
        Handle process{
            OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(id))};
        FILETIME created{}, exited{}, kernel{}, user{};
        if (process.value == nullptr ||
            !GetProcessTimes(process.value, &created, &exited, &kernel, &user))
            throw std::runtime_error("Cannot authenticate application process ancestry");
        const auto birth =
            (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        if (!birth || (expectedBirth && birth != expectedBirth))
            throw std::runtime_error("Capture process ancestry changed");
        auto node = std::make_shared<AncestryNode>(
            ProcessTreeNode{{id, birth}, found->second == 4 ? 0 : found->second}, process.value);
        process.value = nullptr;
        result.nodes.emplace(id, node);
        return node;
    };
    for (const auto& item : selected)
        (void)resolve(item.id, item.generation);
    if (selected.size() == 1) {
        // No deduplication is needed. Retain this root for a future refresh,
        // without requiring an already-exited parent to remain discoverable.
        result.roots = selected;
        return result;
    }
    const auto oldest =
        std::min_element(selected.begin(), selected.end(), [](const auto& a, const auto& b) {
            return a.generation < b.generation;
        })->generation;
    for (const auto& item : selected) {
        auto current = resolve(item.id, item.generation);
        std::set<uint64_t> visited;
        for (;;) {
            const auto& node = current->tree;
            if (!visited.insert(node.process.id).second)
                throw std::runtime_error("Cyclic application process ancestry");
            if (!node.parent || node.process.generation < oldest)
                break;
            current = resolve(node.parent);
        }
    }
    std::vector<ProcessTreeNode> ancestry;
    for (const auto& [pid, node] : result.nodes) {
        (void)pid;
        ancestry.push_back(node->tree);
    }
    result.roots = selectProcessCaptureRoots(selected, ancestry);
    return result;
}
class GroupReader final : public AudioSource {
  public:
    explicit GroupReader(std::vector<std::shared_ptr<AudioHistory>> histories)
        : owners_(std::move(histories)) {
        for (const auto& history : owners_)
            histories_.push_back(history.get());
    }
    uint32_t channels() const noexcept override {
        return 2;
    }
    AudioReadResult read(uint64_t ns, double rate, RoutingOutput output, uint32_t frames,
                         AudioReadCursor&) const noexcept override {
        if (!frames || frames > kBlock || output.capacityFrames < frames ||
            output.channels.empty() || output.channels.size() > 2 ||
            std::any_of(output.channels.begin(), output.channels.end(), [](auto* p) {
                return !p;
            })) {
            for (auto* plane : output.channels)
                if (plane)
                    std::fill_n(plane, std::min(frames, output.capacityFrames), 0.0f);
            return {frames, false};
        }
        std::array<float, kBlock> discarded{};
        return sampleAudioHistoryMix(
            histories_, ns, rate, output.channels[0],
            output.channels.size() == 2 ? output.channels[1] : discarded.data(), frames, reader_);
    }

  private:
    std::vector<std::shared_ptr<AudioHistory>> owners_;
    std::vector<const AudioHistory*> histories_;
    mutable AudioHistoryMixReader reader_;
};
} // namespace
struct ProcessCapturePlan::Impl {
    PreparedAncestry ancestry;
};
ProcessCapturePlan::ProcessCapturePlan(
    const std::vector<std::shared_ptr<AppRoutingProcess>>& members,
    const ProcessCapturePlan* previous) {
    const auto selected = identities(members);
    auto prepared = std::make_unique<Impl>();
    prepared->ancestry = rootsFor(selected, previous ? &previous->impl_->ancestry.nodes : nullptr);
    if (identities(members) != selected)
        throw std::runtime_error("Capture process membership changed during ancestry query");
    impl_ = std::move(prepared);
}
ProcessCapturePlan::~ProcessCapturePlan() = default;
const std::vector<ProcessInstance>& ProcessCapturePlan::roots() const noexcept {
    return impl_->ancestry.roots;
}
std::vector<ProcessInstance>
processCaptureRoots(const std::vector<std::shared_ptr<AppRoutingProcess>>& members) {
    const ProcessCapturePlan plan(members);
    return plan.roots();
}
ProcessCaptureGroup::ProcessCaptureGroup(std::vector<std::shared_ptr<AppRoutingProcess>> members,
                                         const ProcessCaptureGroup* previous)
    : members_(std::move(members)), membership_(identities(members_)),
      plan_(members_, previous ? &previous->plan_ : nullptr) {
    if (plan_.roots().empty() || plan_.roots().size() > kMaximumRoutingSources)
        throw std::runtime_error("Too many independent capture roots for one application");
    for (const auto& root : plan_.roots()) {
        std::shared_ptr<ProcessCapture> capture;
        if (previous)
            for (const auto& prior : previous->roots_)
                if (prior.identity == root && prior.capture->error().empty())
                    capture = prior.capture;
        if (!capture) {
            capture = std::make_shared<ProcessCapture>();
            std::string failure;
            if (!capture->start(static_cast<uint32_t>(root.id), failure))
                throw std::runtime_error(failure);
        }
        roots_.push_back({root, std::move(capture)});
    }
    (void)identities(members_); // Pinned identities must survive every activation.
}
bool ProcessCaptureGroup::matches(
    const std::vector<std::shared_ptr<AppRoutingProcess>>& members) const {
    return identities(members) == membership_;
}
std::string ProcessCaptureGroup::error() const {
    try {
        (void)identities(members_);
    } catch (const std::exception& failure) {
        return failure.what();
    }
    for (const auto& root : roots_)
        if (const auto failure = root.capture->error(); !failure.empty())
            return failure;
    return {};
}
bool ProcessCaptureGroup::readyToRoute() const {
    if (!error().empty())
        return false;
    return std::all_of(roots_.begin(), roots_.end(), [](const auto& root) {
        const auto status = root.capture->history()->status();
        return status.online && status.callbacks != 0;
    });
}
std::shared_ptr<const AudioSource> ProcessCaptureGroup::reader() const {
    std::vector<std::shared_ptr<AudioHistory>> histories;
    for (const auto& root : roots_)
        histories.push_back(root.capture->history());
    return std::make_shared<GroupReader>(std::move(histories));
}
} // namespace loopz::windows
