#pragma once

#include <loopz/macos/editor_controller.hpp>

namespace loopz::macos {

inline constexpr uint32_t kEditorProtocolVersion = 1;
inline constexpr size_t kMaximumEditorRequestBytes = 1024 * 1024;

struct EditorRequest {
    EditorRevision expected;
    std::vector<edit::Command> commands;
    std::optional<EditorHistoryDirection> history{};
};

// JSON is an interchange format, never an authentication mechanism. The local
// transport must authenticate peers before dispatching requests to the control
// owner. Generation counters use decimal strings to remain exact in JavaScript.
EditorRequest decodeEditorRequest(std::string_view json);
std::string encodeEditorSnapshot(const EditorSnapshot& snapshot);
std::string encodeEditorCommit(const EditorCommit& commit);

} // namespace loopz::macos
