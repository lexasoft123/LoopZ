#include <loopz/macos/audio_host_identity.hpp>
#include <loopz/macos/routing_service.hpp>
#include <SystemConfiguration/SystemConfiguration.h>
#include <dispatch/dispatch.h>
#include <pwd.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    if (argc == 2 && !std::strcmp(argv[1], "--runtime-capabilities")) {
        std::fputs(
            "{\"version\":1,\"rateNegotiation\":1,\"audioMappingProtocol\":2,\"sampleRatesHz\":[",
            stdout);
        for (size_t index = 0; index < loopz::kVirtualSampleRates.size(); ++index)
            std::printf("%s%u", index ? "," : "", loopz::kVirtualSampleRates[index]);
        std::puts("]}");
        return 0;
    }
    if (argc != 3 || std::strcmp(argv[1], "--engine-requirement") || geteuid() != 0) {
        std::fprintf(stderr, "usage (system launchd domain): loopz-routing-broker "
                             "--engine-requirement <pinned code requirement>\n");
        return 2;
    }
    try {
        const auto* audio = getpwnam("_coreaudiod");
        if (!audio)
            return 1;
        loopz::macos::RoutingBrokerPolicy policy;
        policy.helperRequirement = argv[2];
        policy.driverRequirement = loopz::macos::kAudioHostRequirement;
        policy.driverUID = audio->pw_uid;
        policy.consoleUser = []() -> std::optional<uint32_t> {
            uid_t uid = static_cast<uid_t>(-1);
            gid_t group;
            const auto user = SCDynamicStoreCopyConsoleUser(nullptr, &uid, &group);
            if (user)
                CFRelease(user);
            if (!uid || uid == static_cast<uid_t>(-1))
                return std::nullopt;
            return uid;
        };
        auto broker = loopz::macos::RoutingBroker::machService(std::move(policy));
        dispatch_main();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
