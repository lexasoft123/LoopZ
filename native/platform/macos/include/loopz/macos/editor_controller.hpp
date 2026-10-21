#pragma once

#include <loopz/macos/configuration_controller.hpp>
#include <loopz/route_edits.hpp>
#include <loopz/edit_history.hpp>

namespace loopz::macos {

struct EditorRevision {
    // A new, unpredictable identity for each engine process; never reused after
    // restart, even if the document's generation happens to be the same.
    std::string epoch;
    uint64_t generation;
    bool operator==(const EditorRevision&) const = default;
};
struct EditorSnapshot {
    EditorRevision revision;
    std::optional<RouteDocument> document;
    RestorationStatus restoration;
    bool canCreateDocument{false};
    struct History {
        bool canUndo{false}, canRedo{false};
    } history{};
};
enum class EditorHistoryDirection { Undo, Redo };
struct EditorCommit {
    EditorRevision revision;
    bool changed;
};
class StaleEditorRevision : public std::runtime_error {
  public:
    StaleEditorRevision()
        : std::runtime_error("Routing changed; refresh before applying this edit") {}
};

// Called only by the engine's serialized control owner, after its IPC adapter
// authenticates the editor. No filesystem path, replacement document or format
// receipt comes from the editor. Saving desired intent is distinct from native
// activation; the snapshot exposes both generations and the restoration phase.
class EditorController {
  public:
    EditorController(ConfigurationController& configuration, std::string epoch);
    EditorSnapshot snapshot() const;
    EditorCommit apply(const EditorRevision& expected, std::span<const edit::Command> commands);
    EditorCommit initialize(const EditorRevision& expected,
                            std::span<const edit::Command> commands);
    EditorCommit moveHistory(const EditorRevision& expected, EditorHistoryDirection direction);

  private:
    ConfigurationController& configuration_;
    const std::string epoch_;
    // A snapshot may discard history fenced out by an external intent change.
    // These caches belong to the same serialized control owner as the editor.
    mutable EditHistory history_;
    mutable uint64_t historyGeneration_{0};
    void synchronizeHistory(const RouteDocument& current, uint64_t generation) const;
};

} // namespace loopz::macos
