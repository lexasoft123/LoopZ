#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace loopz::windows {
// Thread-confined control API. Windows exposes this through a private WinRT
// interface, so activation and every call must be checked at runtime. No global
// default/reset operation is exposed. Not used by the production mixer yet.
// Construct, use and destroy on the same MTA control thread.
class AppAudioPolicy final {
  public:
    AppAudioPolicy();
    ~AppAudioPolicy();
    AppAudioPolicy(const AppAudioPolicy&) = delete;
    AppAudioPolicy& operator=(const AppAudioPolicy&) = delete;
    bool available() const noexcept;
    std::string error() const;
    // role: console=0, multimedia=1, communications=2. Empty means system default.
    // Strings are opaque persisted interface IDs; retain exact values for restore.
    bool get(uint32_t pid, unsigned role, std::wstring& deviceInterface, std::string& error);
    bool set(uint32_t pid, unsigned role, const std::wstring& deviceInterface, std::string& error);
    static std::wstring renderInterface(const std::string& endpointUid);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz::windows
