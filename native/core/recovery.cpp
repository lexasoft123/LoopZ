#include <loopz/recovery.hpp>

#include <algorithm>
#include <utility>
#include <stdexcept>

namespace loopz {
namespace {
uint64_t after(uint64_t now, uint64_t delay) noexcept {
    return now > UINT64_MAX - delay ? UINT64_MAX : now + delay;
}
} // namespace

GraphRecovery::Entry& GraphRecovery::find(const std::string& id) {
    return const_cast<Entry&>(std::as_const(*this).find(id));
}
const GraphRecovery::Entry& GraphRecovery::find(const std::string& id) const {
    const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& value) {
        return value.node.id == id;
    });
    if (entry == entries_.end())
        throw std::invalid_argument("Unknown recovery node: " + id);
    return *entry;
}
void GraphRecovery::setDesired(std::vector<RecoveryNode> nodes, uint64_t now) {
    // Validate and topologically sort before changing the current intent.
    std::vector<Entry> next;
    next.reserve(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].id.empty())
            throw std::invalid_argument("Empty recovery node id");
        for (size_t j = 0; j < i; ++j)
            if (nodes[i].id == nodes[j].id)
                throw std::invalid_argument("Duplicate recovery node id");
        for (const auto& dependency : nodes[i].dependencies)
            if (std::none_of(nodes.begin(), nodes.end(), [&](const RecoveryNode& node) {
                    return node.id == dependency;
                }))
                throw std::invalid_argument("Unknown recovery dependency");
    }
    while (next.size() != nodes.size()) {
        const auto before = next.size();
        for (const auto& node : nodes) {
            if (std::any_of(next.begin(), next.end(), [&](const Entry& entry) {
                    return entry.node.id == node.id;
                }))
                continue;
            if (std::all_of(node.dependencies.begin(), node.dependencies.end(),
                            [&](const std::string& dependency) {
                                return std::any_of(next.begin(), next.end(), [&](const Entry& e) {
                                    return e.node.id == dependency;
                                });
                            })) {
                Entry entry;
                entry.node = node;
                entry.due = now;
                next.push_back(std::move(entry));
            }
        }
        if (before == next.size())
            throw std::invalid_argument("Recovery dependency cycle");
    }
    entries_ = std::move(next);
}
void GraphRecovery::failed(Entry& entry, uint64_t now) noexcept {
    // Reset escalation only after sustained health, so a flapping USB device
    // cannot create a tight open/close loop. Never exhaust the retry budget.
    if (entry.phase == Phase::Ready && now >= entry.readySince && now - entry.readySince >= 5000)
        entry.failures = 0;
    const uint64_t delay = std::min<uint64_t>(5000, 100ULL << std::min(entry.failures, 6U));
    entry.failures = std::min(entry.failures + 1, 7U);
    entry.phase = Phase::Missing;
    entry.token = 0;
    entry.due = after(now, delay);
}
std::vector<RecoveryAttempt> GraphRecovery::attempts(uint64_t now) {
    std::vector<RecoveryAttempt> result;
    for (auto& entry : entries_) {
        if (entry.phase == Phase::Connecting && now >= entry.deadline)
            failed(entry, now);
        if (entry.phase != Phase::Missing || now < entry.due)
            continue;
        if (!std::all_of(entry.node.dependencies.begin(), entry.node.dependencies.end(),
                         [&](const std::string& id) {
                             return ready(id);
                         }))
            continue;
        if (nextToken_ == UINT64_MAX)
            throw std::overflow_error("Recovery token exhausted");
        // Allocate the returned token before committing the in-flight state.
        result.push_back({entry.node.id, nextToken_ + 1});
        entry.token = ++nextToken_;
        entry.phase = Phase::Connecting;
        entry.deadline = after(now, 10000);
    }
    return result;
}
bool GraphRecovery::complete(const RecoveryAttempt& attempt, bool success, uint64_t now) {
    const auto item = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
        return entry.node.id == attempt.id && entry.phase == Phase::Connecting &&
               entry.token == attempt.token && now < entry.deadline;
    });
    if (item == entries_.end())
        return false;
    if (success) {
        item->phase = Phase::Ready;
        item->readySince = now;
        item->token = 0;
    } else
        failed(*item, now);
    return true;
}
std::vector<std::string> GraphRecovery::lost(const std::string& id, uint64_t now) {
    find(id); // Reject an unknown root before any mutation.
    std::vector<std::string> affected{id};
    for (const auto& entry : entries_) {
        if (entry.node.id == id)
            continue;
        if (std::any_of(entry.node.dependencies.begin(), entry.node.dependencies.end(),
                        [&](const std::string& dependency) {
                            return std::find(affected.begin(), affected.end(), dependency) !=
                                   affected.end();
                        }))
            affected.push_back(entry.node.id);
    }
    // Allocate the full release list before invalidating tokens.
    std::reverse(affected.begin(), affected.end());
    for (const auto& key : affected) {
        auto& entry = find(key);
        if (entry.phase != Phase::Missing)
            failed(entry, now);
    }
    return affected;
}
void GraphRecovery::retryNow(uint64_t now) noexcept {
    for (auto& entry : entries_)
        if (entry.phase == Phase::Missing)
            entry.due = now;
}
void GraphRecovery::restart(uint64_t now) noexcept {
    for (auto& entry : entries_) {
        entry.phase = Phase::Missing;
        entry.token = 0;
        entry.due = now;
        entry.deadline = 0;
        entry.readySince = 0;
        entry.failures = 0;
    }
}
void GraphRecovery::disconnected(uint64_t now) noexcept {
    for (auto& entry : entries_)
        if (entry.phase != Phase::Missing)
            failed(entry, now);
}
bool GraphRecovery::ready(const std::string& id) const {
    return find(id).phase == Phase::Ready;
}
bool GraphRecovery::restored() const noexcept {
    return std::all_of(entries_.begin(), entries_.end(), [](const Entry& entry) {
        return entry.phase == Phase::Ready;
    });
}
size_t GraphRecovery::desiredCount() const noexcept {
    return entries_.size();
}
} // namespace loopz
