#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace loopz {
enum class AppRoutingReceiptPhase { Prepared, Applied, UserOverride, Releasing };
struct AppRoutingReceipt {
    std::string application;
    uint64_t revision{};
    std::array<std::string, 3> original;
    std::vector<std::string> attemptedTargets;
    AppRoutingReceiptPhase phase{AppRoutingReceiptPhase::Prepared};
};
enum class AppRoutingRestore { Unchanged, RestoreOriginal, PreserveUserChange };
std::array<AppRoutingRestore, 3> appRoutingRestoration(const AppRoutingReceipt&,
                                                       const std::array<std::string, 3>& current);

// Portable SQLite write-ahead ownership journal, used only on control threads.
// The platform resolves the user-owned parent directory, leaving the database
// filename unchanged so the no-follow check can reject a symlink at that name.
// The platform authenticates the application identity and validates virtual
// targets. Commit prepare() BEFORE any OS preference mutation. A failed/unknown
// setter outcome remains recoverable because unconfirmed targets are retained.
class AppRoutingJournal final {
  public:
    explicit AppRoutingJournal(const std::filesystem::path&, bool initialize = false);
    ~AppRoutingJournal();
    AppRoutingJournal(const AppRoutingJournal&) = delete;
    AppRoutingJournal& operator=(const AppRoutingJournal&) = delete;
    // Every prepare advances revision, even when retrying an earlier target;
    // acknowledgments for prior restoration attempts must become stale.
    AppRoutingReceipt prepare(std::string_view application,
                              const std::array<std::string, 3>& original, std::string_view target);
    // After the platform verifies all roles equal target, discard older targets
    // so a later user selection of an older Feed is recognized as an override.
    bool confirm(std::string_view application, uint64_t expectedRevision, std::string_view target);
    // Invalidate in-flight apply acknowledgments before restoration starts.
    std::optional<AppRoutingReceipt>
    beginRelease(std::string_view application, uint64_t expectedRevision, AppRoutingReceiptPhase);
    std::vector<AppRoutingReceipt> snapshot() const;
    // Call only after restoring/confirming each role or preserving a user
    // override. An old acknowledgment cannot erase a retargeted/new receipt.
    bool forget(std::string_view application, uint64_t expectedRevision);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz
