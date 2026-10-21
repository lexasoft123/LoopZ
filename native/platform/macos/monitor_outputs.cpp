#include <loopz/macos/monitor_outputs.hpp>
#include <loopz/macos/driver_protocol.hpp>
#include <loopz/realtime_publication.hpp>
#include <loopz/recovery.hpp>
#include <loopz/output_clock.hpp>
#include <loopz/macos/permissions.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace loopz::macos {
namespace {
bool ownOutput(const std::string& uid) {
    return uid == kDeviceUID || uid.starts_with("com.lexasoft.loopz.device.");
}
bool validRate(double rate) {
    return std::isfinite(rate) && rate >= 8000 && rate <= 192000;
}
void silence(const singz::AudioHostRenderBlock& block) noexcept {
    if (block.output)
        for (uint32_t channel = 0;
             channel < std::min(block.outputChannels, kMaximumRoutingChannels); ++channel)
            if (block.output[channel])
                std::fill_n(block.output[channel], std::min(block.frames, block.maximumFrames), 0);
}
struct PreparedMonitor {
    std::unique_ptr<MonitorProgram> buffered;
    std::unique_ptr<DirectMonitorProgram> direct;
    const MonitorTiming clock;
    PreparedMonitor(const VirtualDeviceDefinition& device, const MonitorDefinition& monitor,
                    const std::shared_ptr<MonitorBus>& bus, MonitorTiming timing,
                    const std::string& physicalUid, uint32_t inputs)
        : clock(timing) {
        if (inputs && directMonitorInputChannels(device, physicalUid))
            direct = std::make_unique<DirectMonitorProgram>(device, monitor, physicalUid, inputs,
                                                            clock.outputRate);
        else
            buffered = std::make_unique<MonitorProgram>(device, monitor, bus->source(), clock);
    }
    MonitorTiming timing() const noexcept {
        return clock;
    }
    uint32_t inputChannels() const noexcept {
        return direct ? direct->inputChannels() : 0;
    }
    MonitorStatus status() const noexcept {
        return direct ? direct->status() : buffered->status();
    }
    bool meters(AudioMeterSnapshot& value) const noexcept {
        return direct ? direct->meters(value) : buffered->meters(value);
    }
    bool equivalentDevice(const VirtualDeviceDefinition& device) const noexcept {
        return !direct || direct->equivalentDevice(device);
    }
    MonitorRenderResult render(const singz::AudioHostRenderBlock& block,
                               double measuredRate) noexcept {
        if (direct) {
            if (block.inputChannels != direct->inputChannels()) {
                // The next control poll reopens a stream after a committed mode change.
                silence(block);
                return {true, false, block.frames, 0};
            }
            if (!block.input || !block.inputTimestampValid || !block.inputTimestampHardware ||
                (block.discontinuity & singz::AudioHostDiscontinuityXRun)) {
                silence(block);
                return {false, false, block.frames, 0};
            }
            return direct->render({{block.input, block.inputChannels}, block.maximumFrames},
                                  {{block.output, block.outputChannels}, block.maximumFrames},
                                  block.frames, block.outputFrame);
        }
        return buffered->render(block.outputHostTimeNs, block.sampleRate,
                                {{block.output, block.outputChannels}, block.maximumFrames},
                                block.frames, block.outputFrame, measuredRate);
    }
};
using MonitorPrograms = RealtimePublication<PreparedMonitor>;

bool sameMonitor(const MonitorDefinition& left, const MonitorDefinition& right) {
    return left.id == right.id && left.deviceSelector == right.deviceSelector &&
           left.channels == right.channels && left.gain == right.gain &&
           left.enabled == right.enabled && left.muted == right.muted &&
           std::equal(left.connections.begin(), left.connections.end(), right.connections.begin(),
                      right.connections.end(), [](const auto& a, const auto& b) {
                          return a.sourceChannel == b.sourceChannel &&
                                 a.outputChannel == b.outputChannel && a.gain == b.gain;
                      });
}

struct MonitorSlot {
    VirtualDeviceDefinition device;
    const MonitorDefinition monitor;
    const std::shared_ptr<MonitorBus> bus;
    const uint32_t delay, quantum, periodReserve;
    const std::shared_ptr<MonitorPrograms> programs;
    MonitorSlot(const VirtualDeviceDefinition& definition, const MonitorDefinition& output,
                std::shared_ptr<MonitorBus> history, uint32_t lookback, double rate,
                uint32_t period, uint32_t blockFrames, uint32_t reservedPeriods,
                const std::string& physicalUid, uint32_t inputs,
                std::shared_ptr<MonitorPrograms> retained = {})
        : device(definition), monitor(output), bus(std::move(history)), delay(lookback),
          quantum(blockFrames), periodReserve(reservedPeriods),
          programs(retained ? retained : std::make_shared<MonitorPrograms>(2)) {
        if (!retained)
            configure(rate, period, physicalUid, inputs);
    }
    std::shared_ptr<MonitorPrograms> retain(const VirtualDeviceDefinition& definition,
                                            const MonitorDefinition& output,
                                            const std::shared_ptr<MonitorBus>& history,
                                            uint32_t lookback, uint32_t blockFrames) const {
        // The callback's only input is this shared internal-clock bus. Public
        // rate/latency/name and upstream graph edits do not change its DSP.
        auto program = programs->borrow();
        return program && program->equivalentDevice(definition) && device.id == definition.id &&
                       device.routing.outputChannels == definition.routing.outputChannels &&
                       bus == history && delay == lookback && quantum == blockFrames &&
                       sameMonitor(monitor, output)
                   ? programs
                   : nullptr;
    }
    uint32_t queue(double rate, uint32_t period) const {
        const auto minimum = static_cast<uint64_t>(std::ceil(double(periodReserve) * period *
                                                             bus->renderRate() / rate)) +
                             quantum;
        if (minimum > kCapacity / 2)
            throw std::runtime_error("Monitor period exceeds the bounded history capacity");
        return std::max(delay, static_cast<uint32_t>(minimum));
    }
    uint32_t captureChannels(const std::string& uid, uint32_t available) const {
        return std::min(available, directMonitorInputChannels(device, uid));
    }
    void configure(double rate, uint32_t period, const std::string& uid = {}, uint32_t inputs = 0) {
        const bool direct = inputs && directMonitorInputChannels(device, uid);
        auto update = programs->stage(std::make_unique<PreparedMonitor>(
            device, monitor, bus,
            MonitorTiming{bus->renderRate(), rate, direct ? 0 : queue(rate, period)}, uid, inputs));
        update->commit();
    }
    bool ready(double rate, uint32_t period, const std::string& uid = {}, uint32_t inputs = 0) {
        auto program = programs->borrow();
        const bool direct = inputs && directMonitorInputChannels(device, uid);
        return program && program->timing().outputRate == rate &&
               program->timing().delayFrames == (direct ? 0 : queue(rate, period)) &&
               program->inputChannels() == (direct ? inputs : 0);
    }
    void ensureFormat(double rate, uint32_t period, const std::string& uid, uint32_t inputs) {
        if (!ready(rate, period, uid, inputs))
            configure(rate, period, uid, inputs);
        programs->collect();
    }
};
struct MonitorEndpoint {
    const std::string device, monitor, selector;
    const std::unique_ptr<singz::AudioHost> host;
    const MonitorOutputs::InputPermission inputPermission;
    RealtimePublication<MonitorSlot> slots;
    GraphRecovery retry;
    std::atomic<bool> admitted{false}, callbackFault{false};
    std::atomic<uint64_t> callbackMs{0};
    std::atomic<uint64_t> timestampErrors{0}, discontinuities{0};
    uint64_t firstCallbackNs{0}, expectedOutputFrame{0};
    bool previousOutput{false};
    OutputClock outputClock;
    uint64_t clockGeneration{0};
    bool wanted{false}, running{false};
    bool reconfigureRequested{false};
    std::string uid, error;
    uint32_t physicalChannels{0}, inputChannels{0}, periodFrames{0};
    double rate{48000};
    uint64_t attempts{0}, startedAt{0};
    explicit MonitorEndpoint(std::string owner, const MonitorDefinition& definition,
                             std::unique_ptr<singz::AudioHost> backend,
                             MonitorOutputs::InputPermission permission)
        : device(std::move(owner)), monitor(definition.id), selector(definition.deviceSelector),
          host(std::move(backend)), inputPermission(std::move(permission)) {
        if (!host || ownOutput(selector))
            throw std::invalid_argument("Invalid or recursive physical monitor");
        retry.setDesired({{"output", {}}}, 0);
    }
    ~MonitorEndpoint() {
        close();
    }
    void close() noexcept {
        admitted.store(false);
        host->stop();
        outputClock.reset();
        clockGeneration = 0;
        running = false;
        physicalChannels = 0;
        inputChannels = 0;
        callbackMs.store(0);
        callbackFault.store(false);
        timestampErrors.store(0);
        discontinuities.store(0);
        firstCallbackNs = expectedOutputFrame = 0;
        previousOutput = false;
    }
    std::optional<singz::AudioHostDeviceInfo> describe() {
        const auto selected = selector == "default" ? host->enumerate().defaultOutputUid : selector;
        if (selected.empty() || ownOutput(selected)) {
            error = "Monitor output is unavailable or would create a routing cycle";
            return {};
        }
        auto info = host->describeOutputDevice(selected);
        if (!info || info->uid != selected || !info->outputChannels ||
            !validRate(info->nominalSampleRate)) {
            error = "Saved monitor output is unavailable; retrying";
            return {};
        }
        return info;
    }
    bool healthy(MonitorSlot& slot, uint64_t now) {
        reconfigureRequested = false;
        const auto status = host->status();
        const auto last = callbackMs.load();
        if (status.state != singz::AudioHostState::Running || callbackFault.load() ||
            status.format.sampleRate != rate || status.format.outputChannels != physicalChannels ||
            status.format.inputChannels != inputChannels ||
            status.format.nominalBufferFrames != periodFrames ||
            (now >= (last ? last : startedAt) && now - (last ? last : startedAt) >= 2000)) {
            error = "Monitor stream stopped or stalled; restoring its saved route";
            return false;
        }
        const auto info = describe();
        if (!info)
            return false;
        if (info->uid != uid || info->nominalSampleRate != rate ||
            std::min(info->outputChannels, slot.monitor.channels) != physicalChannels) {
            error = "Monitor output profile changed; restoring its saved route";
            return false;
        }
        const auto required = slot.captureChannels(info->uid, info->inputChannels);
        const auto desired = required && inputPermission() ? required : 0;
        const auto period = info->bufferFrames.preferredFrames;
        if (desired != inputChannels || (period && period != periodFrames)) {
            // Routing, permission and shared-buffer changes require a fresh stream format.
            // Reopen immediately instead of accumulating failure backoff.
            reconfigureRequested = true;
            return false;
        }
        return true;
    }
    bool open(MonitorSlot& slot, uint64_t now) {
        close();
        const auto info = describe();
        if (!info)
            return false;
        singz::AudioHostConfig config;
        config.outputDeviceUid = info->uid;
        const auto required = slot.captureChannels(info->uid, info->inputChannels);
        const auto capture = required && inputPermission() ? required : 0;
        if (capture) {
            config.inputDeviceUid = info->uid;
            for (uint32_t channel = 0; channel < capture; ++channel)
                config.inputChannels.push_back(channel);
        }
        for (uint32_t channel = 0; channel < std::min(info->outputChannels, slot.monitor.channels);
             ++channel)
            config.outputChannels.push_back(channel);
        // Shared device only. Existing consent is required before duplex capture;
        // neither path requests privacy access or changes a physical rate/period.
        auto result = host->open(config, render, this);
        if (!result.ok) {
            error = result.message.empty() ? "Monitor output did not open" : result.message;
            return false;
        }
        const auto& format = result.format;
        if (!validRate(format.sampleRate) || !format.maximumFrames ||
            format.maximumFrames > singz::kAudioHostMaxFrames || !format.nominalBufferFrames ||
            format.nominalBufferFrames > format.maximumFrames || format.inputChannels != capture ||
            format.outputChannels != config.outputChannels.size() || !format.float32Planar ||
            !format.outputClockMaster || format.accessMode != singz::AudioHostAccessMode::Shared) {
            error = "Monitor negotiated an unsupported output format";
            return false;
        }
        slot.ensureFormat(format.sampleRate, format.nominalBufferFrames, info->uid, capture);
        uid = info->uid;
        rate = format.sampleRate;
        periodFrames = format.nominalBufferFrames;
        physicalChannels = format.outputChannels;
        inputChannels = format.inputChannels;
        startedAt = now;
        result = host->start();
        if (!result.ok) {
            error = result.message.empty() ? "Monitor output did not start" : result.message;
            return false;
        }
        running = true;
        return true;
    }
    static bool render(void* context, const singz::AudioHostRenderBlock& block) noexcept {
        auto& self = *static_cast<MonitorEndpoint*>(context);
        if (!self.admitted.load()) {
            silence(block);
            return true;
        }
        self.callbackMs.store(block.callbackHostTimeNs / 1000000);
        if (!self.firstCallbackNs)
            self.firstCallbackNs = block.callbackHostTimeNs;
        if (block.callbackHostTimeNs >= self.firstCallbackNs &&
            block.callbackHostTimeNs - self.firstCallbackNs >= 2000000000ULL) {
            if (self.previousOutput && block.outputTimestampHardware &&
                block.outputFrame != self.expectedOutputFrame)
                self.timestampErrors.fetch_add(1);
            if (block.discontinuity != singz::AudioHostDiscontinuityNone)
                self.discontinuities.fetch_add(1);
        }
        self.expectedOutputFrame = block.outputFrame + block.frames;
        self.previousOutput = block.outputTimestampHardware;
        auto slot = self.slots.borrow();
        if (!slot) {
            silence(block);
            return true;
        }
        auto program = slot->programs->borrow();
        // A profile may change while a new scene is staged. Its preallocated
        // program is corrected on the next control poll without reopening a
        // healthy hardware stream or publishing uncommitted wiring.
        if (!program || program->timing().outputRate != block.sampleRate) {
            silence(block);
            return true;
        }
        if (!block.output || block.outputChannels > kMaximumRoutingChannels ||
            !block.outputTimestampValid || !block.outputHostTimeNs) {
            silence(block);
            self.callbackFault.store(true);
            return false;
        }
        double measuredRate = block.sampleRate;
        if (block.outputTimestampHardware) {
            measuredRate = self.outputClock.observe(
                block.outputFrame, block.outputHostTimeNs, block.frames, block.sampleRate,
                block.discontinuity != singz::AudioHostDiscontinuityNone ||
                    block.streamGeneration != self.clockGeneration);
        } else {
            // Callback arrival times include scheduler jitter and cannot
            // establish the physical sample clock.
            self.outputClock.reset();
        }
        self.clockGeneration = block.streamGeneration;
        const auto result = program->render(block, measuredRate);
        if (!result.validBlock)
            self.callbackFault.store(true);
        return result.validBlock;
    }
};
struct MonitorBinding {
    std::shared_ptr<MonitorEndpoint> endpoint;
    std::unique_ptr<RealtimePublication<MonitorSlot>::Update> prepared;
};
struct ActiveMonitoringSink final : AudioSink {
    const void* owner;
    std::shared_ptr<MonitoringSink> output;
    std::vector<MonitorBinding> bindings;
    bool activated{false};
    ActiveMonitoringSink(const void* registry, std::shared_ptr<MonitoringSink> sink)
        : owner(registry), output(std::move(sink)) {}
    void activate() noexcept override {
        if (!activated) {
            output->activate();
            for (auto& binding : bindings)
                binding.prepared->commit();
            activated = true;
        }
    }
    uint32_t channels() const noexcept override {
        return output->channels();
    }
    bool requested() const noexcept override {
        return output->requested();
    }
    bool publish(uint64_t first, uint32_t rate, RoutingInput input,
                 uint32_t frames) noexcept override {
        return output->publish(first, rate, input, frames);
    }
    void idle() noexcept override {
        output->idle();
    }
};
} // namespace

struct MonitorOutputs::Impl {
    const HostFactory factory;
    const Clock clock;
    const InputPermission inputPermission;
    const uint32_t delay, quantum, periodReserve;
    MonitorBuses buses;
    std::vector<std::weak_ptr<MonitorEndpoint>> endpoints;
    Impl(uint32_t rate, uint32_t lookback, HostFactory create, Clock time, uint32_t blockFrames,
         uint32_t reservedPeriods, InputPermission permission)
        : factory(create ? std::move(create) : HostFactory{[] {
              return std::make_unique<singz::AudioHost>();
          }}),
          clock(std::move(time)),
          inputPermission(permission ? std::move(permission) : InputPermission{[] {
              return microphonePermission() == MicrophonePermission::Authorized;
          }}),
          delay(lookback), quantum(blockFrames), periodReserve(reservedPeriods), buses(rate) {
        if (delay > kCapacity / 2 || (periodReserve != 1 && periodReserve != 2) ||
            (quantum != 32 && quantum != 64 && quantum != 128 && quantum != kBlock))
            throw std::invalid_argument("Invalid monitor lookback");
    }
};
MonitorOutputs::MonitorOutputs(uint32_t renderRate, uint32_t delayFrames, HostFactory factory,
                               Clock clock, uint32_t renderBlockFrames, uint32_t periodReserve,
                               InputPermission permission)
    : impl_(std::make_unique<Impl>(renderRate, delayFrames, std::move(factory), std::move(clock),
                                   renderBlockFrames, periodReserve, std::move(permission))) {}
MonitorOutputs::~MonitorOutputs() {
    stop();
}
std::shared_ptr<AudioSink> MonitorOutputs::decorate(const VirtualDeviceDefinition& device,
                                                    std::shared_ptr<AudioSink> publicSink) {
    auto& self = *impl_;
    std::erase_if(self.endpoints, [](const auto& endpoint) {
        return endpoint.expired();
    });
    if (!device.routing.enabled ||
        std::none_of(device.monitors.begin(), device.monitors.end(), [](const auto& monitor) {
            return monitor.enabled;
        })) {
        if (!publicSink || !device.routing.outputChannels ||
            device.routing.outputChannels > kMaximumRoutingChannels ||
            publicSink->channels() != device.routing.outputChannels)
            throw std::invalid_argument("Monitor output does not match its public device");
        // No physical reader can consume this scene's monitor bus. Preparing
        // the public sink neither allocates history nor changes an old active
        // monitor; poll() releases old endpoints only after the scene commits.
        return publicSink;
    }
    auto sink = std::make_shared<ActiveMonitoringSink>(
        &self, self.buses.decorate(device, std::move(publicSink)));
    if (!device.routing.enabled)
        return sink;
    for (const auto& monitor : device.monitors) {
        if (!monitor.enabled)
            continue;
        std::shared_ptr<MonitorEndpoint> endpoint;
        for (const auto& weak : self.endpoints)
            if (auto candidate = weak.lock())
                if (candidate->device == device.id && candidate->monitor == monitor.id &&
                    candidate->selector == monitor.deviceSelector) {
                    endpoint = std::move(candidate);
                    break;
                }
        if (!endpoint) {
            auto host = self.factory();
            if (!host)
                throw std::runtime_error("Monitor factory returned no audio host");
            endpoint = std::make_shared<MonitorEndpoint>(device.id, monitor, std::move(host),
                                                         self.inputPermission);
            self.endpoints.push_back(endpoint);
        }
        endpoint->slots.checkStageCapacity();
        std::shared_ptr<MonitorPrograms> retained;
        if (endpoint->running && endpoint->wanted) {
            auto current = endpoint->slots.borrow();
            if (current)
                retained =
                    current->retain(device, monitor, sink->output->bus(), self.delay, self.quantum);
        }
        auto prepared = endpoint->slots.stage(std::make_unique<MonitorSlot>(
            device, monitor, sink->output->bus(), self.delay, endpoint->rate,
            endpoint->periodFrames, self.quantum, self.periodReserve, endpoint->uid,
            endpoint->inputChannels, std::move(retained)));
        sink->bindings.push_back({std::move(endpoint), std::move(prepared)});
    }
    return sink;
}
void MonitorOutputs::poll(const AudioScene* committed, uint64_t now) {
    auto& self = *impl_;
    auto observedNow = now;
    const auto currentTime = [&] {
        if (self.clock)
            observedNow = std::max(observedNow, self.clock());
        return observedNow;
    };
    for (const auto& weak : self.endpoints)
        if (const auto endpoint = weak.lock())
            endpoint->wanted = false;
    if (committed)
        for (size_t device = 0; device < committed->document().devices.size(); ++device)
            if (const auto* sink =
                    dynamic_cast<const ActiveMonitoringSink*>(committed->sink(device)))
                if (sink->owner == &self && sink->activated)
                    for (const auto& binding : sink->bindings)
                        binding.endpoint->wanted = true;
    for (const auto& weak : self.endpoints) {
        const auto endpoint = weak.lock();
        if (!endpoint)
            continue;
        auto& state = *endpoint;
        state.slots.collect();
        if (!state.wanted) {
            if (state.running)
                state.close();
            state.retry.restart(currentTime());
            state.error.clear();
            continue;
        }
        auto slot = state.slots.borrow();
        if (!slot)
            continue;
        if (state.retry.ready("output")) {
            bool healthy = false;
            try {
                healthy = state.healthy(*slot.get(), currentTime());
            } catch (const std::exception& error) {
                state.error = error.what();
            }
            if (!healthy) {
                const auto requested = state.reconfigureRequested;
                state.close();
                if (requested)
                    state.retry.restart(currentTime());
                else
                    state.retry.lost("output", currentTime());
            } else {
                try {
                    slot->ensureFormat(state.rate, state.periodFrames, state.uid,
                                       state.inputChannels);
                } catch (const std::exception& error) {
                    state.error = error.what();
                }
            }
        }
        for (const auto& attempt : state.retry.attempts(currentTime())) {
            bool opened = false;
            state.error.clear();
            ++state.attempts;
            try {
                opened = state.open(*slot.get(), currentTime());
            } catch (const std::exception& error) {
                state.error = error.what();
            }
            const auto completed = currentTime();
            if (!state.retry.complete(attempt, opened, completed)) {
                opened = false;
                state.error = "Monitor acquisition expired; retrying its saved route";
            }
            if (!opened)
                state.close();
            else {
                // Give a newly admitted stream its own first-callback grace.
                // Host preparation time is not missing callback delivery.
                state.startedAt = completed;
                state.admitted.store(true);
                state.error.clear();
            }
        }
    }
    std::erase_if(self.endpoints, [](const auto& endpoint) {
        return endpoint.expired();
    });
}
void MonitorOutputs::retryNow(uint64_t now) noexcept {
    for (const auto& weak : impl_->endpoints)
        if (const auto endpoint = weak.lock())
            endpoint->retry.retryNow(now);
}
void MonitorOutputs::stop() noexcept {
    for (const auto& weak : impl_->endpoints)
        if (const auto endpoint = weak.lock()) {
            endpoint->close();
            endpoint->wanted = false;
            endpoint->retry.restart(0);
        }
}
bool MonitorOutputs::restored() const noexcept {
    for (const auto& weak : impl_->endpoints)
        if (const auto endpoint = weak.lock())
            if (endpoint->wanted) {
                auto slot = endpoint->slots.borrow();
                if (!endpoint->retry.restored() || !endpoint->running || !slot ||
                    endpoint->callbackFault.load() ||
                    !slot->ready(endpoint->rate, endpoint->periodFrames, endpoint->uid,
                                 endpoint->inputChannels) ||
                    endpoint->physicalChannels < slot->monitor.channels)
                    return false;
            }
    return true;
}
std::vector<MonitorConnectionStatus> MonitorOutputs::status() const {
    std::vector<MonitorConnectionStatus> result;
    for (const auto& weak : impl_->endpoints)
        if (const auto endpoint = weak.lock()) {
            auto slot = endpoint->slots.borrow();
            MonitorStatus audio;
            uint32_t queueFrames = 0;
            bool directClock = false;
            if (slot) {
                auto program = slot->programs->borrow();
                if (program) {
                    audio = program->status();
                    queueFrames = program->timing().delayFrames;
                    directClock = endpoint->running && program->inputChannels() > 0 &&
                                  program->inputChannels() == endpoint->inputChannels;
                }
            }
            result.push_back({endpoint->device,
                              endpoint->monitor,
                              endpoint->selector,
                              endpoint->uid,
                              endpoint->wanted,
                              endpoint->running,
                              endpoint->running && !endpoint->callbackFault.load() && slot &&
                                  slot->ready(endpoint->rate, endpoint->periodFrames, endpoint->uid,
                                              endpoint->inputChannels) &&
                                  endpoint->physicalChannels >= slot->monitor.channels,
                              slot ? slot->monitor.channels : 0,
                              endpoint->physicalChannels,
                              queueFrames,
                              endpoint->rate,
                              endpoint->attempts,
                              endpoint->error,
                              audio,
                              endpoint->callbackMs.load(),
                              directClock,
                              endpoint->timestampErrors.load(),
                              endpoint->discontinuities.load(),
                              endpoint->host->status().xruns,
                              endpoint->periodFrames});
        }
    return result;
}
bool MonitorOutputs::meters(const std::string& device, const std::string& monitor,
                            AudioMeterSnapshot& output) const noexcept {
    for (const auto& weak : impl_->endpoints)
        if (const auto endpoint = weak.lock())
            if (endpoint->wanted && endpoint->device == device && endpoint->monitor == monitor) {
                auto slot = endpoint->slots.borrow();
                if (slot) {
                    auto program = slot->programs->borrow();
                    return program && program->meters(output);
                }
            }
    return false;
}
} // namespace loopz::macos
