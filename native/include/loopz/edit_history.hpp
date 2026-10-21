#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

namespace loopz {

struct EditHistoryLimits {
    size_t edits{32};
    size_t payloadBytes{8 * 1024 * 1024};
};

// Control-thread journal of opaque, canonical intent snapshots. The backend
// validates/encodes documents and supplies the current intent as a fence.
// Preparing a new journal may allocate; swap after durable save cannot throw.
// Payload limits exclude bounded STL bookkeeping and transient prepared state.
class EditHistory {
  public:
    explicit EditHistory(EditHistoryLimits limits = {});
    bool empty() const noexcept;
    bool matches(std::string_view current) const noexcept;
    bool canUndo(std::string_view current) const noexcept;
    bool canRedo(std::string_view current) const noexcept;
    size_t payloadBytes() const noexcept;
    std::string_view current() const noexcept;

    EditHistory withEdit(std::string_view current, std::string_view next) const;
    EditHistory withUndo(std::string_view current) const;
    EditHistory withRedo(std::string_view current) const;
    void swap(EditHistory& prepared) noexcept;

  private:
    struct State;
    EditHistoryLimits limits_;
    std::shared_ptr<const State> state_;
};

} // namespace loopz
