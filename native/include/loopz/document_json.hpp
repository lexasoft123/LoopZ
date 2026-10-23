#pragma once
#include <loopz/route_edits.hpp>
#include <nlohmann/json.hpp>

namespace loopz {
using JSON = nlohmann::json;
JSON encodeDocument(const RouteDocument& document);
RouteDocument decodeDocument(const JSON& value);
std::vector<edit::Command> decodeCommands(const JSON& value);
// Bounded/depth-limited JSON entry point for untrusted IPC and saved files.
JSON parseControlJSON(std::string_view bytes);
} // namespace loopz
