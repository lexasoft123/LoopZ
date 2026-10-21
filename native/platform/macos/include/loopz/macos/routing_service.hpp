#pragma once

#include <loopz/macos/configuration_controller.hpp>
#include <xpc/xpc.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace loopz::macos {
inline constexpr char kRoutingServiceName[] = "com.lexasoft.loopz.routing.bridge";

// Production uses the system-domain Mach service, root server UID and an exact
// installed broker code requirement. Anonymous endpoints use the same checks
// and exist for isolated embedding/tests, never as an authentication bypass.
struct RoutingServiceEndpoint {
    std::string serviceName{kRoutingServiceName};
    std::string serverRequirement;
    uint32_t serverUID{0};
    bool privileged{true};
    xpc_endpoint_t endpoint{nullptr};
};
// Reads the fixed, root-owned, non-symlinked installed broker pin. Missing or
// untrusted metadata fails closed; there is no unsigned/unpinned fallback.
RoutingServiceEndpoint installedRoutingServiceEndpoint();
// HAL hosts cannot read the helper's external configuration. Resolve the
// caller's own loaded image, validate its driver bundle and the exact bytes of
// its sealed Resources/routing-service.json. No external-file fallback.
RoutingServiceEndpoint bundledRoutingServiceEndpoint(const void* imageSymbol);

struct RoutingBrokerPolicy {
    std::string helperRequirement;
    std::string driverRequirement;
    uint32_t driverUID;
    bool allowRootDriver{true};
    std::function<std::optional<uint32_t>()> consoleUser;
};

class RoutingBroker {
  public:
    static std::unique_ptr<RoutingBroker> anonymous(RoutingBrokerPolicy policy);
    static std::unique_ptr<RoutingBroker>
    machService(RoutingBrokerPolicy policy, std::string serviceName = kRoutingServiceName);
    ~RoutingBroker();
    XPC_RETURNS_RETAINED xpc_endpoint_t endpoint() const;
    void stop();

  private:
    struct Impl;
    explicit RoutingBroker(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Control-thread adapter used by ConfigurationController. poll() maintains the
// authenticated transport with bounded reconnect backoff; session() is zero
// while either side is unavailable. No call belongs on an audio callback.
class RoutingDriverLink final : public DriverConfigurationLink {
  public:
    explicit RoutingDriverLink(RoutingServiceEndpoint endpoint);
    ~RoutingDriverLink();
    void poll(uint64_t nowMilliseconds) override;
    uint64_t session() const noexcept override;
    uint32_t sampleRateMask() const noexcept override;
    int32_t submit(std::span<const DeviceTransport> devices, uint64_t revision) override;
    DriverConfigurationResult status(uint64_t revision) override;
    int32_t cancel(uint64_t revision) override;
    std::vector<DriverRateRequest> rateRequests() override;
    void acknowledgeRates(std::span<const DriverRateRequest> requests) override;
    void invalidate() noexcept override;
    std::string lastError() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Production pin discovery remains live while installation is unavailable.
// Missing/untrusted metadata never falls back to an unpinned connection. Pin
// replacement disconnects the old peer, with non-repeating wrapper sessions.
class InstalledDriverLink final : public DriverConfigurationLink {
  public:
    using EndpointLoader = std::function<RoutingServiceEndpoint()>;
    explicit InstalledDriverLink(EndpointLoader loader = installedRoutingServiceEndpoint);
    ~InstalledDriverLink();
    void poll(uint64_t nowMilliseconds) override;
    uint64_t session() const noexcept override;
    uint32_t sampleRateMask() const noexcept override;
    int32_t submit(std::span<const DeviceTransport> devices, uint64_t revision) override;
    DriverConfigurationResult status(uint64_t revision) override;
    int32_t cancel(uint64_t revision) override;
    std::vector<DriverRateRequest> rateRequests() override;
    void acknowledgeRates(std::span<const DriverRateRequest> requests) override;
    void invalidate() noexcept override;
    std::string lastError() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct RoutingDriverOperations {
    std::function<int32_t(xpc_object_t)> apply;
    std::function<DriverConfigurationResult(uint64_t)> status;
    std::function<int32_t(uint64_t)> cancel;
    std::function<void(bool)> availability;
    std::function<std::vector<DriverRateRequest>()> rates{};
    std::function<int32_t(std::span<const DriverRateRequest>)> acknowledgeRates{};
    uint32_t sampleRateMask{kAllVirtualSampleRates};
    std::function<void(uint32_t)> sampleRatesChanged{};
};

// Starts asynchronous driver-side connection/reconnection on its own control
// queue. Operations execute serially off the HAL callback. Destruction drains
// operation callbacks before their contexts may be released.
class RoutingDriverService {
  public:
    RoutingDriverService(RoutingServiceEndpoint endpoint, RoutingDriverOperations operations);
    ~RoutingDriverService();
    void stop();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz::macos
