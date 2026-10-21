#include <loopz/input_timing.hpp>
#include <loopz/sinc_kernel.hpp>
#include <loopz/timeline.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

namespace loopz {
std::vector<DeviceInputFilter> planPhysicalInputFilters(const RouteDocument& document,
                                                        ResamplingFilter preferred) {
    validateRouteDocument(document);
    if (preferred != ResamplingFilter::LinearPhase && preferred != ResamplingFilter::MinimumPhase)
        throw std::invalid_argument("Unsupported physical input filter");
    std::vector<DeviceInputFilter> result;
    std::map<std::pair<std::string, std::string>, size_t> bindings;
    std::map<std::string, size_t> indices;
    for (size_t i = 0; i < document.devices.size(); ++i) {
        const auto& device = document.devices[i];
        indices.emplace(device.id, i);
        if (!device.routing.enabled)
            continue;
        for (const auto& route : device.routing.sources) {
            if (!route.enabled)
                continue;
            const auto binding = std::find_if(device.bindings.begin(), device.bindings.end(),
                                              [&](const auto& value) {
                                                  return value.id == route.id;
                                              });
            if (binding->kind == SourceKind::InputDevice) {
                bindings.emplace(std::pair{device.id, binding->id}, result.size());
                result.push_back({device.id, binding->id, preferred});
            }
        }
    }
    if (preferred == ResamplingFilter::LinearPhase)
        return result;
    struct Inputs {
        std::set<std::pair<SourceKind, std::string>> identities;
        std::set<size_t> physical;
        bool visited{false};
    };
    std::vector<Inputs> inputs(document.devices.size());
    std::function<void(size_t)> visit = [&](size_t index) {
        auto& input = inputs[index];
        if (input.visited)
            return;
        input.visited = true;
        const auto& device = document.devices[index];
        if (!device.routing.enabled)
            return;
        for (const auto& route : device.routing.sources) {
            if (!route.enabled)
                continue;
            const auto binding = std::find_if(device.bindings.begin(), device.bindings.end(),
                                              [&](const auto& value) {
                                                  return value.id == route.id;
                                              });
            if (binding->kind == SourceKind::VirtualDevice) {
                const auto upstream = indices.at(binding->selector);
                visit(upstream);
                input.identities.insert(inputs[upstream].identities.begin(),
                                        inputs[upstream].identities.end());
                input.physical.insert(inputs[upstream].physical.begin(),
                                      inputs[upstream].physical.end());
            } else {
                input.identities.emplace(binding->kind, binding->kind == SourceKind::PassThrough
                                                            ? device.id
                                                            : binding->selector);
                if (binding->kind == SourceKind::InputDevice)
                    input.physical.insert(bindings.at({device.id, binding->id}));
            }
        }
        if (input.identities.size() > 1)
            for (const auto physical : input.physical)
                result[physical].filter = ResamplingFilter::LinearPhase;
    };
    for (size_t i = 0; i < document.devices.size(); ++i)
        visit(i);
    return result;
}
std::optional<uint32_t> inputTimingBudget(InputTimingFacts input, SceneTiming render,
                                          uint32_t lead) noexcept {
    return inputTimingBudget(input, render, lead, ResamplingFilter::LinearPhase);
}
std::optional<uint32_t> inputTimingBudget(InputTimingFacts input, SceneTiming render, uint32_t lead,
                                          ResamplingFilter filter) noexcept {
    if (!std::isfinite(input.sampleRate) || input.sampleRate < 8000 || input.sampleRate > 192000 ||
        !input.bufferFrames || input.bufferFrames > kCapacity / 2 || render.renderRate < 8000 ||
        render.renderRate > 192000 ||
        (render.blockFrames != 32 && render.blockFrames != 64 && render.blockFrames != 128 &&
         render.blockFrames != kBlock) ||
        lead > kCapacity / 2 ||
        (filter != ResamplingFilter::LinearPhase && filter != ResamplingFilter::MinimumPhase))
        return {};
    const auto ratio = double(render.renderRate) / input.sampleRate;
    // Independent clocks need interpolation even at the same nominal rate.
    // Past-only support does not require any not-yet-delivered input samples.
    const auto cutoff = SincKernel::kCutoff * std::min(1.0, ratio);
    const auto wing =
        filter == ResamplingFilter::MinimumPhase ? 0.0 : std::ceil(SincKernel::kRadius / cutoff);
    const auto required = (double(input.latencyFrames) + input.bufferFrames + wing) * ratio + lead +
                          2.0 * render.blockFrames;
    const auto rounded = std::ceil(required / render.blockFrames) * render.blockFrames;
    if (!std::isfinite(rounded) || rounded > kCapacity / 2)
        return {};
    const auto radius = std::ceil(SincKernel::kRadius / cutoff);
    const auto past = filter == ResamplingFilter::MinimumPhase ? 2 * radius : radius;
    // Transport storage may fit while a faster capture has already overwritten
    // the past samples required by this budget and filter support.
    if (rounded / ratio + past > kCapacity)
        return {};
    return static_cast<uint32_t>(rounded);
}
} // namespace loopz
