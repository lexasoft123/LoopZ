#pragma once
#include <loopz/device_intent_database.hpp>
#include <filesystem>
#include <memory>

namespace loopz::windows {
std::filesystem::path deviceIntentStoreRoot();
// Installation only. Existing insecure directories are rejected, not repaired.
void provisionDeviceIntentStore(const std::filesystem::path& root = deviceIntentStoreRoot());

// Privileged control thread only. The installed broker always uses the fixed
// ProgramData root; the explicit root is for isolated administrative tests.
// Neither root nor owner SID may come from request fields: derive the SID from
// the authenticated token. SQLite transactions serialize writers.
class DeviceIntentStore {
  public:
    explicit DeviceIntentStore(const std::filesystem::path& root = deviceIntentStoreRoot());
    ~DeviceIntentStore();
    DeviceIntentStore(const DeviceIntentStore&) = delete;
    DeviceIntentStore& operator=(const DeviceIntentStore&) = delete;
    uint64_t accept(std::wstring_view ownerSid, const DeviceIntentRequest& request);
    DeviceIntentSnapshot snapshot(std::wstring_view ownerSid) const;
    std::vector<std::wstring> ownersWithDesiredState() const;
    bool removalApplied(std::wstring_view ownerSid, std::string_view mixId, uint64_t revision,
                        DeviceIntentKind kind = DeviceIntentKind::MicrophoneMix);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz::windows
