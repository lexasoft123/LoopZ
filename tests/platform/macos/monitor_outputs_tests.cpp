#include <loopz/macos/monitor_outputs.hpp>
#include <loopz/macos/scene_renderer.hpp>
#include <loopz/timeline.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>

namespace {
thread_local bool trackHeap = false;
thread_local unsigned heapEvents = 0;
} // namespace
void* operator new(std::size_t size) {
    if (trackHeap)
        ++heapEvents;
    if (auto* value = std::malloc(size ? size : 1))
        return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void* value) noexcept {
    if (trackHeap && value)
        ++heapEvents;
    std::free(value);
}
void operator delete[](void* value) noexcept {
    ::operator delete(value);
}
void operator delete(void* value, std::size_t) noexcept {
    ::operator delete(value);
}
void operator delete[](void* value, std::size_t) noexcept {
    ::operator delete(value);
}

namespace {
using namespace loopz;
using namespace loopz::macos;
using namespace singz;
void near(float actual, float expected) {
    assert(std::isfinite(actual) && std::fabs(actual - expected) < 0.0001f);
}
template <typename Operation> void rejects(Operation operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::exception&) {
        rejected = true;
    }
    assert(rejected);
}
struct Facts {
    AudioHostInventory inventory;
    AudioHostConfig config;
    AudioHostFormat format;
    AudioHostState state{AudioHostState::Closed};
    AudioHostRender callback{nullptr};
    void* context{nullptr};
    uint32_t opens{0}, starts{0}, stops{0}, enumerations{0};
    bool failOpen{false}, failStart{false}, throwInventory{false};
    std::atomic<uint64_t> callbacks{0};
    std::function<void()> onStart;
    explicit Facts(std::string uid = "headphones", uint32_t channels = 2, double rate = 48000) {
        AudioHostDeviceInfo info;
        info.uid = std::move(uid);
        info.outputChannels = channels;
        info.nominalSampleRate = rate;
        info.direction = AudioHostEndpointDirection::Output;
        inventory.defaultOutputUid = info.uid;
        inventory.devices.push_back(std::move(info));
    }
    bool fire(float& sample, uint64_t now = 1000, bool validTimestamp = true,
              const AudioHostRenderBlock* timing = nullptr) {
        std::array<std::array<float, 64>, 64> output{};
        std::array<float*, 64> planes{};
        std::array<std::array<float, 64>, 64> input{};
        std::array<const float*, 64> inputPlanes{};
        for (uint32_t channel = 0; channel < format.inputChannels; ++channel) {
            input[channel].fill(0.8f);
            inputPlanes[channel] = input[channel].data();
        }
        for (uint32_t channel = 0; channel < format.outputChannels; ++channel) {
            output[channel].fill(9);
            planes[channel] = output[channel].data();
        }
        AudioHostRenderBlock block;
        block.output = planes.data();
        block.input = inputPlanes.data();
        block.inputChannels = format.inputChannels;
        block.inputTimestampValid = block.inputTimestampHardware = true;
        block.outputChannels = format.outputChannels;
        block.frames = 64;
        block.maximumFrames = 64;
        block.sampleRate = format.sampleRate;
        block.outputFrame = 49024;
        block.outputHostTimeNs = frameNs(52000, 48000);
        block.callbackHostTimeNs = now * 1000000;
        block.outputTimestampValid = validTimestamp;
        block.outputTimestampHardware = validTimestamp;
        if (timing) {
            block.outputFrame = timing->outputFrame;
            block.outputHostTimeNs = timing->outputHostTimeNs;
            block.outputTimestampHardware = timing->outputTimestampHardware;
            block.streamGeneration = timing->streamGeneration;
            block.discontinuity = timing->discontinuity;
        }
        heapEvents = 0;
        trackHeap = true;
        const bool valid = callback(context, block);
        trackHeap = false;
        assert(!heapEvents);
        callbacks.fetch_add(1);
        sample = output[0][0];
        return valid;
    }
};
class FakeHost final : public AudioHostBackend {
  public:
    const std::shared_ptr<Facts> facts;
    explicit FakeHost(std::shared_ptr<Facts> state) : facts(std::move(state)) {}
    AudioHostInventory enumerate() const override {
        ++facts->enumerations;
        if (facts->throwInventory)
            throw std::runtime_error("Inventory unavailable");
        return facts->inventory;
    }
    AudioHostResult open(const AudioHostConfig& config, AudioHostRender callback,
                         void* context) override {
        ++facts->opens;
        facts->config = config;
        facts->callback = callback;
        facts->context = context;
        if (facts->failOpen)
            return {false,
                    AudioHostError::ProviderFailure,
                    AudioHostState::Error,
                    {},
                    {},
                    "fixture failure"};
        assert(config.inputChannels.empty() == config.inputDeviceUid.empty());
        assert(config.inputChannels.empty() || config.inputDeviceUid == config.outputDeviceUid);
        assert(config.requestedSampleRate == 0 && config.requestedBufferFrames == 0 &&
               !config.exclusive);
        auto& format = facts->format;
        format = {};
        auto info = std::find_if(facts->inventory.devices.begin(), facts->inventory.devices.end(),
                                 [&](const auto& device) {
                                     return device.uid == config.outputDeviceUid;
                                 });
        assert(info != facts->inventory.devices.end());
        format.sampleRate = info->nominalSampleRate;
        format.outputChannels = static_cast<uint32_t>(config.outputChannels.size());
        format.inputChannels = static_cast<uint32_t>(config.inputChannels.size());
        format.maximumFrames = 8192;
        format.nominalBufferFrames =
            info->bufferFrames.preferredFrames ? info->bufferFrames.preferredFrames : 1024;
        facts->state = AudioHostState::Open;
        return {true, AudioHostError::None, facts->state, format, {}, {}};
    }
    AudioHostResult start() override {
        ++facts->starts;
        if (facts->onStart)
            facts->onStart();
        if (facts->failStart)
            return {false,
                    AudioHostError::ProviderFailure,
                    AudioHostState::Error,
                    {},
                    {},
                    "fixture failure"};
        facts->state = AudioHostState::Running;
        return {true, AudioHostError::None, facts->state, facts->format, {}, {}};
    }
    void stop() noexcept override {
        if (facts->state != AudioHostState::Closed)
            ++facts->stops;
        facts->state = AudioHostState::Closed;
    }
    AudioHostStatus status() const noexcept override {
        AudioHostStatus result;
        result.state = facts->state;
        result.format = facts->format;
        result.callbacks = facts->callbacks.load();
        return result;
    }
};
class Input final : public AudioSource {
  public:
    uint32_t channels() const noexcept override {
        return 2;
    }
    AudioReadResult read(uint64_t, double, RoutingOutput output, uint32_t frames,
                         AudioReadCursor&) const noexcept override {
        for (auto* plane : output.channels)
            std::fill_n(plane, frames, 0.4f);
        return {0, true};
    }
};
class Capture final : public AudioSink {
  public:
    uint32_t channels() const noexcept override {
        return 2;
    }
    bool requested() const noexcept override {
        return false;
    }
    bool publish(uint64_t, uint32_t, RoutingInput, uint32_t) noexcept override {
        return true;
    }
    void idle() noexcept override {}
};
RouteDocument document(bool second = true) {
    VirtualDeviceDefinition device;
    device.id = "mix";
    device.name = "Stream Mix";
    device.routing.sources = {{"input", 2, 1, true, false, defaultChannelConnections(2, 2)}};
    device.bindings = {{"input", SourceKind::InputDevice, "physical"}};
    device.monitors = {
        {"one", "headphones", 2, 0.5f, true, false, defaultChannelConnections(2, 2)}};
    if (second)
        device.monitors.push_back(
            {"two", "speakers", 2, 0.25f, true, false, defaultChannelConnections(2, 2)});
    return {1, {device}};
}
struct Fixture {
    std::vector<std::shared_ptr<Facts>> facts;
    size_t factories{0};
    uint64_t completed{1000};
    MonitorOutputs outputs;
    explicit Fixture(
        std::vector<std::shared_ptr<Facts>> providers, uint32_t delay = 1024,
        uint32_t periodReserve = 2,
        MonitorOutputs::InputPermission permission =
            [] {
                return true;
            },
        uint32_t inspectionIntervalMs = 0)
        : facts(std::move(providers)),
          outputs(
              48000, delay,
              [&] {
                  assert(factories < facts.size());
                  return std::make_unique<AudioHost>(
                      std::make_unique<FakeHost>(facts[factories++]));
              },
              [&] {
                  return completed;
              },
              kBlock, periodReserve, std::move(permission), inspectionIntervalMs) {}
    std::unique_ptr<AudioScene> scene(const RouteDocument& document) {
        return std::make_unique<AudioScene>(
            document, SceneTiming{},
            [](const auto&, const auto&, const auto&) -> ResolvedAudioSource {
                return {std::make_shared<Input>(), {}};
            },
            [&](const auto& device) {
                return outputs.decorate(device, std::make_shared<Capture>());
            });
    }
    void prime(AudioScene& scene) {
        for (uint32_t block = 0; block < 16; ++block)
            assert(scene.render(48000 + block * kBlock, kBlock).validBlock);
    }
    void poll(const AudioScene* scene, uint64_t now = 1000) {
        completed = now;
        outputs.poll(scene, now);
    }
};
void sharedClockMonitoringKeepsControlsAndRecoversModes() {
    auto facts = std::make_shared<Facts>();
    facts->inventory.devices[0].inputChannels = 2;
    facts->inventory.devices[0].direction = AudioHostEndpointDirection::Duplex;
    bool permitted = true;
    Fixture fixture({facts}, 1024, 2, [&] {
        return permitted;
    });
    auto desired = document(false);
    desired.devices[0].bindings[0].selector = "headphones";
    auto current = fixture.scene(desired);
    current->activate();
    fixture.prime(*current);
    fixture.poll(current.get());
    auto status = fixture.outputs.status()[0];
    assert(fixture.outputs.restored() && status.directHardwareClock && status.queueFrames == 0);
    assert(facts->config.inputDeviceUid == "headphones" &&
           facts->config.inputChannels == std::vector<uint32_t>({0, 1}));
    float sample = 0;
    assert(facts->fire(sample));
    near(sample, 0.4f); // Actual duplex input, not the distinct 0.4-valued scene source.
    const auto callbacks = fixture.outputs.status()[0].audio.callbacks;

    desired.devices[0].name = "Unrelated metadata";
    auto renamed = fixture.scene(desired);
    renamed->activate();
    assert(fixture.outputs.status()[0].audio.callbacks == callbacks && facts->opens == 1);
    desired.devices[0].routing.sources[0].gain = 0.25f;
    desired.devices[0].routing.gain = 0.5f;
    auto edited = fixture.scene(desired);
    assert(facts->fire(sample));
    near(sample, 0.4f); // A staged device edit cannot leak before activation.
    edited->activate();
    assert(facts->fire(sample));
    near(sample, 0.05f);
    assert(facts->opens == 1 && fixture.outputs.status()[0].directHardwareClock);
    desired.devices[0].monitors[0].muted = true;
    auto muted = fixture.scene(desired);
    muted->activate();
    assert(facts->fire(sample));
    near(sample, 0);
    assert(facts->opens == 1); // Monitor mute stays on the same clock/stream.
    desired.devices[0].monitors[0].muted = false;
    auto audible = fixture.scene(desired);
    audible->activate();
    fixture.prime(*audible);

    permitted = false;
    fixture.poll(audible.get(), 1010);
    fixture.poll(audible.get(), 1210);
    assert(fixture.outputs.restored() && facts->config.inputChannels.empty());
    assert(!fixture.outputs.status()[0].directHardwareClock);
    permitted = true;
    fixture.poll(audible.get(), 1220);
    fixture.poll(audible.get(), 1420);
    assert(fixture.outputs.restored() && fixture.outputs.status()[0].directHardwareClock);
    assert(facts->fire(sample, 1420));
    near(sample, 0.05f);

    desired.devices[0].bindings.push_back({"other", SourceKind::Application, "other-app", false});
    desired.devices[0].routing.sources.push_back({"other", 2, 1, true, false, {{0, 0, 1}}});
    auto mixed = fixture.scene(desired);
    assert(facts->fire(sample, 1421));
    near(sample, 0.05f);
    mixed->activate();
    fixture.prime(*mixed);
    fixture.poll(mixed.get(), 1430);
    fixture.poll(mixed.get(), 1630);
    assert(fixture.outputs.restored() && facts->config.inputChannels.empty());
    assert(!fixture.outputs.status()[0].directHardwareClock);
    assert(facts->fire(sample, 1630));
    near(sample, 0.125f); // The application source is still included via the common-clock bus.

    desired.devices[0].routing.sources.pop_back();
    desired.devices[0].bindings.pop_back();
    auto restored = fixture.scene(desired);
    restored->activate();
    fixture.poll(restored.get(), 1640);
    fixture.poll(restored.get(), 1840);
    assert(fixture.outputs.restored() && fixture.outputs.status()[0].directHardwareClock);
    facts->state = AudioHostState::DeviceLost;
    fixture.poll(restored.get(), 1850);
    fixture.poll(restored.get(), 2050);
    assert(fixture.outputs.restored() && fixture.outputs.status()[0].directHardwareClock);
    assert(facts->fire(sample, 2050));
    near(sample, 0.05f);
    assert(facts->config.requestedBufferFrames == 0 && facts->config.requestedSampleRate == 0);
}

void optInPeriodReservePreservesFloorsAndRateConversion() {
    rejects([] {
        MonitorOutputs output(48000, 0, {}, {}, 64, 0);
    });
    rejects([] {
        MonitorOutputs output(48000, 0, {}, {}, 64, 3);
    });
    for (const auto delay : {0u, 1536u}) {
        auto facts = std::make_shared<Facts>();
        Fixture fixture({facts}, delay, 1);
        auto desired = document(false);
        auto current = fixture.scene(desired);
        current->activate();
        fixture.prime(*current);
        fixture.poll(current.get());
        assert(fixture.outputs.restored());
        assert(fixture.outputs.status()[0].queueFrames == std::max(delay, 1280u));
        float sample = 0;
        assert(facts->fire(sample));
        near(sample, 0.2f);
        const auto before = fixture.outputs.status()[0];
        desired.devices[0].name = "Renamed while monitoring";
        auto pending = fixture.scene(desired);
        pending->activate();
        assert(fixture.outputs.status()[0].audio.callbacks == before.audio.callbacks);
        assert(facts->opens == 1);

        facts->inventory.devices[0].nominalSampleRate = 44100;
        fixture.poll(pending.get(), 1010);
        fixture.poll(pending.get(), 1110);
        assert(fixture.outputs.restored());
        // Round up the physical period to the graph clock before adding its quantum.
        assert(fixture.outputs.status()[0].queueFrames == std::max(delay, 1371u));
        assert(facts->config.requestedBufferFrames == 0);
    }
}

void hardwareBufferChangesRebuildMonitorTiming() {
    auto facts = std::make_shared<Facts>();
    facts->inventory.devices[0].bufferFrames.preferredFrames = 512;
    Fixture fixture({facts}, 0);
    auto current = fixture.scene(document(false));
    current->activate();
    fixture.prime(*current);
    fixture.poll(current.get());
    assert(fixture.outputs.restored() && facts->opens == 1);
    assert(fixture.outputs.status()[0].queueFrames == 2 * 512 + kBlock);
    for (const auto period : {32u, 64u, 512u}) {
        const auto opens = facts->opens;
        facts->inventory.devices[0].bufferFrames.preferredFrames = period;
        fixture.poll(current.get(), 1010 + period);
        assert(facts->opens == opens + 1 && fixture.outputs.restored());
        assert(fixture.outputs.status()[0].bufferFrames == period);
        assert(fixture.outputs.status()[0].queueFrames == 2 * period + kBlock);
        fixture.poll(current.get(), 1011 + period);
        assert(facts->opens == opens + 1); // No reopen loop after the format is current.
    }
}

void committedOwnershipAndIndependentRecovery() {
    auto a = std::make_shared<Facts>(), b = std::make_shared<Facts>("speakers", 2, 44100);
    Fixture fixture({a, b});
    auto desired = document();
    auto current = fixture.scene(desired);
    fixture.poll(current.get());
    assert(!a->opens && !b->opens && fixture.outputs.restored());
    current->activate();
    fixture.prime(*current);
    fixture.poll(current.get());
    assert(a->opens == 1 && b->opens == 1 && fixture.outputs.restored());
    float sample;
    assert(a->fire(sample));
    near(sample, 0.2f);
    assert(b->fire(sample));
    near(sample, 0.1f);
    a->state = AudioHostState::DeviceLost;
    fixture.poll(current.get(), 1100);
    assert(!fixture.outputs.restored() && a->stops == 1 && !b->stops);
    assert(b->fire(sample, 1100));
    near(sample, 0.1f);
    fixture.poll(current.get(), 1199);
    assert(a->opens == 1);
    fixture.poll(current.get(), 1200);
    assert(a->opens == 2 && b->opens == 1 && fixture.outputs.restored());
    desired.devices[0].monitors[0].gain = 0.25f;
    auto staged = fixture.scene(desired);
    assert(a->fire(sample, 1200));
    near(sample, 0.2f);
    fixture.poll(current.get(), 1201);
    assert(a->opens == 2);
    staged.reset(); // Rejected edit leaves both streams and old gain unchanged.
    assert(a->fire(sample, 1201));
    near(sample, 0.2f);
    auto next = fixture.scene(desired);
    next->activate();
    fixture.prime(*next);
    fixture.poll(next.get(), 1202);
    assert(a->opens == 2 && b->opens == 1);
    assert(a->fire(sample, 1202));
    near(sample, 0.1f);
    AudioMeterSnapshot meter;
    assert(fixture.outputs.meters("mix", "one", meter));
    near(meter.peak[0], 0.1f);
    desired.devices[0].monitors[0].muted = true;
    auto muted = fixture.scene(desired);
    muted->activate();
    fixture.prime(*muted);
    fixture.poll(muted.get(), 1203);
    assert(a->fire(sample, 1203));
    near(sample, 0);
    assert(muted->demandedDevices() == 1 && a->opens == 2);
    desired.devices[0].monitors[0].enabled = false;
    auto disabled = fixture.scene(desired);
    disabled->activate();
    fixture.prime(*disabled);
    fixture.poll(disabled.get(), 1204);
    assert(a->state == AudioHostState::Closed && b->state == AudioHostState::Running &&
           b->opens == 1);
    fixture.poll(nullptr, 1205);
    assert(b->state == AudioHostState::Closed);
    fixture.poll(muted.get(), 1206); // Old desired graph can restore after owner restart.
    assert(fixture.outputs.restored());
    fixture.outputs.stop();
}
void partialProfileDefaultAndPendingRateChange() {
    auto facts = std::make_shared<Facts>();
    Fixture fixture({facts});
    auto desired = document(false);
    auto& monitor = desired.devices[0].monitors[0];
    monitor.deviceSelector = "default";
    monitor.channels = 8;
    auto current = fixture.scene(desired);
    current->activate();
    fixture.prime(*current);
    fixture.poll(current.get());
    assert(!fixture.outputs.restored());
    auto status = fixture.outputs.status();
    assert(status[0].connected && !status[0].complete && status[0].requestedChannels == 8 &&
           status[0].physicalChannels == 2 && status[0].queueFrames == 2304);
    facts->inventory.devices[0].outputChannels = 8;
    fixture.poll(current.get(), 1010);
    fixture.poll(current.get(), 1110);
    assert(fixture.outputs.restored() && facts->opens == 2);
    monitor.gain = 0.25f;
    auto pending = fixture.scene(desired); // Prepared at 48 kHz.
    facts->inventory.devices[0].nominalSampleRate = 44100;
    fixture.poll(current.get(), 1120);
    fixture.poll(current.get(), 1320);
    assert(facts->opens == 3 && fixture.outputs.restored());
    float sample;
    assert(facts->fire(sample, 1320));
    near(sample, 0.2f);
    pending->activate();
    fixture.prime(*pending);
    assert(facts->fire(sample, 1321));
    near(sample, 0); // Rate-correct program prepared off RT next poll.
    fixture.poll(pending.get(), 1321);
    assert(facts->opens == 3 && facts->fire(sample, 1321));
    near(sample, 0.1f);
    facts->inventory.devices[0].uid = "new-default";
    facts->inventory.defaultOutputUid = "new-default";
    fixture.poll(pending.get(), 1330);
    fixture.poll(pending.get(), 1730);
    assert(facts->opens == 4 && fixture.outputs.status()[0].resolvedUid == "new-default");
    facts->inventory.defaultOutputUid = "com.lexasoft.loopz.device.recursive";
    fixture.poll(pending.get(), 1740);
    fixture.outputs.retryNow(1741);
    fixture.poll(pending.get(), 1741);
    assert(!fixture.outputs.restored() && facts->opens == 4);
    facts->inventory.defaultOutputUid = "new-default";
    fixture.outputs.retryNow(1742);
    fixture.poll(pending.get(), 1742);
    assert(fixture.outputs.restored());
}
void failuresDeadlineAndClockStall() {
    auto facts = std::make_shared<Facts>();
    Fixture fixture({facts});
    auto current = fixture.scene(document(false));
    current->activate();
    fixture.prime(*current);
    facts->failOpen = true;
    fixture.poll(current.get());
    assert(!fixture.outputs.restored() && facts->opens == 1);
    fixture.poll(current.get(), 1099);
    assert(facts->opens == 1);
    facts->failOpen = false;
    facts->failStart = true;
    fixture.poll(current.get(), 1100);
    assert(facts->opens == 2 && !fixture.outputs.restored());
    facts->failStart = false;
    fixture.poll(current.get(), 1300);
    assert(fixture.outputs.restored());
    float sample;
    assert(!facts->fire(sample, 1300, false));
    near(sample, 0);
    fixture.poll(current.get(), 1301);
    fixture.poll(current.get(), 1701);
    assert(fixture.outputs.restored());
    auto opens = facts->opens;
    fixture.poll(current.get(), 3701);
    assert(!fixture.outputs.restored()); // No callback for two seconds.
    fixture.outputs.retryNow(3702);
    fixture.poll(current.get(), 3702);
    assert(facts->opens == opens + 1 && fixture.outputs.restored());
    facts->throwInventory = true;
    fixture.poll(current.get(), 3703);
    assert(!fixture.outputs.restored());
    facts->throwInventory = false;
    fixture.outputs.retryNow(3704);
    fixture.poll(current.get(), 3704);
    assert(fixture.outputs.restored());
    fixture.outputs.stop();
    facts->onStart = [&] {
        assert(facts->fire(sample, 4000));
        near(sample, 0); // No admission until deadline accepted.
        fixture.completed = 14000;
    };
    fixture.completed = 4000;
    fixture.outputs.poll(current.get(), 4000);
    assert(!fixture.outputs.restored() && facts->state == AudioHostState::Closed);
    assert(fixture.outputs.status()[0].error.find("expired") != std::string::npos);
    facts->onStart = {};
    fixture.outputs.retryNow(14001);
    fixture.poll(current.get(), 14001);
    fixture.poll(current.get(), 14101);
    assert(fixture.outputs.restored());
    rejects([] {
        MonitorOutputs invalid(48000, kCapacity);
    });
    auto recursive = document(false);
    recursive.devices[0].monitors[0].deviceSelector = "com.lexasoft.loopz.device.self";
    Fixture second({std::make_shared<Facts>()});
    rejects([&] {
        second.scene(recursive);
    });
}
void independentAcquisitionDeadlines() {
    for (const bool firstExpires : {false, true}) {
        auto first = std::make_shared<Facts>();
        auto second = std::make_shared<Facts>("speakers");
        Fixture fixture({first, second});
        first->onStart = [&] {
            fixture.completed += firstExpires ? 10001 : 9000;
        };
        second->onStart = [&] {
            fixture.completed += 2000;
        };
        auto scene = fixture.scene(document());
        scene->activate();
        fixture.prime(*scene);
        fixture.outputs.poll(scene.get(), fixture.completed);
        const auto status = fixture.outputs.status();
        assert(status.size() == 2 && status[0].connected == !firstExpires);
        assert(status[1].connected && second->opens == 1);
        assert(fixture.outputs.restored() == !firstExpires);
        float sample;
        if (!firstExpires) {
            assert(first->fire(sample, fixture.completed));
            near(sample, 0.2f);
        }
        // The later monitor has just been admitted. Its first-callback grace
        // starts now, not before these slow opens. Do not immediately call
        // that new stream stalled or reopen it before it can deliver audio.
        fixture.poll(scene.get(), fixture.completed + 1);
        assert(fixture.outputs.status()[1].connected && second->opens == 1);
        assert(second->fire(sample, fixture.completed));
        near(sample, 0.1f);
        if (firstExpires) {
            first->onStart = {};
            fixture.outputs.retryNow(fixture.completed);
            fixture.outputs.poll(scene.get(), fixture.completed);
            assert(fixture.outputs.restored() && first->opens == 2 && second->opens == 1);
        }
    }
}
void hardwareClockSurvivesEditsAndResetsWithStream() {
    auto facts = std::make_shared<Facts>();
    Fixture fixture({facts});
    auto desired = document(false);
    auto current = fixture.scene(desired);
    current->activate();
    fixture.prime(*current);
    fixture.poll(current.get());
    AudioHostRenderBlock timing;
    timing.outputTimestampHardware = true;
    timing.streamGeneration = 1;
    float sample;
    uint64_t index = 0;
    const auto fire = [&] {
        timing.outputFrame = 49024 + index * 64;
        timing.outputHostTimeNs = frameNs(52000, 48000) + frameNs(index * 64, 48000 * 1.0002);
        assert(facts->fire(sample, 1000 + index * 2, true, &timing));
        ++index;
        return fixture.outputs.status()[0].audio.measuredOutputRate;
    };
    for (uint32_t count = 0; count < 60; ++count)
        fire();
    assert(fire() > 48009);
    desired.devices[0].monitors[0].gain = 0.25f;
    auto next = fixture.scene(desired);
    next->activate();
    fixture.poll(next.get(), 1120);
    assert(fire() > 48009 && facts->opens == 1 && !facts->stops);
    timing.outputTimestampHardware = false;
    assert(fire() == 48000);
    timing.outputTimestampHardware = true;
    assert(fire() == 48000);
    for (uint32_t count = 0; count < 60; ++count)
        fire();
    assert(fire() > 48009);
    ++timing.streamGeneration;
    assert(fire() == 48000);
    for (uint32_t count = 0; count < 60; ++count)
        fire();
    assert(fire() > 48009);
    timing.discontinuity = AudioHostDiscontinuityClockReanchored;
    assert(fire() == 48000);
    timing.discontinuity = AudioHostDiscontinuityNone;
    for (uint32_t count = 0; count < 60; ++count)
        fire();
    assert(fire() > 48009);
    facts->state = AudioHostState::DeviceLost;
    fixture.poll(next.get(), 2000);
    fixture.poll(next.get(), 2100);
    assert(facts->opens == 2 && fixture.outputs.restored());
    assert(fire() == 48000);
    fixture.outputs.stop();
}
void rendererActivationAndConcurrentEdits() {
    auto facts = std::make_shared<Facts>();
    Fixture fixture({facts});
    auto desired = document(false);
    DeviceTransportPool pool;
    SceneRenderer renderer(
        {},
        [](const auto&, const auto&, const auto&) -> ResolvedAudioSource {
            return {std::make_shared<Input>(), {}};
        },
        [&](const auto& device, auto sink) {
            return fixture.outputs.decorate(device, std::move(sink));
        });
    pool.apply(desired, renderer.inputLatencyFrames(desired));
    auto first = renderer.prepare(desired, pool.devices());
    fixture.poll(renderer.observe().get());
    assert(!facts->opens);
    first->commit();
    for (uint32_t block = 0; block < 16; ++block)
        renderer.render(48000 + block * 256, 256, 1000000000);
    fixture.poll(renderer.observe().get());
    assert(facts->opens == 1);
    // Keep the committed update alive during subsequent staging/publication.
    std::atomic<unsigned> calls{0};
    std::jthread audio([&](std::stop_token stop) {
        do {
            float sample;
            assert(facts->fire(sample));
            assert(sample == 0 || std::fabs(sample - 0.1f) < 0.0001f ||
                   std::fabs(sample - 0.2f) < 0.0001f);
            calls.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        } while (!stop.stop_requested());
    });
    // More committed generations than the retirement bound, with interleaved
    // aborts. Preparation throughput belongs to the release benchmark, rather
    // than repeatedly zeroing large DSP arenas under race instrumentation.
    for (unsigned edit = 0; edit < 16; ++edit) {
        if (edit % 2 == 0)
            desired.devices[0].name = "Concurrent rename " + std::to_string(edit);
        else
            desired.devices[0].monitors[0].gain = edit % 4 == 1 ? 0.5f : 0.25f;
        bool unchangedMonitor;
        {
            auto committed = renderer.observe();
            // An aborted gain edit remains in desired intent. Compare against
            // the actual committed scene rather than calling its next rename
            // a no-op when it would also apply that previously rejected gain.
            unchangedMonitor = desired.devices[0].monitors[0].gain ==
                               committed->document().devices[0].monitors[0].gain;
        }
        const auto before = fixture.outputs.status()[0].audio.callbacks;
        std::unique_ptr<PreparedGraphActivation> update;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!update) {
            try {
                update = renderer.prepare(desired, pool.devices());
            } catch (const RealtimePublicationBusy&) {
                assert(std::chrono::steady_clock::now() < deadline);
                fixture.poll(renderer.observe().get());
                assert(facts->opens == 1 && facts->stops == 0);
                renderer.collect();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (edit % 3)
            update->commit(); // Other edits abort with the existing output intact.
        if (unchangedMonitor)
            assert(fixture.outputs.status()[0].audio.callbacks >= before);
        fixture.poll(renderer.observe().get());
        renderer.collect();
    }
    audio.request_stop();
    audio.join();
    assert(calls.load() > 0 && facts->opens == 1 && facts->stops == 0);
    fixture.outputs.stop();
}
} // namespace
void monitorOrderRetainsStreamsAndBufferedAudio() {
    auto a = std::make_shared<Facts>(), b = std::make_shared<Facts>("speakers", 2, 44100);
    Fixture fixture({a, b});
    auto desired = document();
    auto current = fixture.scene(desired);
    current->activate();
    fixture.prime(*current);
    fixture.poll(current.get());
    float sample;
    for (unsigned iteration = 0; iteration < 8; ++iteration) {
        std::reverse(desired.devices[0].monitors.begin(), desired.devices[0].monitors.end());
        auto next = fixture.scene(desired);
        assert(a->fire(sample));
        near(sample, 0.2f);
        assert(b->fire(sample));
        near(sample, 0.1f);
        next->activate();
        // A list-only change must read the already warm shared bus immediately,
        // before any new scene block is available or any control reopen.
        assert(a->fire(sample));
        near(sample, 0.2f);
        assert(b->fire(sample));
        near(sample, 0.1f);
        fixture.poll(next.get(), 1000 + iteration);
        assert(a->opens == 1 && b->opens == 1 && !a->stops && !b->stops);
        assert(fixture.factories == 2 && fixture.outputs.restored());
        current = std::move(next);
    }
}

void unrelatedSceneChangesRetainMonitorPrograms() {
    auto a = std::make_shared<Facts>(), b = std::make_shared<Facts>("speakers", 2, 44100);
    Fixture fixture({a, b});
    auto desired = document();
    auto current = fixture.scene(desired);
    current->activate();
    fixture.prime(*current);
    fixture.poll(current.get());
    float sample;
    for (unsigned callback = 0; callback < 16; ++callback) {
        assert(a->fire(sample));
        assert(b->fire(sample));
    }
    for (unsigned edit = 0; edit < 8; ++edit) {
        const auto before = fixture.outputs.status();
        AudioMeterSnapshot meter;
        assert(before[0].audio.sourceOnline && before[1].audio.sourceOnline);
        assert(fixture.outputs.meters("mix", "one", meter));
        const auto peak = meter.peak[0];
        desired.devices[0].name = "Renamed mix " + std::to_string(edit);
        desired.devices[0].sampleRate = edit % 2 ? 48000 : 44100;
        std::reverse(desired.devices[0].monitors.begin(), desired.devices[0].monitors.end());
        auto rejected = fixture.scene(desired);
        rejected.reset();
        assert(fixture.outputs.status()[0].audio.callbacks == before[0].audio.callbacks);
        auto next = fixture.scene(desired);
        next->activate();
        const auto after = fixture.outputs.status();
        // Observe before another hardware callback. An unchanged monitor must
        // retain its actual reader, meter and counters, rather than appear offline.
        for (size_t monitor = 0; monitor < before.size(); ++monitor) {
            assert(after[monitor].audio.callbacks == before[monitor].audio.callbacks);
            assert(after[monitor].audio.sourceOnline);
            assert(after[monitor].audio.missingFrames == before[monitor].audio.missingFrames);
        }
        assert(fixture.outputs.meters("mix", "one", meter));
        near(meter.peak[0], peak);
        assert(a->fire(sample));
        near(sample, 0.2f);
        assert(b->fire(sample));
        near(sample, 0.1f);
        fixture.poll(next.get(), 1000 + edit);
        assert(a->opens == 1 && b->opens == 1 && !a->stops && !b->stops);
        current = std::move(next);
    }
    // A real monitor edit still publishes its own new DSP program. Its peer
    // retains its reader and observations through that same scene commit.
    auto& edited = desired.devices[0].monitors[0];
    const auto changedID = edited.id;
    edited.gain *= 0.5f;
    const auto before = fixture.outputs.status();
    auto changed = fixture.scene(desired);
    changed->activate();
    const auto after = fixture.outputs.status();
    for (size_t monitor = 0; monitor < before.size(); ++monitor) {
        if (before[monitor].monitor == changedID)
            assert(after[monitor].audio.callbacks == 0);
        else
            assert(after[monitor].audio.callbacks == before[monitor].audio.callbacks);
    }
}

void identicalPendingMonitorFollowsPhysicalFormatRecovery() {
    auto facts = std::make_shared<Facts>();
    Fixture fixture({facts});
    auto desired = document(false);
    auto current = fixture.scene(desired);
    current->activate();
    fixture.prime(*current);
    fixture.poll(current.get());
    // An unrelated scene can be awaiting driver approval while the active
    // monitor reacquires hardware at a different rate. Its identical prepared
    // slot must follow the live program instead of restoring stale 48 kHz DSP.
    desired.devices[0].name = "Pending rename";
    auto pending = fixture.scene(desired);
    facts->inventory.devices[0].nominalSampleRate = 44100;
    fixture.poll(current.get(), 1100);
    fixture.poll(current.get(), 1200);
    assert(fixture.outputs.restored() && facts->opens == 2);
    float sample;
    for (unsigned callback = 0; callback < 16; ++callback) {
        assert(facts->fire(sample, 1200));
        near(sample, 0.2f);
    }
    const auto before = fixture.outputs.status()[0];
    pending->activate();
    const auto after = fixture.outputs.status()[0];
    assert(after.audio.callbacks == before.audio.callbacks && after.audio.sourceOnline);
    assert(after.sampleRate == 44100 && after.complete);
    assert(facts->fire(sample, 1201));
    near(sample, 0.2f);
    assert(facts->opens == 2 && facts->stops == 1);
}

void noEnabledMonitorNeedsNoHistoryOrHardware() {
    auto facts = std::make_shared<Facts>();
    Fixture fixture({facts});
    auto desired = document(false);
    const auto monitor = desired.devices[0].monitors[0];
    auto capture = std::make_shared<Capture>();
    for (unsigned mode = 0; mode < 3; ++mode) {
        auto definition = desired.devices[0];
        if (mode == 0)
            definition.monitors.clear();
        else if (mode == 1)
            definition.monitors[0].enabled = false;
        else
            definition.routing.enabled = false;
        heapEvents = 0;
        trackHeap = true;
        auto decorated = fixture.outputs.decorate(definition, capture);
        trackHeap = false;
        assert(decorated == capture && heapEvents == 0 && fixture.factories == 0);
    }
    auto current = fixture.scene(desired);
    current->activate();
    fixture.prime(*current);
    fixture.poll(current.get());
    assert(facts->opens == 1);
    desired.devices[0].monitors.clear();
    auto pending = fixture.scene(desired);
    fixture.poll(current.get(), 1001);
    float sample;
    assert(facts->fire(sample, 1001));
    near(sample, .2f);
    pending.reset(); // Preparing or rejecting the edit cannot stop the old monitor.
    assert(facts->stops == 0);
    auto next = fixture.scene(desired);
    next->activate();
    fixture.poll(next.get(), 1002);
    assert(facts->stops == 1 && facts->state == AudioHostState::Closed);
    desired.devices[0].monitors = {monitor};
    auto restored = fixture.scene(desired);
    restored->activate();
    fixture.prime(*restored);
    fixture.poll(restored.get(), 1003);
    assert(facts->opens == 2 && fixture.outputs.restored());
    assert(facts->fire(sample, 1003));
    near(sample, .2f);
}

void boundedMetadataKeepsCallbackHealthAndRecoveryImmediate() {
    auto facts = std::make_shared<Facts>();
    Fixture fixture(
        {facts}, 1024, 2,
        [] {
            return true;
        },
        250);
    auto current = fixture.scene(document(false));
    current->activate();
    fixture.prime(*current);
    fixture.poll(current.get(), 1000);
    assert(fixture.outputs.restored());
    float sample;
    assert(facts->fire(sample, 1001));
    fixture.poll(current.get(), 1001);
    const auto queries = facts->enumerations;
    for (uint64_t now = 1010; now < 1250; now += 10) {
        assert(facts->fire(sample, now));
        fixture.poll(current.get(), now);
    }
    assert(facts->enumerations == queries && facts->opens == 1);
    fixture.poll(current.get(), 1251);
    assert(facts->enumerations > queries && facts->opens == 1);

    const auto refreshed = facts->enumerations;
    fixture.outputs.retryNow(1252);
    fixture.poll(current.get(), 1252);
    assert(facts->enumerations > refreshed);

    // A stopped stream must be noticed inside the metadata-cache window.
    facts->state = AudioHostState::Error;
    fixture.poll(current.get(), 1253);
    assert(facts->stops == 1 && !fixture.outputs.restored());
}

int main() {
    boundedMetadataKeepsCallbackHealthAndRecoveryImmediate();
    noEnabledMonitorNeedsNoHistoryOrHardware();
    sharedClockMonitoringKeepsControlsAndRecoversModes();
    optInPeriodReservePreservesFloorsAndRateConversion();
    identicalPendingMonitorFollowsPhysicalFormatRecovery();
    unrelatedSceneChangesRetainMonitorPrograms();
    monitorOrderRetainsStreamsAndBufferedAudio();
    hardwareBufferChangesRebuildMonitorTiming();
    committedOwnershipAndIndependentRecovery();
    partialProfileDefaultAndPendingRateChange();
    failuresDeadlineAndClockStall();
    independentAcquisitionDeadlines();
    hardwareClockSurvivesEditsAndResetsWithStream();
    rendererActivationAndConcurrentEdits();
    std::puts(
        "Monitor ownership, staged activation, independent recovery and concurrent edits passed");
}
