#include <loopz/macos/engine_runtime.hpp>
#include <loopz/macos/capture.hpp>
#include <loopz/timeline.hpp>

#include <cassert>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <functional>
#include <source_location>
#include <thread>
#include <unistd.h>

namespace {
using namespace loopz;
using namespace loopz::macos;
using namespace singz;
class Link final : public DriverConfigurationLink {
  public:
    uint64_t incarnation{1}, revision{0}, submissions{0};
    bool commit{true}, publicClients{false};
    uint32_t slowPollMs{0};
    std::vector<DriverRateRequest> rates;
    bool failRateAck{false};
    uint32_t acknowledgements{0};
    std::function<void()> beforeRateAck, duringRateQuery;
    std::vector<DeviceTransport> mappings;
    DriverConfigurationResult outcome{0, DriverConfigurationPhase::Committed, 0};
    uint64_t session() const noexcept override {
        return incarnation;
    }
    void poll(uint64_t) override {
        if (slowPollMs)
            std::this_thread::sleep_for(std::chrono::milliseconds(slowPollMs));
        for (auto& device : mappings) {
            device.region->header().driverOnline.store(incarnation && publicClients);
            device.region->header().driverHeartbeatNs.store(hostNowNs());
        }
    }
    int32_t submit(std::span<const DeviceTransport> devices, uint64_t next) override {
        ++submissions;
        revision = next;
        mappings.assign(devices.begin(), devices.end());
        outcome = {next,
                   commit ? DriverConfigurationPhase::Committed : DriverConfigurationPhase::Pending,
                   0};
        return 0;
    }
    DriverConfigurationResult status(uint64_t) override {
        return outcome;
    }
    std::vector<DriverRateRequest> rateRequests() override {
        const auto result = rates;
        if (duringRateQuery)
            duringRateQuery();
        return result;
    }
    void acknowledgeRates(std::span<const DriverRateRequest> requests) override {
        ++acknowledgements;
        if (beforeRateAck)
            beforeRateAck();
        if (failRateAck)
            throw std::runtime_error("Lost rate acknowledgement");
        std::erase_if(rates, [&](const auto& pending) {
            return std::any_of(requests.begin(), requests.end(), [&](const auto& accepted) {
                return pending.device == accepted.device && pending.origin == accepted.origin &&
                       pending.sequence == accepted.sequence &&
                       pending.sampleRate == accepted.sampleRate;
            });
        });
    }
    int32_t cancel(uint64_t) override {
        outcome.phase = DriverConfigurationPhase::Aborted;
        return 0;
    }
    void invalidate() noexcept override {
        incarnation = 0;
    }
};
class Input final : public SourceEndpoint {
  public:
    const uint32_t count;
    const float value;
    bool lost{false}, opened{false};
    uint32_t opens{0}, closes{0}, policyUpdates{0};
    SourceOpenPolicy policy{};
    std::vector<InputTimingFacts> timing;
    std::vector<InputTimingFacts> inputTiming() const override {
        return timing;
    }
    void clearInputTiming() noexcept override {
        timing.clear();
    }
    std::vector<InputTimingFacts> retainedInputTiming() const override {
        return timing;
    }
    Input(uint32_t channels, float amplitude) : count(channels), value(amplitude) {}
    uint32_t channels() const noexcept override {
        return count;
    }
    bool open(SourceOpenPolicy requested, std::string&) override {
        ++opens;
        policy = requested;
        opened = !lost;
        return opened;
    }
    bool updatePolicy(SourceOpenPolicy requested) noexcept override {
        if (!opened || lost || !requested.requiredChannels || requested.requiredChannels > count)
            return false;
        policy = requested;
        ++policyUpdates;
        return true;
    }
    void close() noexcept override {
        if (opened)
            ++closes;
        opened = false;
    }
    bool healthy(std::string&) override {
        return opened && !lost;
    }
    AudioReadResult read(uint64_t, double, RoutingOutput output, uint32_t frames,
                         AudioReadCursor&) const noexcept override {
        for (auto* plane : output.channels)
            std::fill_n(plane, frames, value);
        return {0, true};
    }
};
struct Output {
    AudioHostRender callback{nullptr};
    void* context{nullptr};
    AudioHostFormat format;
    AudioHostState state{AudioHostState::Closed};
    uint32_t opens{0}, stops{0};
    float sample{0};
    void fire() {
        if (state != AudioHostState::Running)
            return;
        std::array<std::array<float, 64>, 2> samples{};
        std::array<float*, 2> planes{samples[0].data(), samples[1].data()};
        AudioHostRenderBlock block;
        block.output = planes.data();
        block.outputChannels = 2;
        block.frames = block.maximumFrames = 64;
        block.sampleRate = format.sampleRate;
        block.outputFrame = hostFrame(hostNowNs(), format.sampleRate);
        block.outputHostTimeNs = block.callbackHostTimeNs = hostNowNs();
        block.outputTimestampValid = block.outputTimestampHardware = true;
        assert(callback(context, block));
        sample = samples[0][0];
    }
};
class Host final : public AudioHostBackend {
  public:
    const std::shared_ptr<Output> output;
    explicit Host(std::shared_ptr<Output> state) : output(std::move(state)) {}
    AudioHostInventory enumerate() const override {
        AudioHostDeviceInfo device;
        device.uid = "headphones";
        device.outputChannels = 2;
        device.nominalSampleRate = 48000;
        device.direction = AudioHostEndpointDirection::Output;
        return {{device}, {}, device.uid};
    }
    AudioHostResult open(const AudioHostConfig& config, AudioHostRender callback,
                         void* context) override {
        assert(config.inputChannels.empty() && config.inputDeviceUid.empty() &&
               config.requestedBufferFrames == 0);
        ++output->opens;
        output->callback = callback;
        output->context = context;
        output->format = {48000, 8192, 64, 0, 2, true, true, AudioHostAccessMode::Shared};
        output->state = AudioHostState::Open;
        return {true, AudioHostError::None, output->state, output->format, {}, {}};
    }
    AudioHostResult start() override {
        output->state = AudioHostState::Running;
        return {true, AudioHostError::None, output->state, output->format, {}, {}};
    }
    void stop() noexcept override {
        if (output->state != AudioHostState::Closed)
            ++output->stops;
        output->state = AudioHostState::Closed;
    }
    AudioHostStatus status() const noexcept override {
        AudioHostStatus status;
        status.state = output->state;
        status.format = output->format;
        return status;
    }
};
RouteDocument document() {
    VirtualDeviceDefinition upstream;
    upstream.id = upstream.name = "upstream";
    upstream.routing.sources = {{"app", 2, 1, true, false, defaultChannelConnections(2, 2)},
                                {"input", 2, 1, true, false, defaultChannelConnections(2, 2)}};
    upstream.bindings = {{"app", SourceKind::Application, "app"},
                         {"input", SourceKind::InputDevice, "input"}};
    VirtualDeviceDefinition downstream;
    downstream.id = downstream.name = "downstream";
    downstream.routing.sources = {{"virtual", 2, 1, true, false, defaultChannelConnections(2, 2)},
                                  {"input", 2, 0.5f, true, false, defaultChannelConnections(2, 2)}};
    downstream.bindings = {{"virtual", SourceKind::VirtualDevice, "upstream"},
                           {"input", SourceKind::InputDevice, "input"}};
    downstream.monitors = {
        {"monitor", "headphones", 2, 0.5f, true, false, defaultChannelConnections(2, 2)}};
    return {1, {downstream, upstream}};
}
struct Fixture {
    Link link;
    std::map<std::string, std::shared_ptr<Input>> sources;
    std::shared_ptr<Output> output = std::make_shared<Output>();
    EngineRuntime runtime;
    explicit Fixture(EngineRuntimeOptions options = {})
        : runtime(
              link, std::move(options),
              [&](SourceKind, const auto& selector, uint32_t channels) {
                  auto source = std::make_shared<Input>(channels, selector == "input" ? 0.2f
                                                                  : selector == "app" ? 0.1f
                                                                                      : 0.05f);
                  sources[selector] = source;
                  return source;
              },
              [&] {
                  return std::make_unique<AudioHost>(std::make_unique<Host>(output));
              }) {}
    void tick() {
        runtime.poll();
        output->fire();
    }
    template <typename Predicate>
    void until(Predicate predicate,
               const std::source_location location = std::source_location::current()) {
        const auto deadline = hostNowNs() + 10000000000ULL;
        while (hostNowNs() < deadline) {
            tick();
            if (predicate())
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto status = runtime.status();
        std::fprintf(stderr, "Runtime timeout at line %u: cfg=%u, render=%d, doc=%s, control=%s\n",
                     location.line(), unsigned(status.configuration.phase), status.render.running,
                     status.documentError.c_str(), status.controlError.c_str());
        assert(false);
    }
    void sample(float expected) {
        until([&] {
            return std::fabs(output->sample - expected) < 0.0001f;
        });
    }
};
void disabledDevicesSuspendRenderingAndResumeOnEnable() {
    Fixture fixture;
    auto desired = document();
    desired.devices.resize(1);
    auto& device = desired.devices.front();
    device.routing.sources = {{"input", 2, 1, true, false, defaultChannelConnections(2, 2)}};
    device.bindings = {{"input", SourceKind::InputDevice, "input"}};
    device.routing.enabled = false;
    auto& engine = fixture.runtime;
    engine.start();
    engine.setDesired(desired);
    fixture.until([&] {
        return engine.status().restored;
    });
    assert(!engine.status().render.running && !fixture.output->opens);

    // Editor preview and recovery notifications cannot wake disabled audio.
    engine.setEditorPreview(true);
    engine.retryNow();
    const auto idleBlocks = engine.status().render.blocks;
    for (unsigned poll = 0; poll < 20; ++poll) {
        fixture.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(engine.status().restored && !engine.status().render.running &&
           engine.status().render.blocks == idleBlocks && !fixture.output->opens);

    device.routing.enabled = true;
    engine.setDesired(desired);
    fixture.until([&] {
        return engine.status().restored;
    });
    fixture.sample(0.1f);
    const auto input = fixture.sources.at("input");
    assert(input->opened && fixture.output->opens == 1 && engine.status().render.running);

    // A disabled neighbour does not suspend an enabled device's renderer.
    auto neighbour = device;
    neighbour.id = neighbour.name = "disabled-neighbour";
    neighbour.routing.enabled = false;
    desired.devices.push_back(neighbour);
    engine.setDesired(desired);
    fixture.until([&] {
        return engine.status().restored;
    });
    assert(engine.status().render.running);
    fixture.sample(0.1f);

    desired.devices.front().routing.enabled = false;
    engine.setDesired(desired);
    fixture.until([&] {
        return engine.status().restored;
    });
    assert(!engine.status().render.running && !input->opened &&
           fixture.output->state == AudioHostState::Closed);
    const auto stoppedBlocks = engine.status().render.blocks;
    for (unsigned poll = 0; poll < 20; ++poll) {
        fixture.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(engine.status().render.blocks == stoppedBlocks);

    desired.devices.front().routing.enabled = true;
    fixture.output->sample = 0;
    engine.setDesired(desired);
    fixture.until([&] {
        return engine.status().restored;
    });
    fixture.sample(0.1f);
    assert(engine.status().render.running && fixture.sources.at("input")->opened);

    engine.setDesired({kRouteDocumentVersion, {}});
    fixture.until([&] {
        return engine.status().restored;
    });
    assert(!engine.status().render.running && !input->opened);
}

void editorPreviewWithoutClientsOrWires() {
    Fixture fixture;
    auto desired = document();
    desired.devices.resize(1);
    auto& device = desired.devices[0];
    device.routing.sources = {{"input", 8, 1, true, false, {}}};
    device.bindings = {{"input", SourceKind::InputDevice, "input"}};
    device.monitors.clear();
    auto& engine = fixture.runtime;
    engine.start();
    engine.setDesired(desired);
    fixture.until([&] {
        return engine.status().restored;
    });
    auto input = fixture.sources.at("input");
    assert(!input->opened && !input->opens && !fixture.output->opens);
    engine.setEditorPreview(true);
    fixture.until([&] {
        auto scene = engine.observe();
        AudioMeterSnapshot meter;
        return input->opened && scene && scene->sourceMeters(0, 0, meter) && meter.frames &&
               meter.channels == 8 && std::fabs(meter.peak[7] - 0.2f) < 0.0001f;
    });
    assert(input->opens == 1 && !fixture.output->opens && !fixture.link.publicClients);
    assert(engine.observe()->document().devices[0].routing.sources[0].connections.empty());
    fixture.link.publicClients = true;
    engine.setEditorPreview(false);
    for (unsigned i = 0; i < 10; ++i) {
        fixture.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(input->opened && input->opens == 1 && !input->closes);
    fixture.link.publicClients = false;
    fixture.until([&] {
        return !input->opened;
    });
    assert(input->closes == 1);
}
void physicalInputBudgetDoesNotShortenApplicationHistory() {
    EngineRuntimeOptions options;
    options.scene.blockFrames = 32;
    options.renderLeadFrames = 32;
    options.inputFilter = ResamplingFilter::MinimumPhase;
    options.inputDelayFrames = 192;
    Fixture fixture(options);
    auto desired = document();
    for (auto& device : desired.devices)
        device.monitors.clear();
    auto microphone = desired.devices.back();
    microphone.id = microphone.name = "microphone-only";
    microphone.bindings.erase(microphone.bindings.begin());
    microphone.routing.sources.erase(microphone.routing.sources.begin());
    desired.devices.push_back(microphone);
    fixture.link.publicClients = true;
    fixture.runtime.start();
    fixture.runtime.setDesired(desired);
    fixture.until([&] {
        return fixture.runtime.status().restored && fixture.sources.size() == 2 &&
               fixture.sources.at("app")->opened && fixture.sources.at("input")->opened;
    });
    assert(fixture.runtime.devices().size() == 3);
    for (const auto& transport : fixture.runtime.devices())
        assert(transport.inputLatencyFrames == (transport.id == "microphone-only" ? 193 : 1025));
    assert(fixture.sources.at("input")->opens == 1 && fixture.sources.at("app")->opens == 1);
    assert(options.scene.sourceDelayFrames == 1024);
    options.inputDelayFrames = kCapacity / 2 + 1;
    bool rejected = false;
    try {
        Fixture invalid(options);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}
void applicationTimingReplansWithoutReopeningCaptures() {
    EngineRuntimeOptions options;
    options.scene.blockFrames = options.renderLeadFrames = 32;
    options.inputDelayFrames = 384;
    assert(options.automaticApplicationTiming); // Normal sessions use verified timing.
    Fixture fixture(options);
    auto desired = document();
    for (auto& device : desired.devices)
        device.monitors.clear();
    fixture.link.publicClients = true;
    auto& runtime = fixture.runtime;
    runtime.start();
    runtime.setDesired(desired);
    fixture.until([&] {
        return runtime.status().restored && fixture.sources.size() == 2 &&
               fixture.sources.at("app")->opened && fixture.sources.at("input")->opened;
    });
    const auto application = fixture.sources.at("app"), input = fixture.sources.at("input");
    assert(runtime.observe()->deviceLatencyFrames(0) == 1024);
    const auto submissions = fixture.link.submissions;
    application->timing = {{48000, 128, 2}};
    for (unsigned tick = 0; tick < 8; ++tick)
        fixture.tick();
    assert(fixture.link.submissions == submissions);
    assert(runtime.observe()->deviceLatencyFrames(0) == 1024);
    // Preview keeps capture warm, but must not pin timing once real clients leave.
    runtime.setEditorPreview(true);
    fixture.link.publicClients = false;
    fixture.link.commit = false;
    fixture.until([&] {
        return fixture.link.submissions > submissions;
    });
    // Metadata planning never publishes an unacknowledged graph or reopens
    // the producer that supplied these verified facts.
    assert(runtime.observe()->deviceLatencyFrames(0) == 1024);
    assert(application->opens == 1 && input->opens == 1);
    assert(!application->policy.muteOriginal && application->policyUpdates == 1);
    fixture.link.outcome = {fixture.link.revision, DriverConfigurationPhase::Committed, 0};
    fixture.link.commit = true;
    fixture.until([&] {
        return runtime.status().restored && runtime.observe()->deviceLatencyFrames(0) == 384;
    });
    for (const auto& transport : runtime.devices())
        assert(transport.inputLatencyFrames == 385);
    fixture.link.publicClients = true;
    fixture.tick();
    application->timing = {{48000, 1024, 0}};
    fixture.until([&] {
        return runtime.status().restored && runtime.observe()->deviceLatencyFrames(0) == 1248;
    });
    application->timing.clear();
    application->lost = true;
    fixture.until([&] {
        return !application->opened;
    });
    assert(runtime.observe()->deviceLatencyFrames(0) == 1248);
    application->lost = false;
    runtime.retryNow();
    fixture.until([&] {
        return application->opened && runtime.status().restored;
    });
    assert(application->opens == 2 && input->opens == 1);
    application->timing = {{48000, 128, 2}};
    for (unsigned tick = 0; tick < 8; ++tick)
        fixture.tick();
    assert(runtime.observe()->deviceLatencyFrames(0) == 1248);
    fixture.link.publicClients = false;
    fixture.until([&] {
        return runtime.status().restored && runtime.observe()->deviceLatencyFrames(0) == 384;
    });
    assert(application->opens == 2 && input->opens == 1);
    // A clean idle close retains verified timing even without editor preview.
    runtime.setEditorPreview(false);
    fixture.until([&] {
        return !application->opened && !input->opened;
    });
    for (unsigned tick = 0; tick < 8; ++tick)
        fixture.tick();
    assert(runtime.observe()->deviceLatencyFrames(0) == 384);
    fixture.link.publicClients = true;
    fixture.until([&] {
        return application->opened && input->opened && runtime.status().restored;
    });
    assert(runtime.observe()->deviceLatencyFrames(0) == 384);
    assert(application->opens == 3 && input->opens == 2);
    application->lost = true;
    fixture.until([&] {
        return !application->opened;
    });
    for (unsigned tick = 0; tick < 8; ++tick)
        fixture.tick();
    assert(runtime.observe()->deviceLatencyFrames(0) == 384);
    application->lost = false;
    runtime.retryNow();
    fixture.until([&] {
        return application->opened && runtime.status().restored;
    });
    assert(runtime.observe()->deviceLatencyFrames(0) == 384);
    application->timing = {{48000, 128, UINT32_MAX}};
    fixture.until([&] {
        return !application->opened;
    });
    application->timing.clear();
    runtime.retryNow();
    fixture.until([&] {
        return application->opened && runtime.status().restored &&
               runtime.observe()->deviceLatencyFrames(0) == 1024;
    });
    assert(application->opens == 5 && input->opens == 2);
}
void completeLifecycleAndBlockingControl() {
    Fixture fixture;
    auto& engine = fixture.runtime;
    engine.start();
    fixture.tick();
    assert(!engine.status().restored && !engine.status().render.running &&
           fixture.link.submissions == 0);
    auto desired = document();
    engine.setDesired(desired);
    fixture.until([&] {
        return engine.status().restored;
    });
    fixture.sample(0.2f);
    assert(fixture.sources.size() == 2 && fixture.sources.at("input")->opens == 1 &&
           fixture.output->opens == 1);
    const auto app = fixture.sources.at("app"), input = fixture.sources.at("input");
    auto before = engine.status().render.frames;
    fixture.link.slowPollMs = 150;
    fixture.tick();
    fixture.link.slowPollMs = 0;
    assert(engine.status().render.frames > before);
    input->lost = true;
    fixture.until([&] {
        return !engine.status().restored && !input->opened;
    });
    fixture.sample(0.05f);
    assert(app->opens == 1 && fixture.output->opens == 1);
    input->lost = false;
    engine.retryNow();
    fixture.until([&] {
        return engine.status().restored;
    });
    fixture.sample(0.2f);
    assert(input->opens == 2 && app->opens == 1 && fixture.output->opens == 1);
    fixture.link.incarnation = 0;
    for (const auto& device : fixture.link.mappings)
        device.region->retire();
    fixture.tick();
    assert(engine.status().configuration.phase == RestorationPhase::WaitingForDriver);
    const auto until = hostNowNs() + 80000000;
    while (hostNowNs() < until) {
        fixture.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    fixture.sample(0.2f); // Retired public capture cannot suppress the monitor mix.
    assert(input->opens == 2 && app->opens == 1 && fixture.output->opens == 1);
    fixture.link.incarnation = 2;
    engine.retryNow();
    fixture.until([&] {
        return engine.status().restored;
    });
    assert(engine.observe()->document().devices.size() == 2 && fixture.output->opens == 1);
    desired.devices[1].routing.sources.push_back(
        {"second", 2, 1, true, false, defaultChannelConnections(2, 2)});
    desired.devices[1].bindings.push_back({"second", SourceKind::Application, "second"});
    fixture.link.commit = false;
    engine.setDesired(desired);
    fixture.until([&] {
        return engine.status().configuration.phase == RestorationPhase::Applying;
    });
    assert(fixture.sources.at("second")->opens == 0);
    fixture.sample(0.2f);
    fixture.link.outcome.phase = DriverConfigurationPhase::Committed;
    fixture.until([&] {
        return engine.status().restored;
    });
    fixture.sample(0.225f);
    assert(fixture.sources.at("second")->opens == 1 && app->opens == 1 &&
           fixture.output->opens == 1);
    const auto generation = engine.status().configuration.desiredGeneration;
    engine.stop();
    before = engine.status().render.frames;
    assert(!engine.status().started && !engine.status().render.running && !input->opened &&
           !app->opened);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(engine.status().render.frames == before);
    engine.start();
    fixture.until([&] {
        return engine.status().restored;
    });
    fixture.sample(0.225f);
    assert(engine.status().configuration.desiredGeneration == generation &&
           fixture.output->opens == 2);
}
void smallRenderQuantumUsesMatchingMonitorQueue(uint32_t quantum) {
    EngineRuntimeOptions options;
    options.scene.blockFrames = quantum;
    options.renderLeadFrames = 32;
    options.monitorDelayFrames = 0;
    Fixture fixture(options);
    fixture.runtime.setDesired(document());
    fixture.runtime.start();
    fixture.until([&] {
        return fixture.runtime.status().restored;
    });
    fixture.sample(0.2f);
    assert(fixture.runtime.status().monitors[0].queueFrames == 128 + quantum);
}
void restorationWaitsForCompensationHistory() {
    EngineRuntimeOptions options;
    options.scene.blockFrames = 64;
    options.scene.sourceDelayFrames = kCapacity / 2;
    options.scene.passThroughDelayFrames = 0;
    Fixture fixture(options);
    auto desired = document();
    auto& upstream = desired.devices[1];
    upstream.routing.sources = {{"pass", 2, 1, true, false, defaultChannelConnections(2, 2)}};
    upstream.bindings = {{"pass", SourceKind::PassThrough, ""}};
    fixture.link.publicClients = true;
    fixture.runtime.setDesired(desired);
    fixture.runtime.start();
    fixture.until([&] {
        auto scene = fixture.runtime.observe();
        return scene && scene->hasRendered() && !scene->timingReady();
    });
    assert(!fixture.runtime.status().restored);
    fixture.until([&] {
        return fixture.runtime.status().restored;
    });
    assert(fixture.runtime.observe()->timingReady());
    fixture.runtime.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto restartedAt = hostNowNs();
    fixture.runtime.start();
    fixture.until([&] {
        const auto status = fixture.runtime.status();
        if (status.render.lastBlockHostNs < restartedAt)
            assert(!status.restored);
        auto scene = fixture.runtime.observe();
        return status.render.lastBlockHostNs >= restartedAt && scene && !scene->timingReady();
    });
    assert(!fixture.runtime.status().restored);
    fixture.until([&] {
        return fixture.runtime.status().restored;
    });
    assert(fixture.runtime.status().render.lastBlockHostNs >= restartedAt);
}
struct TemporaryFolder {
    std::filesystem::path path;
    TemporaryFolder() {
        char pattern[] = "/tmp/loopz-runtime-XXXXXX";
        auto* value = mkdtemp(pattern);
        assert(value);
        path = value;
    }
    ~TemporaryFolder() {
        std::filesystem::remove_all(path);
    }
};
DriverRateRequest rateRequest(uint64_t sequence = 1, uint32_t rate = 44100) {
    return {"saved", sequence, rate, "01234567-89ab-cdef-0123-456789abcdef"};
}
void durableClientRateRequests() {
    TemporaryFolder folder;
    EngineRuntimeOptions options;
    options.documentPath = folder.path / "rates.json";
    RouteDocument initial;
    initial.version = 1;
    VirtualDeviceDefinition device;
    device.id = device.name = "saved";
    initial.devices.push_back(device);
    saveRouteDocumentFile(options.documentPath, initial);
    const auto request = rateRequest();
    {
        Fixture fixture(options);
        fixture.runtime.start();
        fixture.until([&] {
            return fixture.runtime.status().restored;
        });
        fixture.link.rates = {request};
        fixture.link.failRateAck = true;
        fixture.link.beforeRateAck = [&] {
            const auto saved = loadRouteDocumentFile(options.documentPath).document;
            assert(saved.version == kRouteDocumentVersion && saved.devices[0].sampleRate == 44100 &&
                   saved.devices[0].formatRequestReceipt == driverRateReceipt(request));
        };
        fixture.runtime.retryNow();
        fixture.until([&] {
            return fixture.link.acknowledgements != 0 &&
                   !fixture.runtime.status().controlError.empty();
        });
        fixture.until([&] {
            return fixture.runtime.status().restored;
        });
        // A newer editor choice must survive a lost ACK and helper reconstruction.
        initial.devices[0].sampleRate = 48000;
        fixture.runtime.setDesired(initial, true);
        assert(
            loadRouteDocumentFile(options.documentPath).document.devices[0].formatRequestReceipt ==
            driverRateReceipt(request));
    }
    Fixture restarted(options);
    auto& engine = restarted.runtime;
    auto& link = restarted.link;
    link.rates = {request};
    link.beforeRateAck = [&] {
        const auto saved = loadRouteDocumentFile(options.documentPath).document;
        assert(saved.devices[0].sampleRate == 48000 &&
               saved.devices[0].formatRequestReceipt == driverRateReceipt(request));
    };
    engine.start();
    restarted.until([&] {
        return link.rates.empty() && engine.status().restored;
    });
    assert(engine.observe()->document().devices[0].sampleRate == 48000);
    link.beforeRateAck = {};

    // A replacement native driver starts at sequence one with a fresh origin.
    auto replacement = request;
    replacement.origin = "fedcba98-7654-3210-fedc-ba9876543210";
    link.rates = {replacement};
    ++link.incarnation;
    engine.retryNow();
    restarted.until([&] {
        return link.rates.empty() && engine.status().restored;
    });
    assert(engine.observe()->document().devices[0].sampleRate == 44100);

    // A reply from a disconnected pairing must not enter the document or be ACKed.
    replacement.sequence = 2;
    replacement.sampleRate = 48000;
    link.rates = {replacement};
    link.duringRateQuery = [&] {
        ++link.incarnation;
    };
    const auto acknowledgements = link.acknowledgements;
    engine.retryNow();
    restarted.tick();
    assert(link.acknowledgements == acknowledgements &&
           loadRouteDocumentFile(options.documentPath).document.devices[0].sampleRate == 44100);
    link.duringRateQuery = {};

    // Failed durable saves leave requests outstanding and keep the previous graph alive.
    {
        std::ofstream future(options.documentPath);
        future << "{\"version\":99}";
    }
    engine.retryNow();
    restarted.tick();
    assert(link.acknowledgements == acknowledgements && !link.rates.empty() &&
           !engine.status().controlError.empty());
    assert(engine.observe()->document().devices[0].sampleRate == 44100);
    {
        std::ofstream repaired(options.documentPath);
        repaired << encodeRouteDocumentJSON(engine.observe()->document());
    }
    engine.retryNow();
    restarted.until([&] {
        return link.rates.empty() && engine.status().restored;
    });
    assert(engine.observe()->document().devices[0].sampleRate == 48000);

    auto disabled = engine.observe()->document();
    disabled.devices[0].routing.enabled = false;
    engine.setDesired(disabled, true);
    replacement.sequence = 3;
    replacement.sampleRate = 44100;
    link.rates = {replacement};
    engine.retryNow();
    restarted.until([&] {
        return link.rates.empty() && engine.status().restored;
    });
    const auto saved = loadRouteDocumentFile(options.documentPath).document;
    assert(!saved.devices[0].routing.enabled && saved.devices[0].sampleRate == 48000 &&
           saved.devices[0].formatRequestReceipt == driverRateReceipt(replacement));

    // Stale requests for deleted nodes never recreate them or rewrite intent.
    replacement.device = "removed";
    link.rates = {replacement};
    const auto generation = engine.status().configuration.desiredGeneration;
    engine.retryNow();
    restarted.until([&] {
        return link.rates.empty();
    });
    assert(engine.status().configuration.desiredGeneration == generation);
}
void savedDocumentWatchAndRecovery() {
    TemporaryFolder folder;
    EngineRuntimeOptions options;
    options.documentPath = folder.path / "routes.json";
    Fixture fixture(options);
    auto& engine = fixture.runtime;
    engine.start();
    fixture.tick();
    assert(!engine.status().documentError.empty() && fixture.link.submissions == 0 &&
           fixture.sources.empty());
    RouteDocument desired;
    VirtualDeviceDefinition device;
    device.id = "saved";
    device.name = "first";
    desired.devices.push_back(device);
    engine.setDesired(desired, true); // First document can be created while startup is waiting.
    fixture.until([&] {
        return engine.status().restored;
    });
    desired.devices[0].name = "memory";
    engine.setDesired(desired);
    fixture.until([&] {
        return engine.status().restored;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    fixture.tick();
    assert(engine.observe()->document().devices[0].name == "memory");
    desired.devices[0].name = "second";
    saveRouteDocumentFile(options.documentPath, desired);
    engine.retryNow();
    fixture.until([&] {
        return engine.status().restored && engine.observe()->document().devices[0].name == "second";
    });
    {
        std::ofstream file(options.documentPath);
        file << "corrupt";
    }
    engine.retryNow();
    fixture.until([&] {
        return engine.status().restored && engine.status().configuration.recoveredBackup;
    });
    assert(engine.observe()->document().devices[0].name == "first");
    {
        std::ofstream file(options.documentPath);
        file << "{\"version\":99}";
    }
    engine.retryNow();
    fixture.tick();
    assert(!engine.status().documentError.empty() && !engine.status().restored);
    assert(engine.observe()->document().devices[0].name == "first");
    std::ifstream file(options.documentPath);
    const std::string contents((std::istreambuf_iterator<char>(file)), {});
    assert(contents == "{\"version\":99}");
    assert(fixture.sources.empty() && fixture.output->opens == 0);
}
} // namespace
void installationValidationNeverConstructsHardware() {
    Link link;
    link.publicClients = true;
    EngineRuntimeOptions options;
    options.installationValidation = true;
    uint32_t sourceFactories = 0, monitorFactories = 0;
    EngineRuntime runtime(
        link, options,
        [&](SourceKind, const std::string&, uint32_t) -> std::shared_ptr<SourceEndpoint> {
            ++sourceFactories;
            throw std::runtime_error("Validation must not construct capture adapters");
        },
        [&]() -> std::unique_ptr<AudioHost> {
            ++monitorFactories;
            throw std::runtime_error("Validation must not construct hardware output adapters");
        });
    const auto desired = document();
    runtime.setDesired(desired);
    runtime.start();
    const auto deadline = hostNowNs() + 3000000000ULL;
    do {
        runtime.poll();
        if (runtime.status().configuration.phase == RestorationPhase::Restored &&
            runtime.status().render.blocks > 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (hostNowNs() < deadline);
    assert(runtime.status().configuration.phase == RestorationPhase::Restored);
    assert(runtime.status().render.blocks > 0 && !sourceFactories && !monitorFactories);
    assert(runtime.devices().size() == desired.devices.size());
    assert(runtime.observe()->document().devices[0].monitors.size() ==
           desired.devices[0].monitors.size());
    for (const auto& source : runtime.status().sources)
        assert(!source.connected);
    assert(runtime.status().monitors.empty());
}

void audioServiceResetRestoresSavedIntent() {
    Fixture fixture;
    fixture.runtime.setDesired(document());
    fixture.runtime.start();
    fixture.until([&] {
        return fixture.runtime.status().restored;
    });
    fixture.sample(0.2f); // Allow render-driven upstream demand before checking healthy owners.
    const auto desired = encodeRouteDocumentJSON(*fixture.runtime.editorSnapshot().document);
    const auto outputOpens = fixture.output->opens;
    const auto submissions = fixture.link.submissions;
    std::map<std::string, uint32_t> opens;
    for (const auto& [id, source] : fixture.sources)
        opens[id] = source->opens;
    // Inventory/wake acceleration cannot restart a healthy graph.
    fixture.runtime.retryNow();
    fixture.tick();
    assert(fixture.output->opens == outputOpens && fixture.link.submissions == submissions);
    for (const auto& [id, source] : fixture.sources)
        assert(source->opens == opens.at(id));
    fixture.runtime.audioServiceReset();
    assert(fixture.runtime.status().started && !fixture.runtime.status().restored);
    assert(!fixture.runtime.status().render.running && fixture.link.session() == 0);
    for (const auto& source : fixture.runtime.status().sources)
        assert(!source.connected);
    for (const auto& monitor : fixture.runtime.status().monitors)
        assert(!monitor.connected);
    assert(encodeRouteDocumentJSON(*fixture.runtime.editorSnapshot().document) == desired);
    // Repeated reset while the service is absent must retain the full intent.
    fixture.runtime.audioServiceReset();
    fixture.tick();
    assert(!fixture.runtime.status().restored);
    fixture.link.incarnation = 2;
    fixture.until([&] {
        return fixture.runtime.status().restored;
    });
    assert(fixture.link.submissions > submissions && fixture.output->opens > outputOpens);
    assert(fixture.sources.size() == opens.size());
    for (const auto& [id, source] : fixture.sources)
        assert(source->opened && source->opens > opens.at(id));
    assert(encodeRouteDocumentJSON(*fixture.runtime.editorSnapshot().document) == desired);
    fixture.runtime.stop();
    fixture.runtime.audioServiceReset();
    assert(!fixture.runtime.status().started && fixture.link.session() == 2);
}

void listReorderingRetainsRunningGraph(bool requireContinuousSamples = true) {
    TemporaryFolder folder;
    EngineRuntimeOptions options;
    options.documentPath = folder.path / "ordering.json";
    options.monitorDelayFrames = 1024; // Explicit conservative reorder fixture.
    std::printf("Reorder fixture: 1024-frame monitor queue, sample check=%s\n",
                requireContinuousSamples ? "every poll" : "eventual after each commit");
    auto desired = document();
    desired.version = 2;
    desired.devices[0].formatRequestReceipt = "preserved-native-receipt";
    saveRouteDocumentFile(options.documentPath, desired);
    Fixture fixture(options);
    fixture.runtime.start();
    fixture.until([&] {
        return fixture.runtime.status().restored;
    });
    fixture.sample(0.2f);
    const auto outputOpens = fixture.output->opens;
    std::map<std::string, uint32_t> opens;
    std::map<std::string, std::shared_ptr<DeviceAudioRegion>> regions;
    for (const auto& [id, source] : fixture.sources)
        opens[id] = source->opens;
    for (const auto& transport : fixture.runtime.devices())
        regions[transport.id] = transport.region;
    for (unsigned iteration = 0; iteration < 8; ++iteration) {
        const auto before = fixture.runtime.editorSnapshot();
        std::vector<std::string> devices;
        std::vector<edit::Command> commands;
        for (const auto& device : before.document->devices) {
            devices.push_back(device.id);
            std::vector<std::string> sources;
            for (const auto& source : device.routing.sources)
                sources.push_back(source.id);
            std::reverse(sources.begin(), sources.end());
            commands.push_back(edit::ReorderSources{device.id, std::move(sources)});
        }
        std::reverse(devices.begin(), devices.end());
        commands.push_back(edit::ReorderDevices{std::move(devices)});
        const auto expected =
            encodeRouteDocumentJSON(edit::applyCommands(*before.document, commands));
        fixture.runtime.applyEditor(before.revision, commands);
        fixture.until([&] {
            for (const auto& [id, source] : fixture.sources)
                assert(source->opens == opens.at(id) && source->opened);
            assert(fixture.output->opens == outputOpens);
            if (requireContinuousSamples && std::fabs(fixture.output->sample - 0.2f) >= 0.0001f) {
                const auto state = fixture.runtime.status();
                std::fprintf(
                    stderr,
                    "Reorder gap: iteration=%u sample=%g frames=%llu skipped=%llu late=%llu "
                    "reanchors=%llu missing=%llu phase=%u published=%llu nowFrame=%llu queue=%u "
                    "maxExecutionNs=%llu\n",
                    iteration, fixture.output->sample,
                    static_cast<unsigned long long>(state.render.frames),
                    static_cast<unsigned long long>(state.render.skippedFrames),
                    static_cast<unsigned long long>(state.render.lateFrames),
                    static_cast<unsigned long long>(state.render.reanchors),
                    static_cast<unsigned long long>(
                        state.monitors.empty() ? 0 : state.monitors[0].audio.missingFrames),
                    static_cast<unsigned>(state.configuration.phase),
                    static_cast<unsigned long long>(state.render.publishedFrame),
                    static_cast<unsigned long long>(hostFrame(hostNowNs(), 48000)),
                    state.monitors.empty() ? 0 : state.monitors[0].queueFrames,
                    static_cast<unsigned long long>(state.render.maximumExecutionNs));
                assert(false);
            }
            return fixture.runtime.status().restored &&
                   encodeRouteDocumentJSON(fixture.runtime.observe()->document()) == expected;
        });
        for (const auto& transport : fixture.runtime.devices())
            assert(transport.region == regions.at(transport.id));
        fixture.sample(0.2f);
        assert(encodeRouteDocumentJSON(loadRouteDocumentFile(options.documentPath).document) ==
               expected);
    }
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--idle") {
        disabledDevicesSuspendRenderingAndResumeOnEnable();
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--reorder") {
        listReorderingRetainsRunningGraph();
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--reorder-concurrency") {
        // Thread instrumentation misses wall-clock deadlines and can reanchor
        // the renderer. Check ownership, mapping retention, persistence and
        // eventual audio here; strict waveform continuity runs in optimized
        // and ASAN builds. Monitor bus continuity also has a deterministic
        // clock test under TSan in monitor_outputs_tests.cpp.
        listReorderingRetainsRunningGraph(false);
        return 0;
    }
    if (argc != 1)
        return 2;
    disabledDevicesSuspendRenderingAndResumeOnEnable();
    audioServiceResetRestoresSavedIntent();
    editorPreviewWithoutClientsOrWires();
    physicalInputBudgetDoesNotShortenApplicationHistory();
    applicationTimingReplansWithoutReopeningCaptures();
    installationValidationNeverConstructsHardware();
    completeLifecycleAndBlockingControl();
    smallRenderQuantumUsesMatchingMonitorQueue(32);
    smallRenderQuantumUsesMatchingMonitorQueue(64);
    restorationWaitsForCompensationHistory();
    savedDocumentWatchAndRecovery();
    durableClientRateRequests();
    std::puts("Persistent runtime scheduling, complete graph recovery, staged acquisition and "
              "saved document lifecycle passed");
}
