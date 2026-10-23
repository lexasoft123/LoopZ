#pragma once
#include <loopz/app_routing_journal.hpp>
#include <filesystem>
#include <memory>

namespace loopz::windows {
std::filesystem::path appRoutingStoreRoot();
// Control-thread user storage, never privileged broker storage. The default is
// a dedicated LocalAppData child; an explicit root is for isolated tests.
// Pins every existing ancestor and the database against rename for its lifetime.
// Rejects reparse points, hard-linked files and unexpected ownership/access.
class AppRoutingStore final {
  public:
    explicit AppRoutingStore(const std::filesystem::path& root = appRoutingStoreRoot(),
                             bool initialize = true);
    ~AppRoutingStore();
    AppRoutingStore(const AppRoutingStore&) = delete;
    AppRoutingStore& operator=(const AppRoutingStore&) = delete;
    // This reference must not outlive the store and its pinned handles.
    AppRoutingJournal& journal() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz::windows
