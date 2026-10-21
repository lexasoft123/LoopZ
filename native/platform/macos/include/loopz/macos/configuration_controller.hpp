#pragma once

#include <loopz/macos/driver_configuration.hpp>
#include <loopz/macos/route_store.hpp>
#include <loopz/macos/transport_pool.hpp>
#include <loopz/recovery.hpp>

#include <optional>

namespace loopz::macos {

// One authenticated, ordered, exclusive control session. Session identities
// never repeat for this link object's lifetime; zero means disconnected. A
// thrown submit has an uncertain outcome: a later ordered status query resolves
// it. A nonzero submit result definitively rejects that revision.
class DriverConfigurationLink {
  public:
    virtual ~DriverConfigurationLink() = default;
    virtual void poll(uint64_t) {}
    virtual uint32_t sampleRateMask() const noexcept {
        return kAllVirtualSampleRates;
    }
    virtual std::vector<DriverRateRequest> rateRequests() {
        return {};
    }
    virtual void acknowledgeRates(std::span<const DriverRateRequest>) {}
    virtual uint64_t session() const noexcept = 0;
    virtual int32_t submit(std::span<const DeviceTransport> devices, uint64_t revision) = 0;
    virtual DriverConfigurationResult status(uint64_t revision) = 0;
    virtual int32_t cancel(uint64_t revision) = 0;
    virtual void invalidate() noexcept = 0;
};

enum class RestorationPhase {
    NoDesiredDocument,
    WaitingForDriver,
    Reconciling,
    Applying,
    Cancelling,
    Retry,
    Restored,
};
struct RestorationStatus {
    RestorationPhase phase;
    uint64_t desiredGeneration;
    uint64_t activeGeneration;
    uint64_t transactionRevision;
    int32_t error;
    std::string detail;
    bool recoveredBackup;
};

// Preparation may fail before the native request is sent. A prepared graph
// pins its mappings and reserves publication capacity until commit or abort.
// commit() must not allocate, throw or destroy an audio thread's borrowed graph.
class PreparedGraphActivation {
  public:
    virtual ~PreparedGraphActivation() = default;
    virtual void commit() noexcept = 0;
};
// One control-thread planning snapshot. Native transport allocation and DSP
// preparation must consume the same metadata, even when endpoint clocks change
// between those steps. The activation owner outlives its plans and updates.
class GraphActivationPlan {
  public:
    virtual ~GraphActivationPlan() = default;
    virtual std::span<const uint32_t> inputLatencyFrames() const noexcept = 0;
    virtual std::unique_ptr<PreparedGraphActivation>
    prepare(std::span<const DeviceTransport> devices) = 0;
};
class GraphActivation {
  public:
    virtual ~GraphActivation() = default;
    // Endpoint input delay in document device order, planned before mappings.
    // An owner without DSP may omit metadata; a renderer must supply it.
    virtual std::vector<uint32_t> inputLatencyFrames(const RouteDocument&) const {
        return {};
    }
    virtual std::unique_ptr<GraphActivationPlan> plan(const RouteDocument& document) = 0;
    virtual std::unique_ptr<PreparedGraphActivation>
    prepare(const RouteDocument& document, std::span<const DeviceTransport> devices) = 0;
};

// Single control-thread owner. This class never requests permissions, opens
// capture, or mutates default devices. The caller pins render resources and
// joins their callbacks before destroying this owner. Publication of prepared
// DSP graphs is staged before submission and committed after the exact native
// acknowledgement when an activation owner is supplied. That owner outlives
// this controller; an offline configuration tool may omit it.
class ConfigurationController {
  public:
    explicit ConfigurationController(DriverConfigurationLink& link,
                                     std::filesystem::path documentPath = {},
                                     GraphActivation* activation = nullptr,
                                     bool loadDocument = true);
    ~ConfigurationController();
    ConfigurationController(const ConfigurationController&) = delete;
    ConfigurationController& operator=(const ConfigurationController&) = delete;

    // Validate before replacing intent. Equal canonical documents are a no-op.
    // In-memory changes never implicitly overwrite the saved document.
    void setDesired(RouteDocument document);
    void saveDesired(RouteDocument document);
    void createDesired(RouteDocument document);
    void reloadDesired();
    const RouteDocument* desired() const noexcept;
    const RouteDocument* active() const noexcept;
    const std::vector<DeviceTransport>& devices() const noexcept;
    void poll(uint64_t nowMilliseconds);
    void retryNow(uint64_t nowMilliseconds) noexcept;
    RestorationStatus status() const;

  private:
    struct Pending {
        RouteDocument document;
        PreparedDeviceTransports transports;
        uint64_t desiredGeneration;
        uint64_t revision;
        std::unique_ptr<PreparedGraphActivation> activation;
        bool cancelRequested{false};
    };
    void replaceDesired(RouteDocument document, bool save, bool create = false);
    void abandonPending() noexcept;
    void newSession(uint64_t session, uint64_t now);
    void pendingResult(uint64_t now);
    bool currentSession(uint64_t now);
    void retry(uint64_t now, int32_t error, std::string detail);
    void protocolFailure(uint64_t now, std::string detail);
    void checkResult(const DriverConfigurationResult& result, uint64_t revision) const;

    DriverConfigurationLink& link_;
    GraphActivation* activation_;
    const std::filesystem::path path_;
    DeviceTransportPool pool_;
    std::optional<RouteDocument> desired_, active_;
    std::string canonicalDesired_;
    std::optional<Pending> pending_;
    GraphRecovery submission_;
    uint64_t session_{0}, desiredGeneration_{0}, activeGeneration_{0};
    uint64_t nextRevision_{0}, orphanCancellation_{0}, nextPoll_{0};
    bool synchronized_{false}, recoveredBackup_{false};
    int32_t error_{0};
    std::string detail_;
};

} // namespace loopz::macos
