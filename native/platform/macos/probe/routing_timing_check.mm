#include "routing_timing_check.hpp"
#include "routing_io_support.hpp"
#include <loopz/macos/driver_timing.hpp>
#include <loopz/macos/public_timing.hpp>

#include <charconv>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

namespace loopz::macos {
namespace {
uint32_t integer(const std::string& value) {
    uint32_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw std::invalid_argument("Invalid timing-check integer");
    return result;
}
struct Silence {
    std::atomic<uint64_t> frames{0}, timestampErrors{0};
    static bool render(void* context, const singz::AudioHostRenderBlock& block) noexcept {
        auto& self = *static_cast<Silence*>(context);
        if (!block.output || block.outputChannels != 2 || block.inputChannels || !block.output[0] ||
            !block.output[1])
            return false;
        for (uint32_t channel = 0; channel < 2; ++channel)
            std::fill_n(block.output[channel], block.frames, 0.0f);
        if (!block.outputTimestampValid || !block.outputTimestampHardware)
            self.timestampErrors.fetch_add(1);
        self.frames.fetch_add(block.frames);
        return true;
    }
};
int run(int argc, char** argv) {
    std::string uid;
    uint32_t period = 512, seconds = 3;
    for (int index = 1; index < argc; index += 2) {
        if (index + 1 == argc)
            throw std::invalid_argument("Timing check arguments require values");
        const std::string key = argv[index], value = argv[index + 1];
        if (key == "--device-uid")
            uid = value;
        else if (key == "--buffer-frames")
            period = integer(value);
        else if (key == "--seconds")
            seconds = integer(value);
        else
            throw std::invalid_argument("Unknown timing-check argument");
    }
    if (uid.empty() || period < 32 || period > 2048 || (period & (period - 1)) || seconds < 3 ||
        seconds > 10)
        throw std::invalid_argument(
            "Timing check needs a virtual UID, 32–2048 frames and 3–10 seconds");
    const auto device = diagnostic::requireRoutingDevice(uid);
    AudioObjectPropertyAddress address{kLoopZDevicePropertyIOTiming,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    if (!AudioObjectHasProperty(device, &address))
        throw std::runtime_error("The installed driver does not expose callback timing");
    Silence output;
    singz::AudioHost host;
    diagnostic::requireOK(
        host.open(diagnostic::config(uid, 2, false, period), Silence::render, &output));
    diagnostic::requireOK(host.start());
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    CFPropertyListRef raw = nullptr;
    UInt32 bytes = sizeof(raw);
    const auto status = AudioObjectGetPropertyData(device, &address, 0, nullptr, &bytes, &raw);
    if (status || bytes != sizeof(raw) || !raw) {
        if (raw)
            CFRelease(raw);
        throw std::runtime_error("The driver timing report is unavailable");
    }
    PublicDeviceTiming timing{};
    try {
        timing = decodePublicDeviceTiming(raw);
    } catch (...) {
        CFRelease(raw);
        throw;
    }
    const auto observed = host.status();
    const bool passed = timing.running && timing.measured && timing.session &&
                        output.frames.load() && !output.timestampErrors.load() &&
                        !observed.renderFailures;
    NSDictionary* report = @{
        @"passed" : @(passed),
        @"driver" : (__bridge id)raw,
        @"clientBufferFrames" : @(observed.format.nominalBufferFrames),
        @"frames" : @(output.frames.load()),
        @"timestampErrors" : @(output.timestampErrors.load()),
        @"renderFailures" : @(observed.renderFailures),
        @"inputOpened" : @NO,
        @"physicalDevicesOpened" : @NO,
        @"permissionRequested" : @NO
    };
    CFRelease(raw);
    const auto encoded = [NSJSONSerialization dataWithJSONObject:report options:0 error:nil];
    host.stop();
    if (!encoded || std::fwrite(encoded.bytes, 1, encoded.length, stdout) != encoded.length ||
        std::putchar('\n') == EOF)
        throw std::runtime_error("The timing report could not be written");
    return passed ? 0 : 1;
}
} // namespace
int routingTimingCheck(int argc, char** argv) {
    @autoreleasepool {
        try {
            return run(argc, argv);
        } catch (const std::invalid_argument& error) {
            std::fprintf(stderr, "Timing check: %s\n", error.what());
            return 2;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Timing check: %s\n", error.what());
            return 1;
        }
    }
}
} // namespace loopz::macos
