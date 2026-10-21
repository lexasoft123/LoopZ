#include <loopz/sample_rates.hpp>
#include <loopz/macos/transport_pool.hpp>

#include <algorithm>
#include <stdexcept>

namespace loopz::macos {
namespace {
void retire(const DeviceTransport& device) noexcept {
    device.region->retire();
}
} // namespace

DeviceTransportPool::~DeviceTransportPool() {
    for (const auto& device : devices_)
        retire(device);
}

PreparedDeviceTransports::PreparedDeviceTransports(std::shared_ptr<const uint8_t> owner,
                                                   uint64_t version,
                                                   std::vector<DeviceTransport> devices)
    : owner_(std::move(owner)), version_(version), devices_(std::move(devices)) {}

const std::vector<DeviceTransport>& PreparedDeviceTransports::devices() const noexcept {
    return devices_;
}

void DeviceTransportPool::apply(const RouteDocument& document,
                                std::span<const uint32_t> inputLatencies) {
    commit(prepare(document, inputLatencies));
}

PreparedDeviceTransports DeviceTransportPool::prepare(const RouteDocument& document,
                                                      std::span<const uint32_t> inputLatencies) {
    validateRouteDocument(document);
    if ((!inputLatencies.empty() && inputLatencies.size() != document.devices.size()) ||
        std::any_of(inputLatencies.begin(), inputLatencies.end(), [](uint32_t latency) {
            return latency > kMaximumInputLatencyFrames;
        }))
        throw std::invalid_argument("Invalid device input latency metadata");
    if (version_ == UINT64_MAX)
        throw std::overflow_error("Device transport revision exhausted");
    std::vector<DeviceTransport> prepared;
    prepared.reserve(document.devices.size());
    auto generation = nextGeneration_;
    for (size_t index = 0; index < document.devices.size(); ++index) {
        const auto& definition = document.devices[index];
        const auto latency = inputLatencies.empty() ? 0 : inputLatencies[index];
        // Check formats before skipping disabled devices, so a saved unsupported
        // format cannot surprise the backend on a later enable operation.
        if (!supportedVirtualSampleRate(definition.sampleRate))
            throw std::invalid_argument("Unsupported virtual-device sample rate");
        if (!definition.routing.enabled)
            continue;
        const auto previous =
            std::find_if(devices_.begin(), devices_.end(), [&](const auto& device) {
                return device.id == definition.id;
            });
        std::shared_ptr<DeviceAudioRegion> region;
        if (previous != devices_.end()) {
            const auto view = previous->region->view();
            if (!view.header().retired.load() &&
                view.channels() == definition.routing.outputChannels &&
                view.sampleRate() == definition.sampleRate &&
                previous->inputLatencyFrames == latency)
                region = previous->region;
        }
        if (!region) {
            if (generation == UINT64_MAX)
                throw std::overflow_error("Device audio generation exhausted");
            region = DeviceAudioRegion::create(definition.routing.outputChannels,
                                               static_cast<uint32_t>(definition.sampleRate),
                                               generation++);
        }
        prepared.push_back({definition.id, "com.lexasoft.loopz.device." + definition.id,
                            definition.name, hasPlaybackEndpoint(definition), std::move(region),
                            latency});
    }
    // Reserve generations even if the transaction is later abandoned. An
    // already-exported tentative mapping must never share a generation with a
    // different future mapping for the same device.
    nextGeneration_ = generation;
    return {owner_, version_, std::move(prepared)};
}

void DeviceTransportPool::commit(PreparedDeviceTransports&& prepared) {
    if (prepared.owner_ != owner_ || prepared.version_ != version_)
        throw std::invalid_argument("Stale or foreign device transport preparation");
    if (version_ == UINT64_MAX)
        throw std::overflow_error("Device transport revision exhausted");
    // Everything that can throw has completed. Deactivate only superseded
    // regions; a renamed/reordered device must not interrupt its current stream.
    for (const auto& previous : devices_) {
        const bool retained = std::any_of(prepared.devices_.begin(), prepared.devices_.end(),
                                          [&](const auto& device) {
                                              return device.region == previous.region;
                                          });
        if (!retained)
            retire(previous);
    }
    devices_.swap(prepared.devices_);
    ++version_;
    prepared.owner_.reset();
    prepared.devices_.clear();
}

const std::vector<DeviceTransport>& DeviceTransportPool::devices() const noexcept {
    return devices_;
}

} // namespace loopz::macos
