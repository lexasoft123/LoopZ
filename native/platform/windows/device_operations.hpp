#pragma once
#include "driver/audio_format.hpp"
#include <windows.h>
#include <setupapi.h>
#include <string>
#include <string_view>
#include <loopz/route_document.hpp>

namespace loopz::windows {
// Shared cross-process lock for service and elevated maintenance operations.
// Nonblocking acquisition: a busy operation is retried asynchronously.
class DeviceSetupLock final {
  public:
    DeviceSetupLock();
    ~DeviceSetupLock();
    DeviceSetupLock(const DeviceSetupLock&) = delete;
    DeviceSetupLock& operator=(const DeviceSetupLock&) = delete;

  private:
    HANDLE value_{};
};
// Privileged control threads only, serialized by the setup/service owner.
// Hardware identity alone is not account authorization: service callers must
// also verify the protected owner SID before mutating an existing device.
bool isLoopZDevice(HDEVINFO set, SP_DEVINFO_DATA& device);
std::wstring installedDevicePackage(HDEVINFO set, SP_DEVINFO_DATA& device);
bool deviceStarted(const SP_DEVINFO_DATA& device);
// Preserve the pending marker until endpoint format finalization succeeds.
bool recordDeviceFormat(HKEY deviceKey, loopz_driver::AudioFormat format);
// New nodes may initialize a missing record. Existing nodes must already carry
// the exact sink configuration; ordinary repair never changes adapter role.
bool recordAppRoutingSinkConfiguration(HKEY deviceKey, bool newlyCreated);

// Rejects unrelated hardware; returns whether Windows requests a restart.
bool removeLoopZDevice(HDEVINFO set, SP_DEVINFO_DATA& device);
struct DeviceUpsertResult {
    SP_DEVINFO_DATA device{};
    bool created{}, rebootRequired{};
};
// Does not stage a driver package. installedInf is a trusted installer
// receipt, never an IPC field. Existing nodes require the matching protected
// owner record before mutation; only newly registered nodes may be claimed.
// Only installation/repair may pass allowPackageChange=true. Routine service
// setup rejects an existing adapter bound to a different package and restarts
// matching adapters without package reselection. Caller serializes device
// operations and finalizes public endpoint formats.
DeviceUpsertResult upsertMixDevice(HDEVINFO set, std::wstring_view ownerSid,
                                   const VirtualDeviceDefinition& mix,
                                   std::wstring_view installedInf, bool allowPackageChange = false);
DeviceUpsertResult upsertAppRoutingSink(HDEVINFO set, std::wstring_view ownerSid,
                                        std::wstring_view installedInf,
                                        bool allowPackageChange = false);
void finalizeAppRoutingSink(HDEVINFO set, std::wstring_view ownerSid);
// Fail closed on absent, malformed, or mismatched protected owner records.
bool deviceOwnerMatches(HKEY deviceKey, std::wstring_view ownerSid);
void finalizeMixDevice(HDEVINFO set, std::wstring_view ownerSid,
                       const VirtualDeviceDefinition& mix);
struct DeviceRemovalResult {
    bool existed{}, rebootRequired{};
};
// Explicit one-mix removal only. Missing devices are idempotent success; no
// absent-record pruning or caller-provided instance paths exist in this API.
DeviceRemovalResult removeMixDevice(std::wstring_view ownerSid, std::string_view mixId);
DeviceRemovalResult removeAppRoutingSink(std::wstring_view ownerSid);
} // namespace loopz::windows
