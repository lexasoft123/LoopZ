#include <loopz/macos/routing_service.hpp>
#include <loopz/macos/engine_runtime.hpp>
#include <loopz/macos/capture.hpp>
#include <loopz/macos/transport_message.hpp>
#include "test_code_identity.hpp"
#include "../../../native/platform/macos/routing_service/connection.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <dispatch/dispatch.h>

namespace {
using namespace loopz;
using namespace loopz::macos;
using Phase = DriverConfigurationPhase;
uint64_t now() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}
template <typename Predicate> void wait(Predicate predicate, uint64_t limit = 10000) {
    const auto end = now() + limit;
    do {
        if (predicate())
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (now() < end);
    assert(false && "Routing service condition timed out");
}
struct Native {
    std::mutex mutex;
    uint64_t latest{0};
    std::map<uint64_t, DriverConfigurationResult> outcomes;
    std::vector<RemoteDeviceTransport> devices;
    std::atomic<unsigned> applications{0};
    std::atomic<bool> available{false};
    bool echo{false};
    std::atomic<unsigned> delayMs{0}, statuses{0};
    std::atomic<bool> holdStatus{false};
    RoutingDriverOperations operations() {
        return {[this](xpc_object_t message) -> int32_t {
                    auto snapshot = DeviceTransportSnapshot::decode(message);
                    if (echo) {
                        for (const auto& device : snapshot.devices) {
                            const auto view = device.view();
                            std::vector<float> samples(view.channels(), 0);
                            if (view.readCapture(123, samples, 1, 1000000000).missingFrames ||
                                !std::all_of(samples.begin(), samples.end(), [](float x) {
                                    return x == 0.25f;
                                }))
                                return -2;
                            std::fill(samples.begin(), samples.end(), -0.5f);
                            view.header().driverOnline.store(1);
                            view.header().driverHeartbeatNs.store(1000000000);
                            if (!view.writePlayback(456, samples, 1))
                                return -3;
                        }
                    }
                    std::lock_guard lock(mutex);
                    if (snapshot.revision <= latest)
                        return -1;
                    latest = snapshot.revision;
                    devices = std::move(snapshot.devices);
                    outcomes[latest] = {latest, Phase::Committed, 0};
                    ++applications;
                    if (const auto delay = delayMs.exchange(0))
                        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                    return 0;
                },
                [this](uint64_t revision) {
                    ++statuses;
                    const auto deadline = now() + 10000;
                    while (holdStatus.load() && now() < deadline)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    std::lock_guard lock(mutex);
                    if (!revision)
                        revision = latest;
                    if (!revision)
                        return DriverConfigurationResult{0, Phase::Committed, 0};
                    const auto found = outcomes.find(revision);
                    return found == outcomes.end()
                               ? DriverConfigurationResult{revision, Phase::Unknown, 0}
                               : found->second;
                },
                [this](uint64_t revision) -> int32_t {
                    std::lock_guard lock(mutex);
                    const auto found = outcomes.find(revision);
                    if (found == outcomes.end())
                        return -1;
                    found->second.phase = Phase::Aborted;
                    return 0;
                },
                [this](bool ready) {
                    available.store(ready);
                }};
    }
};
RoutingBrokerPolicy policy(const std::string& requirement,
                           const std::shared_ptr<std::atomic<uint32_t>>& console) {
    return {requirement, requirement, static_cast<uint32_t>(geteuid()), false,
            [console]() -> std::optional<uint32_t> {
                return console->load();
            }};
}
RoutingServiceEndpoint endpoint(RoutingBroker& broker, const std::string& requirement) {
    RoutingServiceEndpoint result;
    result.endpoint = broker.endpoint();
    result.serverRequirement = requirement;
    result.serverUID = geteuid();
    result.privileged = false;
    return result;
}
RouteDocument document() {
    RouteDocument document;
    for (unsigned index = 0; index < 3; ++index) {
        VirtualDeviceDefinition device;
        device.id = "rpc-" + std::to_string(index);
        device.name = device.id;
        device.routing.outputChannels = index == 2 ? 8 : 2;
        document.devices.push_back(std::move(device));
    }
    return document;
}
template <typename Link> void restore(Link& link, ConfigurationController& controller) {
    const auto deadline = now() + 10000;
    do {
        link.poll(now());
        controller.poll(now());
        if (controller.status().phase == RestorationPhase::Restored)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (now() < deadline);
    const auto status = controller.status();
    std::fprintf(stderr, "Restore timeout, session %llu, phase %u: %s; link: %s\n",
                 static_cast<unsigned long long>(link.session()), unsigned(status.phase),
                 status.detail.c_str(), link.lastError().c_str());
    assert(false && "Configuration did not restore through XPC");
}
void roundTripAndRecovery(const std::string& requirement) {
    auto console = std::make_shared<std::atomic<uint32_t>>(geteuid());
    auto broker = RoutingBroker::anonymous(policy(requirement, console));
    auto target = endpoint(*broker, requirement);
    Native native;
    auto driver = std::make_unique<RoutingDriverService>(target, native.operations());
    RoutingDriverLink link(target);
    ConfigurationController controller(link);
    controller.setDesired(document());
    restore(link, controller);
    assert(native.available.load() && native.applications.load() == 1);
    const auto firstSession = link.session();
    assert(firstSession && controller.devices().size() == 3);
    const auto original = controller.devices();
    // Real XPC port transfer carries private shared-memory aliases through
    // helper -> broker -> driver, without touching installed devices.
    {
        const auto local = original[2].region->view();
        std::lock_guard lock(native.mutex);
        const auto remote = native.devices[2].view();
        std::array<float, 8> samples{}, output{};
        samples.fill(0.25f);
        local.header().helperOnline.store(1);
        local.header().helperHeartbeatNs.store(1000000000);
        assert(local.writeCapture(123, samples, 1));
        assert(remote.readCapture(123, output, 1, 1000000000).missingFrames == 0);
        assert(output == samples);
        remote.header().driverOnline.store(1);
        remote.header().driverHeartbeatNs.store(1000000000);
        samples.fill(-0.5f);
        assert(remote.writePlayback(456, samples, 1));
        assert(local.readPlayback(456, output, 1, 1000000000).missingFrames == 0);
        assert(output == samples);
    }
    RoutingDriverLink competitor(target);
    competitor.poll(now());
    assert(!competitor.session() && link.session() == firstSession);
    assert(!original[0].region->header().retired.load());

    driver->stop();
    wait([&] {
        return !link.session();
    });
    controller.poll(now());
    assert(controller.status().phase == RestorationPhase::WaitingForDriver);
    assert(!original[0].region->header().retired.load());
    driver = std::make_unique<RoutingDriverService>(target, native.operations());
    restore(link, controller);
    assert(link.session() > firstSession);
    for (size_t i = 0; i < original.size(); ++i)
        assert(controller.devices()[i].region == original[i].region);

    const auto secondSession = link.session();
    link.invalidate();
    wait([&] {
        return original[0].region->header().retired.load();
    });
    restore(link, controller);
    assert(link.session() > secondSession);
    for (size_t i = 0; i < original.size(); ++i)
        assert(controller.devices()[i].region != original[i].region);
    auto current = controller.devices();
    console->store(geteuid() + 1);
    wait([&] {
        return !link.session() && current[0].region->header().retired.load();
    });
    assert(!native.available.load());
    console->store(geteuid());
    restore(link, controller);
    assert(controller.devices().size() == 3 &&
           !controller.devices()[0].region->header().retired.load());
    current = controller.devices();
    broker->stop();
    wait([&] {
        return !link.session();
    });
    assert(current[0].region->header().retired.load());
    driver->stop();
}
void uncertainReplyAndDriverLoss(const std::string& requirement) {
    auto console = std::make_shared<std::atomic<uint32_t>>(geteuid());
    auto broker = RoutingBroker::anonymous(policy(requirement, console));
    auto target = endpoint(*broker, requirement);
    Native native;
    auto driver = std::make_unique<RoutingDriverService>(target, native.operations());
    RoutingDriverLink link(target);
    ConfigurationController controller(link);
    auto desired = document();
    controller.setDesired(desired);
    restore(link, controller);
    native.delayMs.store(2500);
    resizeDeviceOutputs(desired, desired.devices[0].id, 4);
    controller.setDesired(desired);
    controller.poll(now());
    assert(controller.status().phase == RestorationPhase::Applying);
    assert(!controller.status().detail.empty()); // Relay timeout, outcome uncertain.
    restore(link, controller);
    assert(controller.devices()[0].region->view().channels() == 4);
    const auto active = controller.devices();
    native.delayMs.store(1000);
    resizeDeviceOutputs(desired, desired.devices[0].id, 8);
    controller.setDesired(desired);
    std::thread stopDriver([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        driver->stop();
    });
    controller.poll(now());
    stopDriver.join();
    // stop() drains the driver's queue; remote invalidation is delivered on
    // the broker and helper queues independently of that thread's completion.
    wait([&] {
        return !link.session();
    });
    controller.poll(now());
    assert(controller.status().phase == RestorationPhase::WaitingForDriver);
    assert(!active[0].region->header().retired.load());
    driver = std::make_unique<RoutingDriverService>(target, native.operations());
    restore(link, controller);
    assert(controller.devices()[0].region->view().channels() == 8);
    assert(controller.devices()[1].region == active[1].region);
}
void installedDiscoveryAndSessionReplacement(const std::string& requirement) {
    auto console = std::make_shared<std::atomic<uint32_t>>(geteuid());
    auto broker = RoutingBroker::anonymous(policy(requirement, console));
    auto target = endpoint(*broker, requirement);
    Native native;
    RoutingDriverService driver(target, native.operations());
    bool trusted = false;
    InstalledDriverLink link([&] {
        if (!trusted)
            throw std::runtime_error("Untrusted installed pin fixture");
        return target;
    });
    ConfigurationController controller(link);
    controller.setDesired(document());
    link.poll(now());
    controller.poll(now());
    assert(!link.session() && !link.lastError().empty() && native.applications.load() == 0);
    trusted = true;
    restore(link, controller);
    const auto first = link.session();
    const auto old = controller.devices();
    trusted = false;
    wait([&] {
        link.poll(now());
        return !link.session();
    });
    controller.poll(now());
    assert(controller.status().phase == RestorationPhase::WaitingForDriver);
    wait([&] {
        return old[0].region->header().retired.load();
    });
    trusted = true;
    restore(link, controller);
    assert(link.session() > first && controller.devices().size() == old.size());
    assert(controller.devices()[0].region != old[0].region);
}
void boundedOutstandingReplies(const std::string& requirement) {
    auto console = std::make_shared<std::atomic<uint32_t>>(geteuid());
    auto broker = RoutingBroker::anonymous(policy(requirement, console));
    const auto target = endpoint(*broker, requirement);
    Native native;
    RoutingDriverService driver(target, native.operations());
    routing::Connection helper(target, "helper");
    std::string error;
    assert(helper.connect(error));
    wait([&] {
        return helper.session() != 0;
    });
    native.holdStatus.store(true);
    std::atomic<unsigned> timedOut{0};
    std::vector<std::thread> requests;
    // Deliberately flood one authenticated peer. Connection lifecycle remains
    // serialized; XPC sends/replies and snapshot reads may overlap.
    for (unsigned index = 0; index < 16; ++index)
        requests.emplace_back([&] {
            try {
                (void)helper.request("status", nullptr, 0);
            } catch (const std::runtime_error&) {
                ++timedOut;
            }
        });
    for (auto& request : requests)
        request.join();
    assert(timedOut.load() == 16 && native.statuses.load() == 1);
    const auto started = now();
    bool rejected = false;
    try {
        (void)helper.request("status", nullptr, 0);
    } catch (const std::runtime_error& failure) {
        rejected = std::string(failure.what()).find("request limit") != std::string::npos;
    }
    assert(rejected && now() - started < 1000 && native.statuses.load() == 1);
    native.holdStatus.store(false);
    wait([&] {
        return native.statuses.load() == 16;
    });
    wait([&] {
        try {
            (void)helper.request("status", nullptr, 0);
            return true;
        } catch (const std::runtime_error&) {
            return false;
        }
    });
}
class RuntimeInput final : public SourceEndpoint {
  public:
    uint32_t opens{0};
    bool opened{false};
    uint32_t channels() const noexcept override {
        return 8;
    }
    bool open(SourceOpenPolicy, std::string&) override {
        ++opens;
        opened = true;
        return true;
    }
    void close() noexcept override {
        opened = false;
    }
    bool healthy(std::string&) override {
        return opened;
    }
    AudioReadResult read(uint64_t, double, RoutingOutput output, uint32_t frames,
                         AudioReadCursor&) const noexcept override {
        for (auto* plane : output.channels)
            std::fill_n(plane, frames, 0.25f);
        return {0, true};
    }
};
void persistentRuntimeThroughAuthenticatedService(const std::string& requirement) {
    auto console = std::make_shared<std::atomic<uint32_t>>(geteuid());
    auto broker = RoutingBroker::anonymous(policy(requirement, console));
    auto target = endpoint(*broker, requirement);
    Native native;
    auto driver = std::make_unique<RoutingDriverService>(target, native.operations());
    RoutingDriverLink link(target);
    auto input = std::make_shared<RuntimeInput>();
    unsigned factories = 0;
    EngineRuntime runtime(link, {}, [&](SourceKind, const std::string&, uint32_t channels) {
        assert(channels == 8);
        ++factories;
        return input;
    });
    auto desired = document();
    for (auto& device : desired.devices) {
        const auto channels = device.routing.outputChannels;
        device.routing.sources = {
            {"source", channels, 1, true, false, defaultChannelConnections(channels, channels)}};
        device.bindings = {{"source", SourceKind::InputDevice, "synthetic"}};
    }
    // Like real HAL IO, client liveness must continue while control prepares
    // DSP or waits on authenticated requests. A control-thread heartbeat would
    // manufacture client loss when instrumented preparation takes >250 ms.
    std::jthread io([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            {
                std::lock_guard lock(native.mutex);
                for (const auto& device : native.devices) {
                    device.view().header().driverOnline.store(native.available.load());
                    device.view().header().driverHeartbeatNs.store(hostNowNs());
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    runtime.setDesired(desired);
    runtime.start();
    const auto pump = [&] {
        runtime.poll();
    };
    const auto audible = [&](float second) {
        const auto end = runtime.status().render.publishedFrame;
        if (end < 32)
            return false;
        std::lock_guard lock(native.mutex);
        if (native.devices.size() != 3)
            return false;
        for (size_t index = 0; index < native.devices.size(); ++index) {
            const auto view = native.devices[index].view();
            std::array<float, 8> samples{};
            const auto result =
                view.readCapture(end - 32, {samples.data(), view.channels()}, 1, hostNowNs(),
                                 kDeviceAudioConcurrentHeartbeatSkewNs);
            if (!result.validBlock || result.missingFrames)
                return false;
            for (uint32_t channel = 0; channel < view.channels(); ++channel)
                if (std::fabs(samples[channel] - (index == 1 ? second : 0.25f)) > 0.00001f)
                    return false;
        }
        return true;
    };
    wait([&] {
        pump();
        return runtime.status().restored && audible(0.25f);
    });
    assert(factories == 1 && input->opens == 1);
    desired.devices[1].routing.gain = 0.5f;
    runtime.setDesired(desired);
    wait([&] {
        pump();
        return runtime.status().restored && audible(0.125f);
    });
    assert(factories == 1 && input->opens == 1);
    driver->stop();
    wait([&] {
        pump();
        return runtime.status().configuration.phase == RestorationPhase::WaitingForDriver;
    });
    driver = std::make_unique<RoutingDriverService>(target, native.operations());
    runtime.retryNow();
    wait([&] {
        pump();
        return runtime.status().restored && audible(0.125f);
    });
    assert(factories == 1 && runtime.observe()->document().devices.size() == 3);
    runtime.stop();
}
void identityRejection(const std::string& requirement) {
    auto console = std::make_shared<std::atomic<uint32_t>>(geteuid());
    {
        auto configuration = policy(requirement, console);
        configuration.helperRequirement = "identifier \"com.lexasoft.loopz.never-signed-this\"";
        auto broker = RoutingBroker::anonymous(std::move(configuration));
        RoutingDriverLink helper(endpoint(*broker, requirement));
        helper.poll(now());
        assert(!helper.session() && !helper.lastError().empty());
    }
    {
        auto broker = RoutingBroker::anonymous(policy(requirement, console));
        auto wrongServer = endpoint(*broker, "identifier \"com.lexasoft.loopz.wrong-server\"");
        RoutingDriverLink helper(wrongServer);
        helper.poll(now());
        assert(!helper.session() && !helper.lastError().empty());
    }
    {
        auto broker = RoutingBroker::anonymous(policy(requirement, console));
        auto wrongUID = endpoint(*broker, requirement);
        ++wrongUID.serverUID;
        RoutingDriverLink helper(wrongUID);
        helper.poll(now());
        assert(!helper.session() && !helper.lastError().empty());
    }
}
RoutingServiceEndpoint namedEndpoint(const std::string& name, const std::string& requirement) {
    RoutingServiceEndpoint result;
    result.serviceName = name;
    result.serverRequirement = requirement;
    result.serverUID = geteuid();
    result.privileged = false;
    return result;
}
class EchoLink final : public DriverConfigurationLink {
  public:
    RoutingDriverLink link;
    explicit EchoLink(RoutingServiceEndpoint target) : link(std::move(target)) {}
    uint64_t session() const noexcept override {
        return link.session();
    }
    int32_t submit(std::span<const DeviceTransport> devices, uint64_t revision) override {
        for (const auto& device : devices) {
            const auto view = device.region->view();
            std::vector<float> samples(view.channels(), 0.25f);
            view.header().helperOnline.store(1);
            view.header().helperHeartbeatNs.store(1000000000);
            assert(view.writeCapture(123, samples, 1));
        }
        return link.submit(devices, revision);
    }
    DriverConfigurationResult status(uint64_t revision) override {
        return link.status(revision);
    }
    int32_t cancel(uint64_t revision) override {
        return link.cancel(revision);
    }
    void invalidate() noexcept override {
        link.invalidate();
    }
};
void helperFixture(const std::string& name, const std::string& requirement) {
    EchoLink peer(namedEndpoint(name, requirement));
    ConfigurationController controller(peer);
    auto desired = document();
    resizeDeviceOutputs(desired, desired.devices[2].id, 64);
    auto restore = [&] {
        wait(
            [&] {
                peer.link.poll(now());
                controller.poll(now());
                return controller.status().phase == RestorationPhase::Restored;
            },
            20000);
        for (const auto& device : controller.devices()) {
            const auto view = device.region->view();
            std::vector<float> samples(view.channels(), 0);
            assert(view.readPlayback(456, samples, 1, 1000000000).missingFrames == 0);
            assert(std::all_of(samples.begin(), samples.end(), [](float x) {
                return x == -0.5f;
            }));
        }
    };
    controller.setDesired(desired);
    restore();
    resizeDeviceOutputs(desired, desired.devices[0].id, 8);
    controller.setDesired(desired);
    restore();
    assert(controller.devices().size() == 3 &&
           controller.devices()[2].region->view().channels() == 64);
    std::printf("{\"fixture\":\"helper\",\"pid\":%d,\"devices\":3,\"passed\":true}\n", getpid());
}
} // namespace
int main(int argc, char** argv) {
    @autoreleasepool {
        const auto requirement = selfCodeRequirement();
        if (argc == 3) {
            const std::string mode = argv[1];
            const std::string name = argv[2];
            if (mode == "--broker-fixture") {
                auto console = std::make_shared<std::atomic<uint32_t>>(geteuid());
                (void)RoutingBroker::machService(policy(requirement, console), name).release();
                dispatch_main();
            }
            if (mode == "--driver-fixture") {
                auto* native = new Native;
                native->echo = true;
                (void)new RoutingDriverService(namedEndpoint(name, requirement),
                                               native->operations());
                dispatch_main();
            }
            if (mode == "--helper-fixture") {
                helperFixture(name, requirement);
                return 0;
            }
            return 2;
        }
        assert(argc == 1);
        roundTripAndRecovery(requirement);
        uncertainReplyAndDriverLoss(requirement);
        installedDiscoveryAndSessionReplacement(requirement);
        persistentRuntimeThroughAuthenticatedService(requirement);
        boundedOutstandingReplies(requirement);
        identityRejection(requirement);
        std::puts("Routing service: authenticated XPC relay, memory aliases, exclusive ownership, "
                  "disconnect recovery and identity rejection passed");
    }
}
