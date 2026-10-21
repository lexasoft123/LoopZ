#include <loopz/monitor.hpp>
#include <loopz/timeline.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace loopz {
namespace {
bool validRate(double rate) noexcept {
    return std::isfinite(rate) && rate >= 8000 && rate <= 192000;
}
constexpr uint32_t kMaximumCallbackFrames = 16384;
void clear(RoutingOutput output, uint32_t frames) noexcept {
    for (auto* channel : output.channels)
        if (channel)
            std::fill_n(channel, std::min(frames, output.capacityFrames), 0.0f);
}
} // namespace
MonitorBus::MonitorBus(uint32_t channels, uint32_t renderRate, uint32_t capacityFrames)
    : history_(std::make_shared<AudioHistory>(channels, capacityFrames)), rate_(renderRate) {
    if (!validRate(renderRate))
        throw std::invalid_argument("Invalid monitor bus rate");
}
uint32_t MonitorBus::channels() const noexcept {
    return history_->channels();
}
uint32_t MonitorBus::renderRate() const noexcept {
    return rate_;
}
std::shared_ptr<const AudioSource> MonitorBus::source() const noexcept {
    return history_;
}
bool MonitorBus::publish(uint64_t firstFrame, uint32_t rate, RoutingInput input,
                         uint32_t frames) noexcept {
    // Keep the conversion bounded before calling frameNs (which returns uint64).
    if (rate != rate_ || !frames || frames > kBlock || input.channels.size() != channels() ||
        firstFrame > (UINT64_MAX / 1000000000ULL - 11) * rate_) {
        idle();
        return false;
    }
    if (!history_->append(input, frames, frameNs(firstFrame, rate_), rate_,
                          static_cast<double>(firstFrame), true)) {
        idle();
        return false;
    }
    producing_ = true;
    return true;
}
void MonitorBus::idle() noexcept {
    if (producing_) {
        history_->stop();
        producing_ = false;
    }
}
MonitoringSink::MonitoringSink(std::shared_ptr<AudioSink> publicSink,
                               std::shared_ptr<MonitorBus> bus, bool monitorDemand)
    : public_(std::move(publicSink)), bus_(std::move(bus)), monitorDemand_(monitorDemand) {
    if (!public_ || !bus_ || public_->channels() != bus_->channels())
        throw std::invalid_argument("Monitor bus does not match its public output");
}
uint32_t MonitoringSink::channels() const noexcept {
    return bus_->channels();
}
void MonitoringSink::activate() noexcept {
    public_->activate();
}
bool MonitoringSink::requested() const noexcept {
    return monitorDemand_ || public_->requested();
}
bool MonitoringSink::publish(uint64_t firstFrame, uint32_t rate, RoutingInput input,
                             uint32_t frames) noexcept {
    const bool monitor = bus_->publish(firstFrame, rate, input, frames);
    const bool capture = public_->publish(firstFrame, rate, input, frames);
    return monitor && capture;
}
void MonitoringSink::idle() noexcept {
    bus_->idle();
    public_->idle();
}
const std::shared_ptr<MonitorBus>& MonitoringSink::bus() const noexcept {
    return bus_;
}
MonitorBuses::MonitorBuses(uint32_t renderRate) : rate_(renderRate) {
    if (!validRate(renderRate))
        throw std::invalid_argument("Invalid monitor bus clock");
}
void MonitorBuses::collect() {
    std::erase_if(entries_, [](const auto& entry) {
        return entry.bus.expired();
    });
}
std::shared_ptr<MonitoringSink> MonitorBuses::decorate(const VirtualDeviceDefinition& device,
                                                       std::shared_ptr<AudioSink> publicSink) {
    collect();
    std::shared_ptr<MonitorBus> bus;
    for (const auto& entry : entries_)
        if (entry.device == device.id && entry.channels == device.routing.outputChannels) {
            bus = entry.bus.lock();
            if (bus)
                break;
        }
    if (!bus) {
        bus = std::make_shared<MonitorBus>(device.routing.outputChannels, rate_);
        entries_.push_back({device.id, device.routing.outputChannels, bus});
    }
    const bool demand =
        device.routing.enabled &&
        std::any_of(device.monitors.begin(), device.monitors.end(), [](const auto& monitor) {
            return monitor.enabled;
        });
    return std::make_shared<MonitoringSink>(std::move(publicSink), std::move(bus), demand);
}

struct MonitorProgram::Impl {
    const MonitorTiming timing;
    const std::shared_ptr<const AudioSource> source;
    const uint32_t inputChannels, outputChannels;
    RoutingGraph graph;
    AudioReadCursor cursor;
    AudioMeter meter;
    std::array<std::array<float, kBlock>, kMaximumRoutingChannels> input{}, output{};
    std::array<float*, kMaximumRoutingChannels> inputPlanes{}, outputPlanes{};
    std::array<const float*, kMaximumRoutingChannels> inputRead{}, outputRead{};
    std::atomic<uint64_t> callbacks{0}, missing{0}, failures{0}, invalid{0};
    std::atomic<uint64_t> clockRateBits{0};
    std::atomic<bool> online{false};
    Impl(const VirtualDeviceDefinition& device, const MonitorDefinition& monitor,
         std::shared_ptr<const AudioSource> bus, MonitorTiming clock)
        : timing(clock), source(std::move(bus)), inputChannels(device.routing.outputChannels),
          outputChannels(monitor.channels),
          graph(clock.outputRate, monitorRouting(device, monitor)), meter(monitor.channels) {
        for (uint32_t channel = 0; channel < inputChannels; ++channel) {
            inputPlanes[channel] = input[channel].data();
            inputRead[channel] = input[channel].data();
        }
        for (uint32_t channel = 0; channel < outputChannels; ++channel) {
            outputPlanes[channel] = output[channel].data();
            outputRead[channel] = output[channel].data();
        }
    }
};
MonitorProgram::MonitorProgram(const VirtualDeviceDefinition& device,
                               const MonitorDefinition& monitor,
                               std::shared_ptr<const AudioSource> source, MonitorTiming timing) {
    if (!source || source->channels() != device.routing.outputChannels ||
        !validRate(timing.renderRate) || !validRate(timing.outputRate) ||
        timing.delayFrames > kCapacity / 2)
        throw std::invalid_argument("Invalid monitor source or timing");
    impl_ = std::make_unique<Impl>(device, monitor, std::move(source), timing);
}
MonitorProgram::~MonitorProgram() = default;
uint32_t MonitorProgram::channels() const noexcept {
    return impl_->outputChannels;
}
MonitorTiming MonitorProgram::timing() const noexcept {
    return impl_->timing;
}
bool MonitorProgram::meters(AudioMeterSnapshot& snapshot) const noexcept {
    return impl_->meter.read(snapshot);
}
MonitorStatus MonitorProgram::status() const noexcept {
    const auto& self = *impl_;
    return {self.callbacks.load(), self.missing.load(),
            self.failures.load(),  self.invalid.load(),
            self.online.load(),    std::bit_cast<double>(self.clockRateBits.load())};
}
MonitorRenderResult MonitorProgram::render(uint64_t firstOutputHostNs, double outputRate,
                                           RoutingOutput output, uint32_t frames, uint64_t position,
                                           double measuredOutputRate) noexcept {
    auto& self = *impl_;
    const auto clockRate = measuredOutputRate == 0 ? outputRate : measuredOutputRate;
    const auto delayNs = frameNs(self.timing.delayFrames, self.timing.renderRate);
    const bool valid =
        frames && frames <= kMaximumCallbackFrames && frames <= output.capacityFrames &&
        !output.channels.empty() && output.channels.size() <= kMaximumRoutingChannels &&
        std::all_of(output.channels.begin(), output.channels.end(),
                    [](auto* p) {
                        return p;
                    }) &&
        outputRate == self.timing.outputRate && std::isfinite(clockRate) &&
        clockRate >= outputRate * 0.995 && clockRate <= outputRate * 1.005 &&
        firstOutputHostNs > delayNs && firstOutputHostNs < UINT64_MAX - 10000000000ULL &&
        position <= UINT64_MAX - frames;
    if (!valid) {
        clear(output, frames);
        self.meter.clear();
        self.online.store(false);
        self.clockRateBits.store(0);
        self.invalid.fetch_add(1);
        return {false, false, frames, 0};
    }
    MonitorRenderResult result{true, true, 0, 0};
    self.clockRateBits.store(std::bit_cast<uint64_t>(clockRate));
    for (uint32_t offset = 0; offset < frames; offset += kBlock) {
        const auto count = std::min(frames - offset, kBlock);
        const RoutingOutput input{{self.inputPlanes.data(), self.inputChannels}, kBlock};
        const auto read =
            self.source->read(firstOutputHostNs - delayNs + frameNs(offset, clockRate), clockRate,
                              input, count, self.cursor);
        result.sourceOnline = result.sourceOnline && read.online;
        result.missingFrames += !read.online ? count : std::min(read.missingFrames, count);
        for (uint32_t channel = 0; channel < self.inputChannels; ++channel)
            for (uint32_t frame = 0; frame < count; ++frame)
                if (!read.online || read.missingFrames > count ||
                    !std::isfinite(self.input[channel][frame]))
                    self.input[channel][frame] = 0;
        const RoutingInput view{{self.inputRead.data(), self.inputChannels}, kBlock};
        const RoutingOutput routed{{self.outputPlanes.data(), self.outputChannels}, kBlock};
        if (!self.graph.render({&view, 1}, routed, count, position + offset))
            ++result.failedChunks;
        // Keep unavailable logical hardware channels silent in the meter as
        // well. A narrower profile must not rewrite the saved channel map.
        for (uint32_t channel = 0; channel < self.outputChannels; ++channel)
            for (uint32_t frame = 0; frame < count; ++frame)
                if (channel >= output.channels.size() ||
                    !std::isfinite(self.output[channel][frame]))
                    self.output[channel][frame] = 0;
        for (uint32_t channel = 0; channel < output.channels.size(); ++channel) {
            if (channel < self.outputChannels)
                std::copy_n(self.output[channel].data(), count, output.channels[channel] + offset);
            else
                std::fill_n(output.channels[channel] + offset, count, 0.0f);
        }
        self.meter.publish({{self.outputRead.data(), self.outputChannels}, kBlock}, count);
    }
    self.online.store(result.sourceOnline);
    self.callbacks.fetch_add(1);
    self.missing.fetch_add(result.missingFrames);
    self.failures.fetch_add(result.failedChunks);
    return result;
}
} // namespace loopz
