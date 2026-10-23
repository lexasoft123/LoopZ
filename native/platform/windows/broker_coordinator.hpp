#pragma once
#include <windows.h>
#include <loopz/device_intent_plan.hpp>
#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace loopz::windows {
struct BrokerDeviceState {
    std::string phase, error;
    uint64_t revision{};
    bool removing{};
};
struct BrokerCoordinatorState {
    uint64_t generation{};
    bool examined{};
    bool accepted{}; // Desired metadata accepted; not proof of live audio/readiness.
    std::string error;
    std::map<std::string, BrokerDeviceState> devices;
    std::optional<BrokerDeviceState> routingSink;
};
struct BrokerCoordinatorTransport {
    std::function<DeviceIntentSnapshot(HANDLE)> read;
    std::function<std::string(std::string_view, HANDLE)> call;
};
// Control-thread submission/status; all IPC runs on a separate serial worker.
// Callbacks (including injected fixtures) must honor their cancellation event.
class BrokerCoordinator final {
  public:
    explicit BrokerCoordinator(BrokerCoordinatorTransport transport = {});
    ~BrokerCoordinator();
    BrokerCoordinator(const BrokerCoordinator&) = delete;
    BrokerCoordinator& operator=(const BrokerCoordinator&) = delete;
    void submit(const RouteDocument& saved, bool authoritative, uint64_t generation,
                AppRoutingSinkDemand sinkDemand = AppRoutingSinkDemand::Unknown);
    BrokerCoordinatorState state() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz::windows
