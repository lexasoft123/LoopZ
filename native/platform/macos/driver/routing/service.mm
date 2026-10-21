#include <loopz/macos/audio_host_identity.hpp>
#include "state.hpp"
#include <loopz/macos/routing_service.hpp>
#include <Security/Security.h>
#include <dispatch/dispatch.h>
#include <pwd.h>
#include <unistd.h>

#include <cstdio>

namespace loopz::macos::hal {
namespace {
bool audioHost() {
    const auto* audio = getpwnam("_coreaudiod");
    if (!audio || (geteuid() != 0 && geteuid() != audio->pw_uid))
        return false;
    SecCodeRef code = nullptr;
    SecRequirementRef requirement = nullptr;
    const auto text =
        CFStringCreateWithCString(nullptr, kAudioHostRequirement, kCFStringEncodingUTF8);
    if (!text)
        return false;
    const bool accepted =
        SecCodeCopySelf(kSecCSDefaultFlags, &code) == errSecSuccess &&
        SecRequirementCreateWithString(text, kSecCSDefaultFlags, &requirement) == errSecSuccess &&
        SecCodeCheckValidity(code, kSecCSDefaultFlags, requirement) == errSecSuccess;
    if (requirement)
        CFRelease(requirement);
    if (code)
        CFRelease(code);
    CFRelease(text);
    return accepted;
}
struct Service {
    dispatch_queue_t queue{nullptr};
    dispatch_source_t timer{nullptr};
    std::unique_ptr<RoutingDriverService> connection;
    std::string lastError;
    void poll() {
        // A sealed bundle pin is fixed for this loaded image. The connection
        // owner handles broker loss/reconnection without reopening trust files.
        if (connection)
            return;
        try {
            auto endpoint =
                bundledRoutingServiceEndpoint(reinterpret_cast<const void*>(&startService));
            state().setServiceAvailable(false);
            RoutingDriverOperations operations{
                [](xpc_object_t message) -> int32_t {
                    try {
                        return state().apply(DeviceTransportSnapshot::decode(message));
                    } catch (const std::invalid_argument&) {
                        return kAudioHardwareIllegalOperationError;
                    } catch (const std::bad_alloc&) {
                        return kAudioHardwareUnspecifiedError;
                    } catch (...) {
                        return kAudioHardwareNotReadyError;
                    }
                },
                [](uint64_t revision) {
                    return state().configurationResult(revision);
                },
                [](uint64_t revision) {
                    return state().cancelConfiguration(revision);
                },
                [](bool available) {
                    state().setServiceAvailable(available);
                },
                [] {
                    return state().rateRequests();
                },
                [](std::span<const DriverRateRequest> requests) {
                    return state().acknowledgeRates(requests);
                },
                kAllVirtualSampleRates,
                [](uint32_t mask) {
                    state().setSampleRateMask(mask);
                }};
            connection =
                std::make_unique<RoutingDriverService>(std::move(endpoint), std::move(operations));
            lastError.clear();
        } catch (const std::exception& error) {
            connection.reset();
            state().setServiceAvailable(false);
            if (lastError != error.what()) {
                lastError = error.what();
                std::fprintf(stderr,
                             "LoopZ routing driver: %s; waiting for service configuration\n",
                             lastError.c_str());
            }
        }
    }
};
} // namespace
void startService() {
    // A factory loaded in a test/tool is not a HAL service. Only the concrete
    // Apple audio hosts start the production privileged Mach connection.
    if (!audioHost())
        return;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        state().setSampleRateMask(kLegacyVirtualSampleRates);
        state().setServiceAvailable(false);
        // HAL controls process lifetime; do not tear down control/context state
        // while it may still invoke callbacks during process shutdown.
        auto* service = new Service;
        service->queue =
            dispatch_queue_create("com.lexasoft.loopz.routing.service-pin", DISPATCH_QUEUE_SERIAL);
        service->timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, service->queue);
        if (!service->queue || !service->timer)
            throw std::bad_alloc();
        dispatch_source_set_timer(service->timer, DISPATCH_TIME_NOW, NSEC_PER_SEC,
                                  50 * NSEC_PER_MSEC);
        dispatch_source_set_event_handler(service->timer, ^{
            service->poll();
        });
        dispatch_resume(service->timer);
    });
}
} // namespace loopz::macos::hal
