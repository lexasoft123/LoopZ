#pragma once
#include "device_intent_store.hpp"
#include <windows.h>
#include <functional>
#include <map>
#include <string>

namespace loopz::windows {
enum class BrokerApplyResult { Applied, RebootRequired };
enum class BrokerDevicePhase { Applying, Ready, Removed, Queued, RebootRequired, Error };
struct BrokerDeviceProgress {
    std::wstring owner;
    std::string mixId;
    uint64_t revision{};
    BrokerDevicePhase phase{};
    std::string error;
    DeviceIntentKind kind{DeviceIntentKind::MicrophoneMix};
};
class BrokerDeviceAdapter {
  public:
    virtual ~BrokerDeviceAdapter() = default;
    virtual BrokerApplyResult apply(std::wstring_view owner,
                                    const DeviceIntentRequest& desired) = 0;
};
class WindowsBrokerDeviceAdapter final : public BrokerDeviceAdapter {
  public:
    explicit WindowsBrokerDeviceAdapter(std::wstring installedInf);
    BrokerApplyResult apply(std::wstring_view owner, const DeviceIntentRequest& desired) override;

  private:
    std::wstring installedInf_;
};
// One background control owner, with its own SQLite connection. Never holds a
// transaction while executing PnP operations. Progress is revision-tagged: callers
// must compare it with current desired state before presenting readiness.
class BrokerReconciler final {
  public:
    using Progress = std::function<void(const BrokerDeviceProgress&)>;
    BrokerReconciler(DeviceIntentStore& store, BrokerDeviceAdapter& adapter, HANDLE stopEvent,
                     Progress progress);
    // True requests another pass (superseded work or a retryable failure).
    bool pass();
    void invalidate(); // Device/maintenance notifications invalidate completed-work cache.
  private:
    DeviceIntentStore& store_;
    BrokerDeviceAdapter& adapter_;
    HANDLE stop_{};
    Progress progress_;
    using Key = std::pair<std::wstring, std::string>;
    std::map<Key, uint64_t> completed_;
};
} // namespace loopz::windows
