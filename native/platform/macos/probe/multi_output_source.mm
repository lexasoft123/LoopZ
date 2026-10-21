#include "multi_output_source.hpp"
#include "routing_io_support.hpp"
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>

namespace loopz::macos {
namespace {
using Clock = std::chrono::steady_clock;
using namespace diagnostic;
uint32_t integer(const std::string& text, uint32_t maximum) {
    try {
        size_t used = 0;
        const auto value = std::stoul(text, &used);
        if (used == text.size() && value <= maximum)
            return static_cast<uint32_t>(value);
    } catch (const std::exception&) {
    }
    throw std::invalid_argument("Invalid multi-output fixture integer");
}
void emit(NSDictionary* value) {
    const auto bytes = [NSJSONSerialization dataWithJSONObject:value options:0 error:nil];
    if (!bytes || std::fwrite(bytes.bytes, 1, bytes.length, stdout) != bytes.length ||
        std::putchar('\n') == EOF || std::fflush(stdout))
        throw std::runtime_error("Multi-output fixture observer disconnected");
}
struct Output {
    singz::AudioHost host;
    const std::string uid;
    const double leftHz, rightHz;
    const uint32_t bufferFrames;
    const bool markerOnly;
    uint64_t position{0}; // Callback only, or control after stop() has joined it.
    std::atomic<uint64_t> frames{0}, timestampErrors{0};
    std::atomic<uint32_t> minimumFrames{UINT32_MAX}, maximumFrames{0};
    explicit Output(std::string selected, double left, double right, uint32_t period, bool marker)
        : uid(std::move(selected)), leftHz(left), rightHz(right), bufferFrames(period),
          markerOnly(marker) {}
    ~Output() {
        host.stop();
    }
    static bool render(void* context, const singz::AudioHostRenderBlock& block) noexcept {
        auto& self = *static_cast<Output*>(context);
        if (!block.output || block.outputChannels != 2 || block.inputChannels || !block.output[0] ||
            !block.output[1])
            return false;
        const bool valid = block.outputTimestampValid && block.outputTimestampHardware;
        if (!valid)
            self.timestampErrors.fetch_add(1);
        // One callback writer; the control owner observes atomic counters.
        self.minimumFrames.store(std::min(self.minimumFrames.load(), block.frames));
        self.maximumFrames.store(std::max(self.maximumFrames.load(), block.frames));
        const double frequency[]{self.leftHz, self.rightHz};
        for (uint32_t channel = 0; channel < 2; ++channel)
            for (uint32_t frame = 0; frame < block.frames; ++frame) {
                const auto ns = block.outputHostTimeNs +
                                static_cast<uint64_t>((frame + 0.5) * 1e9 / block.sampleRate);
                const auto tone =
                    self.markerOnly
                        ? 0.0
                        : 0.1 * std::sin(2 * std::numbers::pi_v<double> * frequency[channel] *
                                         (self.position + frame) / block.sampleRate);
                block.output[channel][frame] =
                    static_cast<float>(tone + (valid ? probe::marker(ns, channel) : 0));
            }
        self.position += block.frames;
        self.frames.store(self.position);
        return true;
    }
    void prepare() {
        requireRoutingDevice(uid);
        requireOK(host.open(config(uid, 2, false, bufferFrames), render, this));
    }
    void start() {
        requireOK(host.start());
    }
    NSDictionary* status() {
        const auto state = host.status();
        return @{
            @"frames" : @(frames.load()),
            @"timestampErrors" : @(timestampErrors.load()),
            @"renderFailures" : @(state.renderFailures),
            @"rate" : @(state.format.sampleRate),
            @"bufferFrames" : @(state.format.nominalBufferFrames),
            @"callbackFrames" :
                @[ @(frames.load() ? minimumFrames.load() : 0), @(maximumFrames.load()) ]
        };
    }
};
int run(int argc, char** argv) {
    std::string uidA, uidB;
    uint32_t seconds = 120, bufferFrames = 0, secondaryBufferFrames = 0, autoStartMs = 0;
    bool markerOnly = false;
    for (int index = 1; index < argc; index += 2) {
        if (index + 1 == argc)
            throw std::invalid_argument("Multi-output fixture arguments require values");
        const std::string key = argv[index], value = argv[index + 1];
        if (key == "--device-a")
            uidA = value;
        else if (key == "--device-b")
            uidB = value;
        else if (key == "--seconds")
            seconds = integer(value, 86400);
        else if (key == "--auto-start-b-ms")
            autoStartMs = integer(value, 30000);
        else if (key == "--signal") {
            if (value != "marker" && value != "tone")
                throw std::invalid_argument("Fixture signal must be marker or tone");
            markerOnly = value == "marker";
        } else if (key == "--buffer-frames" || key == "--buffer-b-frames") {
            const auto period = integer(value, 2048);
            if (period < 32 || (period & (period - 1)))
                throw std::invalid_argument("Fixture buffer must be a power of two from 32–2048");
            (key == "--buffer-frames" ? bufferFrames : secondaryBufferFrames) = period;
        } else
            throw std::invalid_argument("Unknown multi-output fixture argument");
    }
    if (seconds < 3 || uidA.empty() || uidB.empty() || uidA == uidB)
        throw std::invalid_argument(
            "Fixture requires two distinct virtual outputs and 3–86400 seconds");
    // Verify both before opening either: an invalid second endpoint cannot
    // cause even the first output to acquire IO or change its virtual period.
    if (requireRoutingDevice(uidA) == requireRoutingDevice(uidB))
        throw std::invalid_argument("The two fixture identities resolve to one device");
    Output a(uidA, 1100, 2200, bufferFrames, markerOnly),
        b(uidB, 1300, 2600, secondaryBufferFrames ? secondaryBufferFrames : bufferFrames,
          markerOnly);
    a.prepare();
    b.prepare();
    a.start();
    bool startedB = false, closedB = false;
    const auto report = [&](const char* event) {
        emit(@{
            @"event" : @(event),
            @"signal" : markerOnly ? @"marker" : @"tone",
            @"pid" : @(getpid()),
            @"bRunning" : @(startedB),
            @"a" : a.status(),
            @"b" : b.status(),
            @"physicalOutputsOpened" : @NO,
            @"inputOpened" : @NO,
            @"permissionRequested" : @NO
        });
    };
    report("a-running-b-prepared");
    const auto begin = Clock::now();
    std::string command;
    bool autoStarted = false;
    while (Clock::now() - begin < std::chrono::seconds(seconds)) {
        if (autoStartMs && !autoStarted &&
            Clock::now() - begin >= std::chrono::milliseconds(autoStartMs)) {
            if (!startedB && !closedB) {
                b.start();
                startedB = true;
                report("watchdog-started-b");
            }
            autoStarted = true;
        }
        pollfd descriptor{STDIN_FILENO, POLLIN, 0};
        const auto ready = ::poll(&descriptor, 1, 20);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error("Multi-output controller polling failed");
        }
        if (!ready)
            continue;
        char byte;
        if (::read(STDIN_FILENO, &byte, 1) != 1)
            throw std::runtime_error("Multi-output controller closed before completion");
        if (byte != '\n') {
            if (command.size() == 32)
                throw std::invalid_argument("Multi-output command is too long");
            command += byte;
            continue;
        }
        if (command == "start-b" && !startedB && !closedB) {
            b.start();
            startedB = true;
        } else if (command == "close-b" && startedB) {
            b.host.stop();
            startedB = false;
            closedB = true;
        } else if (command == "reopen-b" && closedB && !startedB) {
            b.prepare();
            b.start();
            startedB = true;
            closedB = false;
        } else if (command == "quit") {
            a.host.stop();
            b.host.stop();
            report("complete");
            return a.frames.load() && !a.timestampErrors.load() && !b.timestampErrors.load() ? 0
                                                                                             : 1;
        } else
            throw std::invalid_argument("Unexpected multi-output fixture command");
        report(command.c_str());
        command.clear();
    }
    throw std::runtime_error("Multi-output fixture control deadline expired");
}
} // namespace
int multiOutputSource(int argc, char** argv) {
    @autoreleasepool {
        try {
            return run(argc, argv);
        } catch (const std::invalid_argument& error) {
            std::fprintf(stderr, "Multi-output fixture: %s\n", error.what());
            return 2;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Multi-output fixture: %s\n", error.what());
            return 1;
        }
    }
}
} // namespace loopz::macos
