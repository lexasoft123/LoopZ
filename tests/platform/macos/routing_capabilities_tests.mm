#include <loopz/macos/routing_service.hpp>
#include <loopz/macos/transport_message.hpp>
#include <loopz/macos/engine_runtime.hpp>
#include <loopz/macos/editor_service.hpp>
#include "../../../native/platform/macos/routing_service/connection.hpp"
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include "../../../native/platform/macos/routing_service/wire.hpp"
#include "routing_compatibility_fixtures.hpp"
#include "test_code_identity.hpp"
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>

namespace {
using namespace loopz;
using namespace loopz::macos;
uint64_t now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
template <typename Predicate> void wait(Predicate predicate) {
    const auto deadline = now() + 10000;
    while (!predicate()) {
        assert(now() < deadline && "Capability negotiation did not settle");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
RoutingBrokerPolicy policy(const std::string& requirement) {
    return {requirement, requirement, static_cast<uint32_t>(geteuid()), false,
            []() -> std::optional<uint32_t> {
                return geteuid();
            }};
}
RoutingServiceEndpoint endpoint(RoutingBroker& broker, const std::string& requirement) {
    RoutingServiceEndpoint target;
    target.endpoint = broker.endpoint();
    target.serverRequirement = requirement;
    target.serverUID = geteuid();
    target.privileged = false;
    return target;
}
RouteDocument document(uint32_t rate) {
    VirtualDeviceDefinition device;
    device.id = "capability-device";
    device.name = "Capability device";
    device.sampleRate = rate;
    return {kRouteDocumentVersion, {std::move(device)}};
}
struct Native {
    std::atomic<uint64_t> revision{0};
    std::atomic<unsigned> applications{0};
    std::atomic<uint32_t> negotiated{kLegacyVirtualSampleRates};
    int32_t apply(xpc_object_t payload) {
        const auto snapshot = DeviceTransportSnapshot::decode(payload);
        revision.store(snapshot.revision);
        ++applications;
        return 0;
    }
    DriverConfigurationResult status(uint64_t requested) const {
        return {requested ? requested : revision.load(), DriverConfigurationPhase::Committed, 0};
    }
    RoutingDriverOperations operations(uint32_t mask = kAllVirtualSampleRates) {
        return {[this](xpc_object_t payload) {
                    return apply(payload);
                },
                [this](uint64_t value) {
                    return status(value);
                },
                [](uint64_t) -> int32_t {
                    return 0;
                },
                [](bool) {},
                {},
                {},
                mask,
                [this](uint32_t value) {
                    negotiated.store(value);
                }};
    }
    xpc_object_t oldHandler(xpc_object_t command) {
        routing::dictionary(command, 4);
        auto response = xpc_dictionary_create(nullptr, nullptr, 0);
        const auto operation = routing::text(command, "op");
        int32_t result = 0;
        if (operation == "apply")
            result = apply(xpc_dictionary_get_value(command, "payload"));
        else if (operation == "status")
            xpc_dictionary_set_value(
                response, "configuration",
                routing::encode(status(routing::integer(command, "revision"))));
        else if (operation == "rates")
            xpc_dictionary_set_value(response, "rates", xpc_array_create(nullptr, 0));
        else
            assert(operation == "cancel" || operation == "ack_rates");
        xpc_dictionary_set_int64(response, "result", result);
        return response;
    }
};

void snapshotRates(DriverConfigurationLink& link, uint32_t expected) {
    EngineRuntimeOptions options;
    options.installationValidation = true;
    EngineRuntime runtime(link, options);
    const auto encoded = runtimeEditorOperations(runtime).snapshot();
    auto* data = [NSData dataWithBytes:encoded.data() length:encoded.size()];
    NSDictionary* snapshot = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    NSArray* rates = snapshot[@"runtime"][@"sampleRates"];
    uint32_t observed = 0;
    for (NSNumber* rate in rates) {
        const auto bit = virtualSampleRateBit(rate.doubleValue);
        assert(bit && !(observed & bit));
        observed |= bit;
    }
    assert(observed == expected);
}

void mixedVersions(const std::string& requirement) {
    for (const bool oldBroker : {false, true})
        for (const bool oldDriver : {false, true})
            for (const bool oldHelper : {false, true})
                for (const bool helperFirst : {false, true}) {
                    std::unique_ptr<RoutingBroker> broker;
                    test::PriorBroker prior;
                    RoutingServiceEndpoint target;
                    if (oldBroker) {
                        prior = test::priorBroker(policy(requirement), requirement);
                        target = prior.target;
                    } else {
                        broker = RoutingBroker::anonymous(policy(requirement));
                        target = endpoint(*broker, requirement);
                    }
                    Native native;
                    std::unique_ptr<RoutingDriverService> driver;
                    std::unique_ptr<test::PriorPeer> priorDriver, priorHelper;
                    std::unique_ptr<RoutingDriverLink> helper;
                    std::string error;
                    auto connectHelper = [&] {
                        if (oldHelper) {
                            priorHelper = test::priorPeer(target, "helper");
                            assert(priorHelper->connect(error));
                        } else {
                            helper = std::make_unique<RoutingDriverLink>(target);
                            helper->poll(now());
                            if (helperFirst)
                                assert(helper->sampleRateMask() == kLegacyVirtualSampleRates);
                        }
                    };
                    auto connectDriver = [&] {
                        if (oldDriver) {
                            priorDriver =
                                test::priorPeer(target, "driver", [&](xpc_object_t command) {
                                    return native.oldHandler(command);
                                });
                            assert(priorDriver->connect(error));
                        } else
                            driver =
                                std::make_unique<RoutingDriverService>(target, native.operations());
                    };
                    if (helperFirst) {
                        connectHelper();
                        connectDriver();
                    } else {
                        connectDriver();
                        connectHelper();
                    }
                    wait([&] {
                        if (helper)
                            helper->poll(now());
                        return helper ? helper->session() != 0 : priorHelper->session() != 0;
                    });
                    const auto expected = !oldBroker && !oldDriver && !oldHelper
                                              ? kAllVirtualSampleRates
                                              : kLegacyVirtualSampleRates;
                    if (helper) {
                        assert(helper->sampleRateMask() == expected);
                        snapshotRates(*helper, expected);
                    }
                    if (driver)
                        wait([&] {
                            return native.negotiated.load() == expected;
                        });
                    DeviceTransportPool pool;
                    pool.apply(document(48000));
                    if (helper) {
                        assert(helper->status(0).phase == DriverConfigurationPhase::Committed);
                        assert(helper->submit(pool.devices(), 1) == 0);
                        pool.apply(document(96000));
                        assert((helper->submit(pool.devices(), 2) == 0) ==
                               (expected == kAllVirtualSampleRates));
                        assert(native.applications.load() ==
                               (expected == kAllVirtualSampleRates ? 2U : 1U));
                        helper.reset();
                    } else {
                        auto status = priorHelper->request("status", nullptr, 0);
                        assert(routing::resultCode(status, "result") == 0);
                        auto response = priorHelper->request(
                            "apply", encodeDeviceTransports(pool.devices(), 1), 0);
                        assert(routing::resultCode(response, "result") == 0 &&
                               native.applications.load() == 1);
                        assert(
                            priorHelper->alive()); // Strict old decoder accepted every pair notice.
                        priorHelper.reset();
                    }
                    if (driver)
                        wait([&] {
                            return native.negotiated.load() == kLegacyVirtualSampleRates;
                        });
                }
}

std::string contents(const std::filesystem::path& path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), {}};
}
void savedIntentAcrossDowngrade(const std::string& requirement) {
    char directory[] = "/private/tmp/loopz-capability-intent-XXXXXX";
    assert(mkdtemp(directory));
    const auto path = std::filesystem::path(directory) / "routes.json";
    auto broker = RoutingBroker::anonymous(policy(requirement));
    const auto target = endpoint(*broker, requirement);
    RoutingDriverLink helper(target);
    ConfigurationController control(helper, path, nullptr, false);
    bool rejected = false;
    try {
        control.saveDesired(document(96000));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected && !std::filesystem::exists(path) && !control.desired());
    control.saveDesired(document(48000));
    Native initial;
    auto driver = std::make_unique<RoutingDriverService>(target, initial.operations());
    auto restored = [&] {
        wait([&] {
            helper.poll(now());
            control.poll(now());
            return control.status().phase == RestorationPhase::Restored;
        });
    };
    restored();
    control.saveDesired(document(96000));
    restored();
    const auto firstSession = helper.session();
    driver.reset();
    Native downgraded;
    driver = std::make_unique<RoutingDriverService>(
        target, downgraded.operations(kLegacyVirtualSampleRates));
    wait([&] {
        helper.poll(now());
        control.poll(now());
        return helper.session() && helper.session() != firstSession &&
               control.status().detail.find("compatible") != std::string::npos;
    });
    assert(helper.sampleRateMask() == kLegacyVirtualSampleRates &&
           downgraded.applications.load() == 0);
    snapshotRates(helper, kLegacyVirtualSampleRates);
    auto retained = *control.desired();
    assert(retained.devices[0].sampleRate == 96000);
    retained.devices[0].name = "Saved while waiting";
    control.saveDesired(retained);
    const auto before = contents(path);
    auto unsupported = retained;
    unsupported.devices[0].sampleRate = 192000;
    rejected = false;
    try {
        control.saveDesired(unsupported);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected && contents(path) == before &&
           encodeRouteDocumentJSON(*control.desired()) == encodeRouteDocumentJSON(retained));
    {
        auto disabled = retained;
        disabled.devices[0].routing.enabled = false;
        const auto disabledPath = std::filesystem::path(directory) / "disabled.json";
        ConfigurationController dormant(helper, disabledPath, nullptr, false);
        dormant.setDesired(disabled);
        dormant.saveDesired(disabled);
        const auto saved = contents(disabledPath);
        bool refusedEnable = false;
        try {
            dormant.saveDesired(retained);
        } catch (const std::invalid_argument&) {
            refusedEnable = true;
        }
        auto duplicate = disabled;
        duplicate.devices[0].id = "another-high-rate-device";
        bool refusedCreation = false;
        try {
            dormant.saveDesired(duplicate);
        } catch (const std::invalid_argument&) {
            refusedCreation = true;
        }
        assert(refusedEnable && refusedCreation && contents(disabledPath) == saved);
    }
    driver.reset();
    Native upgraded;
    driver = std::make_unique<RoutingDriverService>(target, upgraded.operations());
    restored();
    assert(helper.sampleRateMask() == kAllVirtualSampleRates);
    snapshotRates(helper, kAllVirtualSampleRates);
    assert(upgraded.applications.load() == 1 && contents(path) == before);
    assert(control.active() && control.active()->devices[0].sampleRate == 96000);
    driver.reset();
    helper.invalidate();
    assert(helper.sampleRateMask() == kLegacyVirtualSampleRates);
    std::filesystem::remove_all(directory);
}

void partialRateSet(const std::string& requirement) {
    auto broker = RoutingBroker::anonymous(policy(requirement));
    const auto target = endpoint(*broker, requirement);
    Native native;
    const auto mask = kLegacyVirtualSampleRates | virtualSampleRateBit(96000);
    RoutingDriverService driver(target, native.operations(mask));
    RoutingDriverLink helper(target);
    wait([&] {
        helper.poll(now());
        return helper.session() != 0;
    });
    assert(helper.sampleRateMask() == mask);
    snapshotRates(helper, mask);
    DeviceTransportPool pool;
    pool.apply(document(96000));
    assert(helper.submit(pool.devices(), 1) == 0);
    pool.apply(document(88200));
    assert(helper.submit(pool.devices(), 2) != 0 && native.applications.load() == 1);
    auto malformed = pool.devices();
    malformed[0].region.reset();
    assert(helper.submit(malformed, 2) != 0 && native.applications.load() == 1);
}

void brokerRejectsUnsupportedPublication(const std::string& requirement) {
    auto broker = RoutingBroker::anonymous(policy(requirement));
    const auto target = endpoint(*broker, requirement);
    Native native;
    RoutingDriverService driver(target, native.operations(kLegacyVirtualSampleRates));
    routing::Connection helper(target, "helper");
    std::string error;
    assert(helper.connect(error));
    wait([&] {
        return helper.session() != 0;
    });
    DeviceTransportPool pool;
    pool.apply(document(96000));
    bool rejected = false;
    try {
        (void)helper.request("apply", encodeDeviceTransports(pool.devices(), 1), 0);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    assert(rejected && native.applications.load() == 0 && helper.alive());
    pool.apply(document(48000));
    const auto response = helper.request("apply", encodeDeviceTransports(pool.devices(), 1), 0);
    assert(routing::resultCode(response, "result") == 0 && native.applications.load() == 1);
}

xpc_object_t exchange(xpc_connection_t peer, xpc_object_t message) {
    struct Reply {
        xpc_object_t value{nullptr};
    };
    auto reply = std::make_shared<Reply>();
    auto ready = dispatch_semaphore_create(0);
    xpc_connection_send_message_with_reply(peer, message, dispatch_get_global_queue(0, 0),
                                           ^(xpc_object_t value) {
                                               reply->value = value;
                                               dispatch_semaphore_signal(ready);
                                           });
    assert(dispatch_semaphore_wait(ready, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)) == 0);
    routing::protocol(reply->value);
    return reply->value;
}

void malformedCapabilities(const std::string& requirement) {
    auto broker = RoutingBroker::anonymous(policy(requirement));
    const auto target = endpoint(*broker, requirement);
    for (unsigned index = 0; index < 8; ++index) {
        auto peer = xpc_connection_create_from_endpoint(target.endpoint);
        assert(peer &&
               xpc_connection_set_peer_code_signing_requirement(peer, requirement.c_str()) == 0);
        xpc_connection_set_event_handler(peer, ^(xpc_object_t){
                                         });
        xpc_connection_activate(peer);
        auto hello = routing::message("hello");
        xpc_dictionary_set_string(hello, "role", "helper");
        const auto before = exchange(peer, hello);
        assert(routing::boolean(before, "ok") &&
               routing::integer(before, "rateCapabilitiesVersion") == 1);
        auto attach = routing::message("attach");
        xpc_dictionary_set_string(attach, "role", "helper");
        constexpr uint64_t invalid[]{0, 1, 64, 128, UINT64_MAX};
        if (index < std::size(invalid))
            xpc_dictionary_set_uint64(attach, "sampleRateMask", invalid[index]);
        else if (index == 5)
            xpc_dictionary_set_string(attach, "sampleRateMask", "63");
        else if (index == 6)
            xpc_dictionary_set_bool(attach, "sampleRateMask", true);
        else
            xpc_dictionary_set_double(attach, "sampleRateMask", 63);
        const auto reply = exchange(peer, attach);
        assert(!routing::boolean(reply, "ok") && !routing::boolean(reply, "paired"));
        assert(routing::integer(reply, "epoch") == routing::integer(before, "epoch"));
        xpc_connection_cancel(peer);
    }
    Native native;
    RoutingDriverService driver(target, native.operations());
    RoutingDriverLink helper(target);
    wait([&] {
        helper.poll(now());
        return helper.session() != 0;
    });
    assert(helper.sampleRateMask() == kAllVirtualSampleRates && native.applications.load() == 0);
}
} // namespace

int main() {
    const auto requirement = selfCodeRequirement();
    mixedVersions(requirement);
    savedIntentAcrossDowngrade(requirement);
    partialRateSet(requirement);
    brokerRejectsUnsupportedPublication(requirement);
    malformedCapabilities(requirement);
    std::puts("Routing capabilities: 16 real-XPC version/order combinations and durable "
              "downgrade/recovery passed");
}
