#include <loopz/macos/routing_service.hpp>

#include <cstdio>
#include <exception>

extern "C" int LoopZBundlePinFixture(char* output, size_t capacity) {
    try {
        const auto endpoint = loopz::macos::bundledRoutingServiceEndpoint(
            reinterpret_cast<const void*>(&LoopZBundlePinFixture));
        if (!endpoint.privileged || endpoint.serverUID != 0 || endpoint.endpoint ||
            endpoint.serviceName != loopz::macos::kRoutingServiceName)
            return 2;
        const auto count =
            std::snprintf(output, capacity, "%s", endpoint.serverRequirement.c_str());
        return count >= 0 && static_cast<size_t>(count) < capacity ? 0 : 3;
    } catch (const std::exception& error) {
        std::snprintf(output, capacity, "%s", error.what());
        return 1;
    }
}
