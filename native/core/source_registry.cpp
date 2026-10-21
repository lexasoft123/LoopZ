#include <loopz/source_registry.hpp>
#include <loopz/recovery.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace loopz {
namespace {
void silence(RoutingOutput output, uint32_t frames) noexcept {
    for (auto* plane : output.channels)
        if (plane)
            std::fill_n(plane, std::min(frames, output.capacityFrames), 0.0f);
}
class ManagedSource final : public AudioSource {
  public:
    std::shared_ptr<SourceEndpoint> endpoint;
    std::atomic<uint64_t> admission{0};
    mutable std::atomic<uint64_t> missingReadFrames{0};
    AudioReadResult account(AudioReadResult result) const noexcept {
        if (result.missingFrames)
            missingReadFrames.fetch_add(result.missingFrames, std::memory_order_relaxed);
        return result;
    }
    void admit(bool available) noexcept {
        admission.store(((admission.load() + 2) & ~uint64_t{1}) | (available ? 1 : 0));
    }
    bool connected() const noexcept {
        return admission.load() & 1;
    }
    explicit ManagedSource(std::shared_ptr<SourceEndpoint> source) : endpoint(std::move(source)) {}
    ~ManagedSource() override {
        endpoint->close();
    }
    uint32_t channels() const noexcept override {
        return endpoint->channels();
    }
    AudioReadResult read(uint64_t firstHostNs, double rate, RoutingOutput output, uint32_t frames,
                         AudioReadCursor& cursor) const noexcept override {
        const auto epoch = admission.load();
        if (!(epoch & 1)) {
            silence(output, frames);
            return account({frames, false});
        }
        const auto result = endpoint->read(firstHostNs, rate, output, frames, cursor);
        if (admission.load() != epoch) {
            silence(output, frames);
            return account({frames, false});
        }
        return account(result);
    }
    AudioReadResult readWithFilter(uint64_t firstHostNs, double rate, RoutingOutput output,
                                   uint32_t frames, AudioReadCursor& cursor,
                                   ResamplingFilter filter) const noexcept override {
        const auto epoch = admission.load();
        if (!(epoch & 1)) {
            silence(output, frames);
            return account({frames, false});
        }
        const auto result =
            endpoint->readWithFilter(firstHostNs, rate, output, frames, cursor, filter);
        if (admission.load() != epoch) {
            silence(output, frames);
            return account({frames, false});
        }
        return account(result);
    }
};
bool external(SourceKind kind) noexcept {
    return kind == SourceKind::Application || kind == SourceKind::InputDevice;
}
} // namespace
struct SourceRegistry::Impl {
    struct Plan {
        SourceKind kind;
        std::string selector;
        uint32_t channels;
    };
    struct Reader {
        std::string device, source;
        std::weak_ptr<AudioReadCursor> cursor;
    };
    struct Entry {
        SourceKind kind;
        std::string selector;
        std::weak_ptr<ManagedSource> source;
        std::vector<Reader> readers;
        GraphRecovery retry;
        bool demanded{false}, wanted{false}, mute{false}, openedMute{false}, timingRejected{false};
        bool retainedTiming{false};
        uint32_t required{0}, openedRequired{0};
        uint64_t attempts{0};
        std::string error;
        Entry(SourceKind type, std::string uid, const std::shared_ptr<ManagedSource>& provider)
            : kind(type), selector(std::move(uid)), source(provider) {
            retry.setDesired({{"source", {}}}, 0);
        }
    };
    const Factory factory;
    const Clock clock;
    std::vector<Plan> plans;
    std::vector<std::unique_ptr<Entry>> entries;
    explicit Impl(Factory create, Clock time) : factory(std::move(create)), clock(std::move(time)) {
        if (!factory)
            throw std::invalid_argument("A source registry needs an endpoint factory");
    }
};
SourceRegistry::SourceRegistry(Factory factory, Clock controlClock)
    : impl_(std::make_unique<Impl>(std::move(factory), std::move(controlClock))) {}
SourceRegistry::~SourceRegistry() {
    stop();
}
std::vector<InputTimingFacts> SourceRegistry::inputTiming(SourceKind kind,
                                                          const std::string& selector) const {
    for (const auto& entry : impl_->entries)
        if (entry->kind == kind && entry->selector == selector)
            if (const auto source = entry->source.lock(); source && !entry->timingRejected) {
                if (source->connected() && source->endpoint->complete())
                    return source->endpoint->inputTiming();
                if (!source->connected() && entry->retainedTiming)
                    return source->endpoint->retainedInputTiming();
            }
    return {};
}
void SourceRegistry::rejectInputTiming(SourceKind kind, const std::string& selector) {
    for (auto& entry : impl_->entries)
        if (entry->kind == kind && entry->selector == selector) {
            entry->timingRejected = true;
            entry->retainedTiming = false;
            if (const auto source = entry->source.lock())
                source->endpoint->clearInputTiming();
            entry->error = "Capture timing exceeds supported history; reacquiring the saved source";
        }
}
void SourceRegistry::prepare(const RouteDocument& document) {
    validateRouteDocument(document);
    std::vector<Impl::Plan> plans;
    for (const auto& device : document.devices) {
        if (!device.routing.enabled)
            continue;
        for (const auto& route : device.routing.sources) {
            if (!route.enabled)
                continue;
            const auto& binding = *std::find_if(device.bindings.begin(), device.bindings.end(),
                                                [&](const auto& value) {
                                                    return value.id == route.id;
                                                });
            if (!external(binding.kind))
                continue;
            const auto found = std::find_if(plans.begin(), plans.end(), [&](const auto& plan) {
                return plan.kind == binding.kind && plan.selector == binding.selector;
            });
            if (found == plans.end())
                plans.push_back({binding.kind, binding.selector, route.channels});
            else
                found->channels = std::max(found->channels, route.channels);
        }
    }
    impl_->plans = std::move(plans);
}
ResolvedAudioSource SourceRegistry::resolve(const VirtualDeviceDefinition& device,
                                            const SourceBinding& binding,
                                            const SourceRouting& routing) {
    auto& self = *impl_;
    if (!external(binding.kind))
        throw std::invalid_argument("Only capture bindings belong to the source registry");
    const auto planned = std::find_if(self.plans.begin(), self.plans.end(), [&](const auto& plan) {
        return plan.kind == binding.kind && plan.selector == binding.selector;
    });
    if (planned == self.plans.end() || planned->channels < routing.channels)
        throw std::logic_error("Capture source was not prepared with the full document");
    std::erase_if(self.entries, [](const auto& entry) {
        return entry->source.expired();
    });
    Impl::Entry* chosen = nullptr;
    std::shared_ptr<ManagedSource> source;
    for (auto& entry : self.entries) {
        if (entry->kind != binding.kind || entry->selector != binding.selector)
            continue;
        auto candidate = entry->source.lock();
        if (candidate && candidate->channels() >= planned->channels) {
            chosen = entry.get();
            source = std::move(candidate);
            break;
        }
    }
    if (!source) {
        auto endpoint = self.factory(binding.kind, binding.selector, planned->channels);
        if (!endpoint || endpoint->channels() < planned->channels ||
            endpoint->channels() > kMaximumRoutingChannels)
            throw std::runtime_error("Capture provider cannot represent the requested channels");
        for (const auto& entry : self.entries)
            if (const auto existing = entry->source.lock())
                if (existing->endpoint == endpoint)
                    throw std::runtime_error(
                        "Capture factory aliased independently owned endpoints");
        source = std::make_shared<ManagedSource>(std::move(endpoint));
        auto entry = std::make_unique<Impl::Entry>(binding.kind, binding.selector, source);
        chosen = entry.get();
        self.entries.push_back(std::move(entry));
    }
    std::erase_if(chosen->readers, [](const auto& reader) {
        return reader.cursor.expired();
    });
    for (const auto& reader : chosen->readers)
        if (reader.device == device.id && reader.source == routing.id)
            if (auto cursor = reader.cursor.lock())
                return {source, std::move(cursor)};
    auto cursor = std::make_shared<AudioReadCursor>();
    chosen->readers.push_back({device.id, routing.id, cursor});
    return {std::move(source), std::move(cursor)};
}
void SourceRegistry::poll(const AudioScene* committed, uint64_t now, uint32_t supplementalDemand,
                          uint32_t previewDemand) {
    auto& self = *impl_;
    auto observedNow = now;
    const auto currentTime = [&] {
        if (self.clock)
            observedNow = std::max(observedNow, self.clock());
        return observedNow;
    };
    // Keep unchanged capture alive across the interval between scene commit
    // and its first audio block. An all-idle rendered scene still closes it.
    if (committed && !committed->hasRendered())
        return;
    const auto playbackDemand = committed ? committed->requestedDevices() | supplementalDemand : 0;
    const auto demand = committed
                            ? committed->demandedDevices() |
                                  committed->requestedDevices(previewDemand) | supplementalDemand
                            : 0;
    for (auto& entry : self.entries) {
        entry->wanted = entry->mute = false;
        entry->required = 0;
    }
    if (committed) {
        const auto& devices = committed->document().devices;
        for (size_t device = 0; device < devices.size(); ++device) {
            if (!(demand & (uint32_t{1} << device)))
                continue;
            for (size_t input = 0; input < devices[device].routing.sources.size(); ++input) {
                const auto& route = devices[device].routing.sources[input];
                const auto* resolved = committed->input(device, input);
                if (!route.enabled || !resolved || !resolved->source)
                    continue;
                const auto& binding =
                    *std::find_if(devices[device].bindings.begin(), devices[device].bindings.end(),
                                  [&](const auto& item) {
                                      return item.id == route.id;
                                  });
                for (auto& entry : self.entries) {
                    auto provider = entry->source.lock();
                    if (provider && resolved->source.get() == provider.get()) {
                        entry->wanted = true;
                        entry->required = std::max(entry->required, route.channels);
                        entry->mute = entry->mute || ((playbackDemand & (uint32_t{1} << device)) &&
                                                      binding.kind == SourceKind::Application &&
                                                      binding.muteOriginal);
                        break;
                    }
                }
            }
        }
    }
    for (auto& entry : self.entries) {
        const auto source = entry->source.lock();
        if (!source)
            continue;
        if (!entry->wanted) {
            if (entry->demanded) {
                entry->retainedTiming = false;
                if (!entry->timingRejected && source->connected() && source->endpoint->complete()) {
                    try {
                        entry->retainedTiming = !source->endpoint->retainedInputTiming().empty();
                    } catch (...) {
                        // Timing inspection must never prevent an idle capture closing.
                    }
                }
                source->admit(false);
                source->endpoint->close();
                entry->retry.restart(currentTime());
                entry->error.clear();
            }
            entry->demanded = false;
            entry->timingRejected = false;
            continue;
        }
        if (!entry->demanded)
            entry->retry.restart(currentTime());
        entry->demanded = true;
        if (entry->retry.ready("source")) {
            bool healthy = false;
            try {
                healthy = !entry->timingRejected && source->endpoint->healthy(entry->error);
            } catch (const std::exception& error) {
                entry->error = error.what();
            }
            bool policyChanged =
                entry->openedMute != entry->mute || entry->openedRequired != entry->required;
            if (healthy && policyChanged &&
                source->endpoint->updatePolicy({entry->required, entry->mute})) {
                entry->openedMute = entry->mute;
                entry->openedRequired = entry->required;
                entry->error.clear();
                policyChanged = false;
            }
            if (!healthy || policyChanged) {
                entry->retainedTiming = false;
                if (!entry->timingRejected) {
                    try {
                        entry->retainedTiming = !source->endpoint->retainedInputTiming().empty();
                    } catch (...) {
                        // Recovery must still close a failed producer.
                    }
                }
                source->admit(false);
                source->endpoint->close();
                entry->timingRejected = false;
                entry->retry.lost("source", currentTime());
                if (healthy)
                    entry->retry.retryNow(currentTime()); // A policy requiring stream replacement.
            }
        }
        // Earlier providers may have blocked this control poll. Start this
        // provider's deadline when its own acquisition actually begins.
        for (const auto& attempt : entry->retry.attempts(currentTime())) {
            bool opened = false;
            entry->error.clear();
            ++entry->attempts;
            try {
                opened = source->endpoint->open({entry->required, entry->mute}, entry->error);
            } catch (const std::exception& error) {
                entry->error = error.what();
            }
            const auto completedAt = currentTime();
            if (!entry->retry.complete(attempt, opened, completedAt)) {
                opened = false;
                entry->error = "Source acquisition expired; retrying the saved binding";
            }
            if (!opened)
                source->endpoint->close();
            else {
                entry->openedMute = entry->mute;
                entry->openedRequired = entry->required;
                entry->error.clear();
            }
            source->admit(opened);
        }
    }
    std::erase_if(self.entries, [](const auto& entry) {
        return entry->source.expired();
    });
}
void SourceRegistry::retryNow(uint64_t now) noexcept {
    for (auto& entry : impl_->entries)
        entry->retry.retryNow(now);
}
void SourceRegistry::stop() noexcept {
    for (auto& entry : impl_->entries) {
        if (const auto source = entry->source.lock()) {
            source->admit(false);
            source->endpoint->close();
        }
        entry->demanded = entry->wanted = false;
        entry->timingRejected = false;
        entry->retainedTiming = false;
        entry->retry.restart(0);
    }
}
bool SourceRegistry::restored() const noexcept {
    for (const auto& entry : impl_->entries)
        if (entry->demanded) {
            const auto source = entry->source.lock();
            if (entry->timingRejected || !entry->retry.restored() || !source ||
                !source->connected() || !source->endpoint->complete())
                return false;
        }
    return true;
}
std::vector<SourceConnectionStatus> SourceRegistry::status() const {
    std::vector<SourceConnectionStatus> result;
    for (const auto& entry : impl_->entries)
        if (const auto source = entry->source.lock())
            result.push_back({entry->kind, entry->selector, source->channels(), entry->required,
                              entry->demanded, source->connected(),
                              source->connected() && source->endpoint->complete(),
                              entry->openedMute, entry->attempts, entry->error,
                              source->missingReadFrames.load(std::memory_order_relaxed),
                              source->endpoint->diagnostics()});
    return result;
}
} // namespace loopz
