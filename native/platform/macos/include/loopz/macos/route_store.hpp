#pragma once

#include <loopz/route_document.hpp>
#include <filesystem>
#include <string_view>
#include <stdexcept>

namespace loopz::macos {

inline constexpr size_t kMaximumRouteFileBytes = 16 * 1024 * 1024;

class UnsupportedRouteVersion : public std::runtime_error {
  public:
    explicit UnsupportedRouteVersion(uint32_t version)
        : std::runtime_error("Unsupported routing document version: " + std::to_string(version)) {}
};

// Versioned interchange for the controller/editor. Parsing validates the full
// portable document and rejects missing fields, wrong types and unknown versions.
RouteDocument decodeRouteDocumentJSON(std::string_view json);
std::string encodeRouteDocumentJSON(const RouteDocument& document);

struct LoadedRouteDocument {
    RouteDocument document;
    bool recoveredBackup{false};
    std::string primaryError;
};

// The controller serializes writes. Every replacement is atomic and mode 0600;
// a last-valid backup permits recovery without silently replacing corrupt data.
LoadedRouteDocument loadRouteDocumentFile(const std::filesystem::path& path);
void saveRouteDocumentFile(const std::filesystem::path& path, const RouteDocument& document);
// First-run publication: never replace an existing primary or recovered backup.
void createRouteDocumentFile(const std::filesystem::path& path, const RouteDocument& document);

} // namespace loopz::macos
