#include <loopz/macos/editor_controller.hpp>

#include <stdexcept>
#include <utility>

namespace loopz::macos {
namespace {
std::string intent(RouteDocument document) {
    for (auto& device : document.devices)
        device.formatRequestReceipt.clear();
    return encodeRouteDocumentJSON(document);
}
void preserveReceipts(RouteDocument& target, const RouteDocument& current) {
    for (auto& device : target.devices)
        for (const auto& live : current.devices)
            if (live.id == device.id) {
                device.formatRequestReceipt = live.formatRequestReceipt;
                break;
            }
}
} // namespace

EditorController::EditorController(ConfigurationController& configuration, std::string epoch)
    : configuration_(configuration), epoch_(std::move(epoch)) {
    if (epoch_.empty() || epoch_.size() > 128 || epoch_.find('\0') != std::string::npos)
        throw std::invalid_argument("The editor requires a bounded engine epoch identity");
}

EditorSnapshot EditorController::snapshot() const {
    const auto status = configuration_.status();
    EditorSnapshot result{{epoch_, status.desiredGeneration}, std::nullopt, status};
    if (const auto* document = configuration_.desired()) {
        result.document = *document;
        synchronizeHistory(*document, status.desiredGeneration);
        result.history = {history_.canUndo(history_.current()),
                          history_.canRedo(history_.current())};
    }
    return result;
}

void EditorController::synchronizeHistory(const RouteDocument& current, uint64_t generation) const {
    if (historyGeneration_ == generation)
        return;
    if (!history_.empty() && !history_.matches(intent(current)))
        history_ = EditHistory{};
    historyGeneration_ = generation;
}

EditorCommit EditorController::apply(const EditorRevision& expected,
                                     std::span<const edit::Command> commands) {
    const auto before = configuration_.status().desiredGeneration;
    if (expected.epoch != epoch_ || expected.generation != before)
        throw StaleEditorRevision();
    const auto* desired = configuration_.desired();
    if (!desired)
        throw std::runtime_error("No routing document is loaded; repair or create it first");

    auto candidate = edit::applyCommands(*desired, commands);
    const bool changed = encodeRouteDocumentJSON(candidate) != encodeRouteDocumentJSON(*desired);
    // Allocate the reply's epoch before saving. Once saveDesired succeeds, the
    // return path cannot throw from constructing a reply and hide the commit.
    EditorCommit result{{epoch_, before}, changed};
    if (changed) {
        auto prepared = history_.withEdit(intent(*desired), intent(candidate));
        configuration_.saveDesired(std::move(candidate));
        // The canonical comparison above matches the configuration owner's
        // comparison; a changed save advances exactly once (or throws first).
        result.revision.generation = before + 1;
        history_.swap(prepared);
        historyGeneration_ = result.revision.generation;
    }
    return result;
}

EditorCommit EditorController::initialize(const EditorRevision& expected,
                                          std::span<const edit::Command> commands) {
    const auto before = configuration_.status().desiredGeneration;
    if (expected.epoch != epoch_ || expected.generation != before || configuration_.desired())
        throw StaleEditorRevision();
    auto candidate = edit::applyCommands({}, commands);
    if (candidate.devices.empty())
        throw std::invalid_argument("Create at least one virtual device to begin");
    auto prepared = history_.withEdit(intent({}), intent(candidate));
    EditorCommit result{{epoch_, before + 1}, true};
    configuration_.createDesired(std::move(candidate));
    history_.swap(prepared);
    historyGeneration_ = result.revision.generation;
    return result;
}

EditorCommit EditorController::moveHistory(const EditorRevision& expected,
                                           EditorHistoryDirection direction) {
    const auto before = configuration_.status().desiredGeneration;
    if (expected.epoch != epoch_ || expected.generation != before)
        throw StaleEditorRevision();
    const auto* desired = configuration_.desired();
    if (!desired)
        throw std::runtime_error("No routing document is loaded");
    synchronizeHistory(*desired, before);
    const auto current = intent(*desired);
    auto prepared = direction == EditorHistoryDirection::Undo ? history_.withUndo(current)
                                                              : history_.withRedo(current);
    auto candidate = decodeRouteDocumentJSON(prepared.current());
    preserveReceipts(candidate, *desired);
    EditorCommit result{{epoch_, before + 1}, true};
    configuration_.saveDesired(std::move(candidate));
    history_.swap(prepared);
    historyGeneration_ = result.revision.generation;
    return result;
}
} // namespace loopz::macos
