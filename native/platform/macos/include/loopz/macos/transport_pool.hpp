#pragma once

#include <loopz/macos/device_transport.hpp>
#include <loopz/route_document.hpp>
#include <loopz/causal_resampler.hpp>
#include <loopz/sample_rates.hpp>

namespace loopz::macos {

// Maximum scene lookback plus the causal converter's upsampling bound,
// rescaled from the minimum 8 kHz render clock to the highest public rate.
inline constexpr uint32_t kMaximumInputLatencyFrames =
    (kCapacity / 2 + CausalResampler::kMaximumUpsamplingDelayFrames) *
    (kVirtualSampleRates.back() / 8000);

struct DeviceTransport {
    std::string id;
    std::string uid;
    std::string name;
    bool hasOutput;
    std::shared_ptr<DeviceAudioRegion> region;
    uint32_t inputLatencyFrames{0};
};

class PreparedDeviceTransports {
  public:
    PreparedDeviceTransports(PreparedDeviceTransports&&) noexcept = default;
    PreparedDeviceTransports& operator=(PreparedDeviceTransports&&) noexcept = default;
    PreparedDeviceTransports(const PreparedDeviceTransports&) = delete;
    PreparedDeviceTransports& operator=(const PreparedDeviceTransports&) = delete;
    const std::vector<DeviceTransport>& devices() const noexcept;

  private:
    friend class DeviceTransportPool;
    PreparedDeviceTransports(std::shared_ptr<const uint8_t> owner, uint64_t version,
                             std::vector<DeviceTransport> devices);
    std::shared_ptr<const uint8_t> owner_;
    uint64_t version_;
    std::vector<DeviceTransport> devices_;
};

// Single control-thread owner. Audio callbacks borrow views from a separately
// pinned region; they never access this vector or manipulate shared ownership.
class DeviceTransportPool {
  public:
    DeviceTransportPool() = default;
    ~DeviceTransportPool();
    DeviceTransportPool(const DeviceTransportPool&) = delete;
    DeviceTransportPool& operator=(const DeviceTransportPool&) = delete;

    // Prepares every changed mapping before committing any change. Rename and
    // routing-only edits with unchanged timing preserve active audio; format or
    // input-delay changes, removal and
    // disabling retire the previous generation to silence. Re-enable gets a
    // fresh region. This prepares transport, not HAL device publication.
    void apply(const RouteDocument& document, std::span<const uint32_t> inputLatencies = {});
    // Publish the prepared snapshot first, then commit only after the driver
    // reports Committed. A successful submit may still mean Pending host
    // approval. Rejection/destruction preserves every active region.
    PreparedDeviceTransports prepare(const RouteDocument& document,
                                     std::span<const uint32_t> inputLatencies = {});
    void commit(PreparedDeviceTransports&& prepared);
    const std::vector<DeviceTransport>& devices() const noexcept;

  private:
    std::vector<DeviceTransport> devices_;
    uint64_t nextGeneration_{1};
    uint64_t version_{0};
    const std::shared_ptr<const uint8_t> owner_{std::make_shared<const uint8_t>(0)};
};

} // namespace loopz::macos
