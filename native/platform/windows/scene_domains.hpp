#pragma once
#include <loopz/audio_scene.hpp>
#include <loopz/render_schedule.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace loopz::windows {
struct SceneDomainPlan {
    uint32_t rate{};
    std::vector<size_t> devices;
};
// Control-domain partition. Every virtual dependency stays in one rate domain,
// including disabled bindings, so no connected graph loses nominal bandwidth.
inline std::vector<SceneDomainPlan> planSceneDomains(const RouteDocument& document) {
    const auto count = document.devices.size();
    if (count > kMaximumVirtualDevices)
        throw std::invalid_argument("Too many devices for Windows render domains");
    std::map<std::string, size_t> indices;
    std::vector<size_t> parents(count);
    std::iota(parents.begin(), parents.end(), size_t{0});
    for (size_t i = 0; i < count; ++i) {
        const auto& device = document.devices[i];
        if (!indices.emplace(device.id, i).second || !std::isfinite(device.sampleRate) ||
            device.sampleRate < 8000 || device.sampleRate > 192000 ||
            std::floor(device.sampleRate) != device.sampleRate)
            throw std::invalid_argument("Invalid Windows render-domain device");
    }
    const auto root = [&](size_t index) {
        while (parents[index] != index)
            index = parents[index];
        return index;
    };
    for (size_t i = 0; i < count; ++i)
        for (const auto& binding : document.devices[i].bindings)
            if (binding.kind == SourceKind::VirtualDevice) {
                const auto found = indices.find(binding.selector);
                if (found != indices.end())
                    parents[root(i)] = root(found->second);
            }
    std::vector<uint32_t> rates(count);
    for (size_t i = 0; i < count; ++i)
        rates[root(i)] =
            std::max(rates[root(i)], static_cast<uint32_t>(document.devices[i].sampleRate));
    std::map<uint32_t, std::vector<size_t>> grouped;
    for (size_t i = 0; i < count; ++i)
        grouped[rates[root(i)]].push_back(i);
    std::vector<SceneDomainPlan> result;
    for (auto& [rate, devices] : grouped)
        result.push_back({rate, std::move(devices)});
    return result;
}
using SceneDomainSchedules = std::map<uint32_t, std::shared_ptr<RenderSchedule>>;
class SceneDomains final {
  public:
    struct RenderTiming {
        std::atomic<uint64_t> calls{0}, totalNs{0}, maximumNs{0}, maximumLatenessNs{0};
    };
    struct Domain {
        uint32_t rate;
        std::shared_ptr<RenderSchedule> schedule;
        std::unique_ptr<AudioScene> scene;
        std::unique_ptr<RenderTiming> timing;
    };
    SceneDomains(const RouteDocument& document, const std::vector<SceneDomainPlan>& plan,
                 AudioScene::SourceResolver source, AudioScene::SinkResolver sink,
                 std::shared_ptr<SceneHistoryStore> histories, SceneDomainSchedules& schedules,
                 bool stageTiming)
        : addresses_(document.devices.size()), rates_(document.devices.size()) {
        for (const auto& partition : plan) {
            RouteDocument subset;
            subset.version = document.version;
            for (const auto device : partition.devices) {
                addresses_.at(device) = {domains_.size(), subset.devices.size()};
                rates_.at(device) = partition.rate;
                subset.devices.push_back(document.devices.at(device));
            }
            auto& schedule = schedules[partition.rate];
            if (!schedule)
                schedule = std::make_shared<RenderSchedule>(
                    RenderScheduleTiming{partition.rate, 256, 0, partition.rate * 4096 / 48000});
            auto scene = std::make_unique<AudioScene>(
                std::move(subset),
                SceneTiming{partition.rate, 256, partition.rate * 2048 / 48000,
                            partition.rate * 1024 / 48000, stageTiming},
                source, sink, histories);
            domains_.push_back(
                {partition.rate, schedule, std::move(scene), std::make_unique<RenderTiming>()});
        }
    }
    // All ownership changes and preparation precede publication. A single
    // serial render owner steps these immutable domain entries after the swap.
    const std::vector<Domain>& domains() const noexcept {
        return domains_;
    }
    uint32_t rate(size_t device) const noexcept {
        return rates_[device];
    }
    void activate() noexcept {
        for (const auto& domain : domains_)
            domain.scene->activate();
    }
    SceneDeviceStatus deviceStatus(size_t device) const noexcept {
        const auto address = addresses_[device];
        return domains_[address.domain].scene->deviceStatus(address.device);
    }
    SceneSourceStatus sourceStatus(size_t device, size_t source) const noexcept {
        const auto address = addresses_[device];
        return domains_[address.domain].scene->sourceStatus(address.device, source);
    }
    bool deviceMeters(size_t device, AudioMeterSnapshot& value) const noexcept {
        const auto address = addresses_[device];
        return domains_[address.domain].scene->deviceMeters(address.device, value);
    }
    bool sourceMeters(size_t device, size_t source, AudioMeterSnapshot& value) const noexcept {
        const auto address = addresses_[device];
        return domains_[address.domain].scene->sourceMeters(address.device, source, value);
    }
    const ResolvedAudioSource* input(size_t device, size_t source) const noexcept {
        const auto address = addresses_[device];
        return domains_[address.domain].scene->input(address.device, source);
    }

  private:
    struct Address {
        size_t domain{}, device{};
    };
    std::vector<Domain> domains_;
    std::vector<Address> addresses_;
    std::vector<uint32_t> rates_;
};
} // namespace loopz::windows
