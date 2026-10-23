#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace loopz::windows {
struct RoutingProcessIdentity {
    std::string application; // Durable journal key, not a PID or display name.
    std::wstring ownerSid, reportedImage, resolvedImage, applicationUserModelId, packageFullName;
    uint32_t pid{}, session{};
    uint64_t created100ns{};
};
// Control thread only. Pins one process object and authenticates the process
// against the current user/session and the selected executable. Does not sign
// or authenticate executable contents. New instances must be resolved explicitly;
// a dead object never follows a reused PID. expectedApplication binds recovery
// to an existing receipt. Packaged hosts without an application ID are refused.
class AppRoutingProcess final {
  public:
    AppRoutingProcess(uint32_t pid, const std::filesystem::path& selectedImage,
                      std::string_view expectedApplication = {});
    ~AppRoutingProcess();
    AppRoutingProcess(const AppRoutingProcess&) = delete;
    AppRoutingProcess& operator=(const AppRoutingProcess&) = delete;
    const RoutingProcessIdentity& identity() const noexcept;
    void validate() const;
    uint32_t livePid() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz::windows
