#include <loopz/application_timing.hpp>
#include <loopz/timeline.hpp>

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace loopz {
void retainActiveApplicationTiming(const RouteDocument& desired, const AudioScene& active,
                                   std::span<DeviceInputDelay> delays) {
    const auto demand = active.requestedDevices();
    const auto& committed = active.document();
    for (auto& delay : delays) {
        const auto desiredDevice =
            std::find_if(desired.devices.begin(), desired.devices.end(), [&](const auto& device) {
                return device.id == delay.device;
            });
        if (desiredDevice == desired.devices.end())
            continue;
        const auto binding = std::find_if(desiredDevice->bindings.begin(),
                                          desiredDevice->bindings.end(), [&](const auto& source) {
                                              return source.id == delay.source;
                                          });
        if (binding == desiredDevice->bindings.end() || binding->kind != SourceKind::Application)
            continue;
        for (size_t device = 0; device < committed.devices.size(); ++device) {
            const auto& previous = committed.devices[device];
            if (!(demand & (uint32_t{1} << device)) || previous.id != delay.device)
                continue;
            for (size_t source = 0; source < previous.routing.sources.size(); ++source) {
                const auto& route = previous.routing.sources[source];
                const auto oldBinding = std::find_if(
                    previous.bindings.begin(), previous.bindings.end(), [&](const auto& value) {
                        return value.id == route.id;
                    });
                if (route.enabled && route.id == delay.source &&
                    oldBinding->kind == binding->kind && oldBinding->selector == binding->selector)
                    delay.frames = std::max(
                        delay.frames,
                        active.sourceStatus(device, source)
                            .historyBudgetFrames.value_or(active.timing().sourceDelayFrames));
            }
        }
    }
}

ApplicationInputTiming::ApplicationInputTiming(SceneTiming timing, uint32_t lead, Probe probe,
                                               Reject reject)
    : timing_(timing), lead_(lead), probe_(std::move(probe)), reject_(std::move(reject)) {
    (void)planSceneTiming({}, timing_);
    if (!probe_ || lead_ > kCapacity / 2)
        throw std::invalid_argument("Invalid application timing policy");
}
std::vector<DeviceInputDelay> ApplicationInputTiming::delays(const RouteDocument& document,
                                                             const RouteDocument* active) {
    validateRouteDocument(document);
    if (active) {
        std::unordered_set<std::string> referenced;
        for (const auto* graph : {&document, active})
            for (const auto& device : graph->devices)
                for (const auto& binding : device.bindings)
                    if (binding.kind == SourceKind::Application)
                        referenced.insert(binding.selector);
        std::erase_if(retained_, [&](const auto& entry) {
            return !referenced.contains(entry.first);
        });
    }
    std::unordered_map<std::string, uint32_t> current;
    std::vector<DeviceInputDelay> result;
    for (const auto& device : document.devices) {
        if (!device.routing.enabled)
            continue;
        for (const auto& route : device.routing.sources) {
            if (!route.enabled)
                continue;
            const auto binding = std::find_if(device.bindings.begin(), device.bindings.end(),
                                              [&](const auto& value) {
                                                  return value.id == route.id;
                                              });
            if (binding->kind != SourceKind::Application)
                continue;
            auto found = current.find(binding->selector);
            if (found == current.end()) {
                auto budget = std::max(1024U, timing_.sourceDelayFrames);
                const auto facts = probe_(binding->selector);
                bool verified = !facts.empty() && facts.size() <= kMaximumRoutingSources;
                uint32_t candidate = 0;
                if (verified) {
                    for (const auto& stream : facts) {
                        const auto value = inputTimingBudget(stream, timing_, lead_);
                        if (!value) {
                            verified = false;
                            break;
                        }
                        candidate = std::max(candidate, *value);
                    }
                }
                if (!facts.empty() && !verified) {
                    if (!reject_)
                        throw std::runtime_error(
                            "Application capture timing exceeds supported history");
                    reject_(binding->selector);
                }
                if (verified) {
                    budget = candidate;
                    constexpr auto maximumIdentities =
                        2 * kMaximumVirtualDevices * kMaximumRoutingSources;
                    if (!retained_.contains(binding->selector) &&
                        retained_.size() >= maximumIdentities)
                        throw std::runtime_error("Too many application timing identities");
                    retained_[binding->selector] = budget;
                } else if (const auto prior = retained_.find(binding->selector);
                           prior != retained_.end()) {
                    budget = std::max(budget, prior->second);
                }
                found = current.emplace(binding->selector, budget).first;
            }
            result.push_back({device.id, binding->id, found->second});
        }
    }
    return result;
}
} // namespace loopz
