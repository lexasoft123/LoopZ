#include <loopz/runtime/audio_runtime.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <vector>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace {
using namespace loopz;
using namespace loopz::runtime;
struct Device {
    unsigned opens{0}, stops{0}, live{0};
    bool offline{false}, failure{false};
    uint32_t channels{0};
    uint64_t generation{1};
    double rate{48000};
    singz::AudioInputBusPush push{nullptr};
    void* context{nullptr};
    std::thread::id stoppedOn;
};
struct State {
    std::map<std::string, Device> devices;
    unsigned outputOpens{0};
    bool listed{true};
    uint32_t inventoryChannels{10};
    unsigned opens() const {
        unsigned result = 0;
        for (const auto& [uid, device] : devices) {
            (void)uid;
            result += device.opens;
        }
        return result;
    }
};
class Host final : public singz::AudioHostBackend {
    std::shared_ptr<State> state_;

  public:
    explicit Host(std::shared_ptr<State> state) : state_(std::move(state)) {}
    singz::AudioHostInventory enumerate() const override {
        singz::AudioHostInventory result;
        result.defaultInputUid = "mic";
        if (state_->listed) {
            singz::AudioHostDeviceInfo device;
            device.uid = "mic";
            device.inputChannels = state_->inventoryChannels;
            device.outputChannels = 2;
            result.devices.push_back(device);
        }
        return result;
    }
    singz::AudioHostResult open(const singz::AudioHostConfig&, singz::AudioHostRender,
                                void*) override {
        ++state_->outputOpens;
        throw std::runtime_error("Private input must not open any output");
    }
    singz::AudioHostResult start() override {
        return {};
    }
    void stop() noexcept override {}
    singz::AudioHostStatus status() const noexcept override {
        return {};
    }
};
class Input final : public singz::AudioInputBackend {
    std::shared_ptr<State> state_;
    Device* device_{nullptr};
    bool opened_{false};

  public:
    explicit Input(std::shared_ptr<State> state) : state_(std::move(state)) {}
    singz::AudioInputResult open(const singz::AudioInputConfig&, singz::AudioInputPush,
                                 void*) override {
        throw std::runtime_error("Subscription must use shared planar capture");
    }
    singz::AudioInputBusResult openBus(const singz::AudioInputBusConfig& config,
                                       singz::AudioInputBusPush push, void* context) override {
        device_ = &state_->devices[config.deviceUid];
        ++device_->opens;
        if (device_->offline)
            return {singz::AudioInputBusOpenStatus::Error,
                    singz::AudioInputResult::failure(singz::AudioInputState::Error, "Offline", 0)};
        assert(!device_->live); // negative control: a second ALSA owner is forbidden
        opened_ = true;
        ++device_->live;
        device_->channels = static_cast<uint32_t>(config.channels.size());
        device_->push = push;
        device_->context = context;
        return {
            singz::AudioInputBusOpenStatus::Ready,
            singz::AudioInputResult::success(singz::AudioInputState::Starting, device_->rate, 0)};
    }
    singz::AudioInputResult start() override {
        return singz::AudioInputResult::success(singz::AudioInputState::Running, 48000, 0);
    }
    void stop() override {
        if (!opened_)
            return;
        assert(device_->live == 1);
        opened_ = false;
        --device_->live;
        ++device_->stops;
        device_->stoppedOn = std::this_thread::get_id();
        device_->push = nullptr;
        device_->context = nullptr;
    }
    bool takeFailure(std::string& error) override {
        if (!device_ || !device_->failure)
            return false;
        device_->failure = false;
        error = "Disconnected";
        return true;
    }
};
AudioRuntime runtime(const std::shared_ptr<State>& state) {
    return AudioRuntime(
        [state] {
            return std::make_unique<Host>(state);
        },
        [state] {
            return std::make_unique<Input>(state);
        });
}
template <typename F> void rejects(F operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::exception&) {
        rejected = true;
    }
    assert(rejected);
}
RouteDocument route(std::string uid, uint32_t channels = 2) {
    VirtualDeviceDefinition device;
    device.id = "bus";
    device.name = "Input";
    SourceRouting input;
    input.id = "mic";
    input.channels = channels;
    input.connections = defaultChannelConnections(channels, 2);
    device.routing.sources.push_back(input);
    device.bindings.push_back({"mic", SourceKind::InputDevice, std::move(uid), false});
    return {kRouteDocumentVersion, {device}};
}
void drain(AudioRuntime& engine, const Device& device) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (device.live && std::chrono::steady_clock::now() < deadline) {
        engine.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(!device.live);
}
void feed(Device& device) {
    std::array<std::array<float, 256>, 64> samples{};
    std::array<const float*, 64> planes{};
    for (uint32_t channel = 0; channel < device.channels; ++channel) {
        samples[channel].fill(float(channel + 1) / 10.f);
        planes[channel] = samples[channel].data();
    }
    for (uint64_t index = 0; index < 16; ++index) {
        singz::AudioInputBusBlock block;
        block.planes = planes.data();
        block.channelCount = device.channels;
        block.frames = block.capacityFrames = 256;
        block.firstNs = 1000000000 + index * 256 * 1000000000 / 48000;
        block.deliveryNs = block.firstNs;
        block.quality = singz::AudioInputTimestampQuality::CallbackEstimate;
        block.capture = {1,
                         device.generation,
                         index,
                         index * 256,
                         block.firstNs,
                         block.deliveryNs,
                         block.quality,
                         singz::AudioInputDiscontinuityReason::None,
                         singz::AudioInputSourceFrameValid | singz::AudioInputSampleHostTimeValid};
        assert(device.push && device.push(device.context, block));
    }
}
void privateAndPublicShare() {
    auto state = std::make_shared<State>();
    auto engine = runtime(state);
    const auto one = engine.acquireInput("mic", 2);
    const auto two = engine.acquireInput("mic", 1);
    assert(one.id != two.id && one.source == two.source && one.channels == 2 && two.channels == 2);
    assert(state->opens() == 1 && state->outputOpens == 0);
    assert(!engine.status().active && engine.status().inputs.empty());
    assert(engine.inputStatus(one.id)->connected);
    feed(state->devices.at("mic"));
    std::array<float, 256> a{}, b{};
    std::array<float*, 1> planeA{a.data()}, planeB{b.data()};
    std::array<uint32_t, 1> selected{1};
    AudioReadCursor cursorA, cursorB;
    const uint64_t time = 1000000000 + uint64_t(1024) * 1000000000 / 48000;
    const auto readA =
        one.source->readChannels(time, 48000, {planeA, 256}, 256, cursorA, selected, {});
    const auto readB =
        two.source->readChannels(time, 48000, {planeB, 256}, 256, cursorB, selected, {});
    assert(readA && readB && !readA->missingFrames && !readB->missingFrames && a == b);
    assert(std::all_of(a.begin(), a.end(), [](float value) {
        return std::abs(value - 0.2f) < 0.0001f;
    }));
    engine.commit(engine.prepare(route("mic")));
    assert(state->opens() == 1);
    assert(engine.releaseInput(one.id) && !engine.releaseInput(one.id));
    assert(!engine.inputStatus(one.id) && state->devices.at("mic").live == 1);
    // Public removal cannot close the subscribed physical owner.
    engine.commit(engine.prepare({}));
    engine.poll();
    assert(state->devices.at("mic").live == 1 && engine.inputStatus(two.id)->connected);
    // Re-add the public route; releasing its final private reader leaves it on.
    engine.commit(engine.prepare(route("mic")));
    assert(engine.releaseInput(two.id) && state->opens() == 1);
    assert(state->devices.at("mic").live == 1);
    engine.commit(engine.prepare({}));
    drain(engine, state->devices.at("mic"));
    assert(state->devices.at("mic").stops == 1);
    assert(state->devices.at("mic").stoppedOn == std::this_thread::get_id());
}
void retiredCaptureAndFailureIsolation() {
    auto state = std::make_shared<State>();
    auto engine = runtime(state);
    engine.commit(engine.prepare(route("mic")));
    engine.commit(engine.prepare({}));
    const auto lease = engine.acquireInput("mic", 1);
    assert(state->opens() == 1 && state->devices.at("mic").live == 1);
    engine.poll();
    state->devices["missing"].offline = true;
    rejects([&] {
        engine.prepare(route("missing"));
    });
    assert(engine.inputStatus(lease.id)->connected && state->devices.at("mic").live == 1);
    assert(engine.status().inputs.empty() && state->outputOpens == 0);
    assert(engine.releaseInput(lease.id));
    drain(engine, state->devices.at("mic"));
    // Initial acquisition can retain an offline-only lease without a scene.
    const auto offline = engine.acquireInput("missing", 2);
    assert(!engine.inputStatus(offline.id)->connected &&
           !engine.inputStatus(offline.id)->error.empty());
    assert(engine.releaseInput(offline.id));
}
void offlineRetryAndInventory() {
    auto state = std::make_shared<State>();
    auto engine = runtime(state);
    const auto lease = engine.acquireInput("mic", 10);
    state->inventoryChannels = 0; // exclusive ALSA query sees EBUSY
    assert(engine.inventory().devices[0].inputChannels == 10);
    assert(engine.inventory().devices[0].outputChannels == 2);
    state->inventoryChannels = 12;
    assert(engine.inventory().devices[0].inputChannels == 12);
    state->listed = false;
    assert(engine.inventory().devices.empty());
    state->listed = true;
    state->inventoryChannels = 0;
    auto& device = state->devices.at("mic");
    feed(device);
    assert(engine.inputStatus(lease.id)->captureHistory.online);
    device.offline = device.failure = true;
    engine.poll();
    assert(!engine.inputStatus(lease.id)->connected);
    assert(engine.inventory().devices[0].inputChannels == 0);
    std::array<float, 256> pcm{};
    std::array<float*, 1> output{pcm.data()};
    AudioReadCursor cursor;
    const auto missing = lease.source->read(1000000000, 48000, {output, 256}, 256, cursor);
    assert(!missing.online && missing.missingFrames == 256);
    device.offline = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!engine.inputStatus(lease.id)->connected &&
           std::chrono::steady_clock::now() < deadline) {
        engine.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(engine.inputStatus(lease.id)->connected && !engine.status().active);
    assert(!engine.inputStatus(lease.id)->captureHistory.online); // stale PCM never replayed
    ++device.generation;
    feed(device);
    assert(engine.inputStatus(lease.id)->captureHistory.online && state->outputOpens == 0);
    assert(engine.releaseInput(lease.id) && !device.live);
}
void limitsAndDefaultGuards() {
    auto state = std::make_shared<State>();
    auto engine = runtime(state);
    rejects([&] {
        engine.acquireInput("default", 1);
    });
    rejects([&] {
        engine.acquireInput("", 1);
    });
    rejects([&] {
        engine.acquireInput("mic", 0);
    });
    const auto narrow = engine.acquireInput("mic", 2);
    rejects([&] {
        engine.acquireInput("mic", 3);
    });
    rejects([&] {
        engine.prepare(route("mic", 4));
    });
    rejects([&] {
        engine.prepare(route("default"));
    });
    assert(state->opens() == 1);
    assert(engine.releaseInput(narrow.id));
    engine.commit(engine.prepare(route("default")));
    rejects([&] {
        engine.acquireInput("mic", 2);
    });
    engine.commit(engine.prepare({}));
    // The predecessor remains live until poll/scene borrowers retire it.
    rejects([&] {
        engine.acquireInput("mic", 2);
    });
    drain(engine, state->devices.at("mic"));
    std::vector<InputSubscription> leases;
    for (unsigned i = 0; i < RuntimeLimits::maximumInputSubscriptions; ++i)
        leases.push_back(engine.acquireInput("mic", 2));
    const auto opens = state->opens();
    rejects([&] {
        engine.acquireInput("mic", 2);
    });
    assert(state->opens() == opens);
    for (const auto& lease : leases)
        assert(engine.releaseInput(lease.id));
    leases.clear();
    for (unsigned i = 0; i < RuntimeLimits::maximumPhysicalCaptures; ++i)
        leases.push_back(engine.acquireInput("input-" + std::to_string(i), 1));
    rejects([&] {
        engine.acquireInput("one-too-many", 1);
    });
    for (const auto& lease : leases)
        assert(engine.releaseInput(lease.id));
    const auto wideA = engine.acquireInput("wide-a", 64);
    const auto wideB = engine.acquireInput("wide-b", 64);
    rejects([&] {
        engine.acquireInput("over-channels", 1);
    });
    const auto duplicate = engine.acquireInput("wide-a", 32);
    assert(duplicate.channels == 64); // unique owner counts once
    assert(engine.releaseInput(wideA.id) && engine.releaseInput(wideB.id));
    assert(engine.releaseInput(duplicate.id));
    assert(state->outputOpens == 0);
}
template <typename F>
void pollFor(AudioRuntime& engine, std::chrono::milliseconds duration, F stop) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (!stop() && std::chrono::steady_clock::now() < deadline) {
        engine.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
void unsupportedCaptureRateRejectedAtOpen() {
    auto state = std::make_shared<State>();
    auto engine = runtime(state);
    // History accepts at most 192 kHz; a wider provider must not report a
    // connected capture that can never go online.
    state->devices["fast"].rate = 384000;
    const auto fast = engine.acquireInput("fast", 2);
    const auto fastStatus = engine.inputStatus(fast.id);
    assert(!fastStatus->connected && !state->devices.at("fast").live);
    assert(fastStatus->error.find("sample rate") != std::string::npos);
    state->devices["edge"].rate = 192000;
    const auto edge = engine.acquireInput("edge", 2);
    assert(engine.inputStatus(edge.id)->connected && state->devices.at("edge").live == 1);
    assert(engine.releaseInput(fast.id) && engine.releaseInput(edge.id));
}
void stalledCaptureReopens() {
    auto state = std::make_shared<State>();
    auto engine = runtime(state);
    const auto lease = engine.acquireInput("mic", 2);
    auto& device = state->devices.at("mic");
    const auto never = [] {
        return false;
    };
    // Open time is the baseline: no first callback yet, still within 2 s.
    pollFor(engine, std::chrono::milliseconds(1200), never);
    assert(engine.inputStatus(lease.id)->connected && device.opens == 1 && !device.stops);
    feed(device);
    assert(engine.inputStatus(lease.id)->captureHistory.online);
    // 2.4 s after open but only 1.2 s after the latest delivery.
    pollFor(engine, std::chrono::milliseconds(1200), never);
    assert(engine.inputStatus(lease.id)->connected && device.opens == 1 && !device.stops);
    // The provider stops calling back without reporting failure. Detection
    // (2 s) plus the shared 2 s retry cadence bounds recovery.
    pollFor(engine, std::chrono::milliseconds(6000), [&] {
        return device.opens >= 2 && engine.inputStatus(lease.id)->connected;
    });
    assert(device.opens == 2 && device.stops == 1 && device.live == 1);
    const auto reopened = engine.inputStatus(lease.id);
    assert(reopened->connected && !reopened->captureHistory.online); // stale anchor retired
    // A second consecutive stall gets 4 s, so 3 s of silence after the reopen
    // must not close it again (without backoff it would close at 2 s).
    pollFor(engine, std::chrono::milliseconds(3000), never);
    assert(device.opens == 2 && device.stops == 1 && engine.inputStatus(lease.id)->connected);
    ++device.generation;
    feed(device);
    assert(engine.inputStatus(lease.id)->captureHistory.online);
    assert(engine.releaseInput(lease.id) && !device.live);
}
void rendererSchedulingIsObservable() {
    auto state = std::make_shared<State>();
    auto engine = runtime(state);
#if defined(__linux__)
    // The renderer must obtain SCHED_FIFO 19 exactly when this process may.
    bool permitted = false;
    std::thread probe([&] {
        sched_param parameters{};
        parameters.sched_priority = 19;
        permitted = pthread_setschedparam(pthread_self(), SCHED_FIFO, &parameters) == 0;
    });
    probe.join();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (permitted && !engine.status().rendererRealtime &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(engine.status().rendererRealtime == permitted);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(!engine.status().rendererRealtime);
#endif
}
} // namespace
int main() {
    privateAndPublicShare();
    retiredCaptureAndFailureIsolation();
    offlineRetryAndInventory();
    limitsAndDefaultGuards();
    unsupportedCaptureRateRejectedAtOpen();
    rendererSchedulingIsObservable();
    stalledCaptureReopens();
}
