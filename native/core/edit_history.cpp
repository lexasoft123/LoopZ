#include <loopz/edit_history.hpp>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace loopz {

struct EditHistory::State {
    std::vector<std::shared_ptr<const std::string>> entries;
    size_t cursor{0}, bytes{0};
};

EditHistory::EditHistory(EditHistoryLimits limits) : limits_(limits) {
    if (!limits.edits || limits.edits > 32 || !limits.payloadBytes ||
        limits.payloadBytes > 8 * 1024 * 1024)
        throw std::invalid_argument("Invalid edit history bounds");
}
bool EditHistory::empty() const noexcept {
    return !state_;
}
bool EditHistory::matches(std::string_view value) const noexcept {
    return state_ && current() == value;
}
bool EditHistory::canUndo(std::string_view value) const noexcept {
    return matches(value) && state_->cursor > 0;
}
bool EditHistory::canRedo(std::string_view value) const noexcept {
    return matches(value) && state_->cursor + 1 < state_->entries.size();
}
size_t EditHistory::payloadBytes() const noexcept {
    return state_ ? state_->bytes : 0;
}
std::string_view EditHistory::current() const noexcept {
    return state_ ? std::string_view(*state_->entries[state_->cursor]) : std::string_view{};
}
EditHistory EditHistory::withEdit(std::string_view before, std::string_view after) const {
    if (before == after)
        return *this;
    EditHistory result(limits_);
    // A large document is still editable. It simply cannot retain an undo
    // snapshot within this journal's explicit payload budget.
    if (after.size() > limits_.payloadBytes)
        return result;
    auto next = std::make_shared<State>();
    next->entries.reserve(limits_.edits + 2);
    if (matches(before)) {
        next->entries.assign(state_->entries.begin(), state_->entries.begin() + state_->cursor + 1);
        for (const auto& entry : next->entries)
            next->bytes += entry->size();
    } else if (before.size() <= limits_.payloadBytes) {
        next->entries.push_back(std::make_shared<const std::string>(before));
        next->bytes = before.size();
    }
    next->entries.push_back(std::make_shared<const std::string>(after));
    next->bytes += after.size();
    while (next->entries.size() > limits_.edits + 1 || next->bytes > limits_.payloadBytes) {
        next->bytes -= next->entries.front()->size();
        next->entries.erase(next->entries.begin());
    }
    next->cursor = next->entries.size() - 1;
    result.state_ = std::move(next);
    return result;
}
EditHistory EditHistory::withUndo(std::string_view value) const {
    if (!canUndo(value))
        throw std::invalid_argument("No matching routing edit to undo");
    auto next = std::make_shared<State>(*state_);
    --next->cursor;
    EditHistory result(limits_);
    result.state_ = std::move(next);
    return result;
}
EditHistory EditHistory::withRedo(std::string_view value) const {
    if (!canRedo(value))
        throw std::invalid_argument("No matching routing edit to redo");
    auto next = std::make_shared<State>(*state_);
    ++next->cursor;
    EditHistory result(limits_);
    result.state_ = std::move(next);
    return result;
}
void EditHistory::swap(EditHistory& prepared) noexcept {
    std::swap(limits_, prepared.limits_);
    state_.swap(prepared.state_);
}

} // namespace loopz
