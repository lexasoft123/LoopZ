#pragma once
#include <loopz/device_intent.hpp>
#include <filesystem>
#include <memory>
#include <vector>

namespace loopz {
struct DeviceIntentSnapshot {
    uint64_t revision{};
    std::vector<DeviceIntentRequest> records;
};
// Control thread only. The platform adapter supplies a protected database path
// and an authenticated opaque owner identity; neither may come from request fields.
// initialize=true permits an explicitly provisioned empty database, including
// recovery after interrupted first initialization. It never resets existing data.
class DeviceIntentDatabase {
  public:
    explicit DeviceIntentDatabase(const std::filesystem::path& path, bool initialize = false);
    ~DeviceIntentDatabase();
    DeviceIntentDatabase(const DeviceIntentDatabase&) = delete;
    DeviceIntentDatabase& operator=(const DeviceIntentDatabase&) = delete;
    // Identical retries return their original revision, possibly superseded by
    // later edits. Reconcile snapshot(), never the incoming retry directly.
    uint64_t accept(std::string_view ownerIdentity, const DeviceIntentRequest& request);
    DeviceIntentSnapshot snapshot(std::string_view ownerIdentity) const;
    // Protected service recovery only; includes owners with explicit removal
    // tombstones, excludes receipt-only owners. Absence never authorizes deletion.
    std::vector<std::string> ownersWithDesiredState() const;
    bool removalApplied(std::string_view ownerIdentity, std::string_view mixId, uint64_t revision,
                        DeviceIntentKind kind = DeviceIntentKind::MicrophoneMix);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz
