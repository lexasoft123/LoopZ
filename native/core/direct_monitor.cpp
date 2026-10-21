#include <loopz/monitor.hpp>
#include <loopz/timeline.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace loopz {
namespace {
bool sameWires(const std::vector<ChannelConnection>& left,
               const std::vector<ChannelConnection>& right) noexcept {
    return std::equal(left.begin(), left.end(), right.begin(), right.end(),
                      [](const auto& a, const auto& b) {
                          return a.sourceChannel == b.sourceChannel &&
                                 a.outputChannel == b.outputChannel && a.gain == b.gain;
                      });
}
bool audible(const SourceRouting& source) {
    return source.enabled && !source.muted && source.gain > 0 &&
           std::any_of(source.connections.begin(), source.connections.end(), [](const auto& wire) {
               return wire.gain > 0;
           });
}
} // namespace

uint32_t directMonitorInputChannels(const VirtualDeviceDefinition& device,
                                    const std::string& physicalUid) {
    uint32_t required = 0;
    if (physicalUid.empty())
        return 0;
    for (const auto& source : device.routing.sources) {
        if (!audible(source))
            continue;
        const auto binding =
            std::find_if(device.bindings.begin(), device.bindings.end(), [&](const auto& value) {
                return value.id == source.id;
            });
        if (binding == device.bindings.end() || binding->kind != SourceKind::InputDevice ||
            binding->selector != physicalUid)
            return 0;
        required = std::max(required, source.channels);
    }
    return required;
}

struct DirectMonitorProgram::Impl {
    const VirtualDeviceDefinition device;
    const uint32_t inputs, outputs;
    const double rate;
    RoutingGraph deviceGraph, monitorGraph;
    AudioMeter meter;
    std::array<std::array<float, kBlock>, kMaximumRoutingChannels> capture{}, mixed{}, rendered{};
    std::array<std::array<const float*, kMaximumRoutingChannels>, kMaximumRoutingSources>
        sourcePlanes{};
    std::array<RoutingInput, kMaximumRoutingSources> sources{};
    std::array<float*, kMaximumRoutingChannels> mixPlanes{}, outputPlanes{};
    std::array<const float*, kMaximumRoutingChannels> mixRead{}, outputRead{};
    std::atomic<uint64_t> callbacks{0}, failures{0}, invalid{0};
    std::atomic<bool> online{false};

    Impl(const VirtualDeviceDefinition& definition, const MonitorDefinition& monitor,
         uint32_t physicalInputs, double sampleRate)
        : device(definition), inputs(physicalInputs), outputs(monitor.channels), rate(sampleRate),
          deviceGraph(rate, definition.routing),
          monitorGraph(rate, monitorRouting(definition, monitor)), meter(outputs) {
        for (size_t index = 0; index < device.routing.sources.size(); ++index) {
            const auto& source = device.routing.sources[index];
            for (uint32_t channel = 0; channel < source.channels; ++channel)
                sourcePlanes[index][channel] = capture[channel].data();
            sources[index] = {{sourcePlanes[index].data(), source.channels}, kBlock};
        }
        for (uint32_t channel = 0; channel < kMaximumRoutingChannels; ++channel) {
            mixRead[channel] = mixPlanes[channel] = mixed[channel].data();
            outputRead[channel] = outputPlanes[channel] = rendered[channel].data();
        }
    }
};

DirectMonitorProgram::DirectMonitorProgram(const VirtualDeviceDefinition& device,
                                           const MonitorDefinition& monitor, const std::string& uid,
                                           uint32_t inputs, double rate) {
    validateRoutingConfiguration(device.routing);
    if (!directMonitorInputChannels(device, uid) || !inputs || inputs > kMaximumRoutingChannels ||
        !std::isfinite(rate) || rate < 8000 || rate > 192000)
        throw std::invalid_argument("Invalid shared-clock monitor configuration");
    impl_ = std::make_unique<Impl>(device, monitor, inputs, rate);
}
DirectMonitorProgram::~DirectMonitorProgram() = default;

MonitorRenderResult DirectMonitorProgram::render(RoutingInput input, RoutingOutput output,
                                                 uint32_t frames, uint64_t position) noexcept {
    auto& self = *impl_;
    const auto validPlanes = [](const auto& planes) {
        return std::all_of(planes.begin(), planes.end(), [](const auto* plane) {
            return plane;
        });
    };
    if (!frames || frames > 16384 || frames > input.capacityFrames ||
        frames > output.capacityFrames || input.channels.size() != self.inputs ||
        output.channels.empty() || output.channels.size() > kMaximumRoutingChannels ||
        !validPlanes(input.channels) || !validPlanes(output.channels) ||
        position > UINT64_MAX - frames) {
        for (auto* plane : output.channels)
            if (plane)
                std::fill_n(plane, std::min(frames, output.capacityFrames), 0.f);
        self.meter.clear();
        self.online.store(false);
        self.invalid.fetch_add(1);
        return {false, false, frames, 0};
    }
    uint32_t failed = 0;
    for (uint32_t offset = 0; offset < frames; offset += kBlock) {
        const auto count = std::min(frames - offset, kBlock);
        for (uint32_t channel = 0; channel < self.inputs; ++channel)
            for (uint32_t frame = 0; frame < count; ++frame) {
                const auto sample = input.channels[channel][offset + frame];
                self.capture[channel][frame] = std::isfinite(sample) ? sample : 0.f;
            }
        if (!self.deviceGraph.render(
                {self.sources.data(), self.device.routing.sources.size()},
                {{self.mixPlanes.data(), self.device.routing.outputChannels}, kBlock}, count,
                position + offset))
            ++failed;
        const RoutingInput mixed{{self.mixRead.data(), self.device.routing.outputChannels}, kBlock};
        if (!self.monitorGraph.render({&mixed, 1},
                                      {{self.outputPlanes.data(), self.outputs}, kBlock}, count,
                                      position + offset))
            ++failed;
        for (uint32_t channel = 0; channel < self.outputs; ++channel)
            if (channel >= output.channels.size())
                std::fill_n(self.rendered[channel].data(), count, 0.f);
        for (uint32_t channel = 0; channel < output.channels.size(); ++channel)
            for (uint32_t frame = 0; frame < count; ++frame) {
                const auto sample = channel < self.outputs ? self.rendered[channel][frame] : 0.f;
                output.channels[channel][offset + frame] = std::isfinite(sample) ? sample : 0.f;
            }
        self.meter.publish({{self.outputRead.data(), self.outputs}, kBlock}, count);
    }
    self.callbacks.fetch_add(1);
    self.failures.fetch_add(failed);
    self.online.store(true);
    return {true, true, 0, failed};
}

bool DirectMonitorProgram::meters(AudioMeterSnapshot& value) const noexcept {
    return impl_->meter.read(value);
}
MonitorStatus DirectMonitorProgram::status() const noexcept {
    return {impl_->callbacks.load(), 0,          impl_->failures.load(), impl_->invalid.load(),
            impl_->online.load(),    impl_->rate};
}
uint32_t DirectMonitorProgram::inputChannels() const noexcept {
    return impl_->inputs;
}
double DirectMonitorProgram::sampleRate() const noexcept {
    return impl_->rate;
}
bool DirectMonitorProgram::equivalentDevice(const VirtualDeviceDefinition& device) const noexcept {
    const auto& a = impl_->device.routing;
    const auto& b = device.routing;
    if (a.outputChannels != b.outputChannels || a.gain != b.gain || a.enabled != b.enabled ||
        a.muted != b.muted || a.safetyLimiter != b.safetyLimiter ||
        a.sources.size() != b.sources.size() ||
        impl_->device.bindings.size() != device.bindings.size())
        return false;
    for (size_t index = 0; index < a.sources.size(); ++index) {
        const auto& left = a.sources[index];
        const auto& right = b.sources[index];
        if (left.id != right.id || left.channels != right.channels || left.gain != right.gain ||
            left.enabled != right.enabled || left.muted != right.muted ||
            !sameWires(left.connections, right.connections))
            return false;
    }
    return std::all_of(
        impl_->device.bindings.begin(), impl_->device.bindings.end(), [&](const auto& left) {
            return std::any_of(device.bindings.begin(), device.bindings.end(),
                               [&](const auto& right) {
                                   return left.id == right.id && left.kind == right.kind &&
                                          left.selector == right.selector;
                               });
        });
}
} // namespace loopz
