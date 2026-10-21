#include <loopz/macos/application_ownership.hpp>
#include "routing_io_support.hpp"
#import <AppKit/AppKit.h>
#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {
using namespace loopz;
using namespace loopz::macos;
using Clock = std::chrono::steady_clock;
constexpr auto parentID = "com.lexasoft.loopz.fixture.family";
void emit(NSDictionary* value) {
    NSData* bytes = [NSJSONSerialization dataWithJSONObject:value options:0 error:nil];
    if (!bytes || std::fwrite(bytes.bytes, 1, bytes.length, stdout) != bytes.length ||
        std::putchar('\n') == EOF || std::fflush(stdout))
        throw std::runtime_error("Family fixture observer disconnected");
}
struct Options {
    bool child{false}, metadata{false};
    std::string uidA, uidB;
    uint32_t seconds{120};
};
Options options(int argc, char** argv) {
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--child")
            result.child = true;
        else if (key == "--metadata-only")
            result.metadata = true;
        else if (i + 1 < argc && key == "--device-a")
            result.uidA = argv[++i];
        else if (i + 1 < argc && key == "--device-b")
            result.uidB = argv[++i];
        else if (i + 1 < argc && key == "--seconds") {
            const std::string value = argv[++i];
            size_t end = 0;
            unsigned long seconds;
            try {
                seconds = std::stoul(value, &end);
            } catch (const std::exception&) {
                throw std::invalid_argument("Invalid family fixture duration");
            }
            if (end != value.size() || seconds < 3 || seconds > 120)
                throw std::invalid_argument("Family fixture duration must be 3–120 seconds");
            result.seconds = static_cast<uint32_t>(seconds);
        } else
            throw std::invalid_argument("Invalid family fixture argument");
    }
    if (result.metadata) {
        if (!result.uidA.empty() || !result.uidB.empty())
            throw std::invalid_argument("Metadata-only mode cannot specify audio devices");
    } else if (result.uidA.empty() ||
               (!result.child && (result.uidB.empty() || result.uidA == result.uidB)))
        throw std::invalid_argument("Family audio fixture requires distinct virtual outputs");
    return result;
}
// Reads one bounded line without stdio read-ahead, so poll remains authoritative.
bool command(std::string& pending, std::string& result, int timeout) {
    pollfd descriptor{STDIN_FILENO, POLLIN, 0};
    const auto ready = ::poll(&descriptor, 1, timeout);
    if (ready < 0) {
        if (errno == EINTR)
            return false;
        throw std::runtime_error("Family fixture control polling failed");
    }
    if (!ready)
        return false;
    char byte;
    if (::read(STDIN_FILENO, &byte, 1) != 1) {
        result = "quit";
        return true;
    }
    if (byte == '\n') {
        result = std::exchange(pending, {});
        return true;
    }
    if (pending.size() >= 32)
        throw std::invalid_argument("Family fixture command exceeds limit");
    pending += byte;
    return false;
}
struct Output {
    singz::AudioHost host;
    std::atomic<uint64_t> frames{0}, timestampErrors{0};
    const double frequency;
    explicit Output(double hz) : frequency(hz) {}
    ~Output() {
        host.stop();
    }
    static bool render(void* context, const singz::AudioHostRenderBlock& block) noexcept {
        auto& self = *static_cast<Output*>(context);
        if (!block.output || block.outputChannels != 2 || block.inputChannels || !block.output[0] ||
            !block.output[1])
            return false;
        if (!block.outputTimestampValid || !block.outputTimestampHardware)
            self.timestampErrors.fetch_add(1);
        const auto position = self.frames.load();
        for (uint32_t channel = 0; channel < 2; ++channel)
            for (uint32_t frame = 0; frame < block.frames; ++frame)
                block.output[channel][frame] = static_cast<float>(
                    0.1 * std::sin(2 * std::numbers::pi_v<double> * self.frequency * (channel + 1) *
                                   (position + frame) / block.sampleRate));
        self.frames.store(position + block.frames);
        return true;
    }
};
int child(const Options& selected) {
    NSString* bundle = NSBundle.mainBundle.bundleIdentifier;
    if (![bundle isEqualToString:@"com.lexasoft.loopz.fixture.family.helper.a"] &&
        ![bundle isEqualToString:@"com.lexasoft.loopz.fixture.family.helper.b"])
        throw std::invalid_argument("Child must run from its prepared helper app");
    const bool a = [bundle hasSuffix:@".a"];
    Output output(a ? 1100 : 1300);
    if (!selected.metadata) {
        diagnostic::requireRoutingDevice(selected.uidA);
        diagnostic::requireOK(output.host.open(diagnostic::config(selected.uidA, 2, false, 64),
                                               Output::render, &output));
        diagnostic::requireOK(output.host.start());
    }
    emit(@{
        @"event" : @"ready",
        @"pid" : @(getpid()),
        @"bundle" : bundle,
        @"metadataOnly" : @(selected.metadata)
    });
    const auto deadline = Clock::now() + std::chrono::seconds(selected.seconds);
    std::string pending, next;
    while (Clock::now() < deadline) {
        if (!command(pending, next, 20))
            continue;
        if (next == "quit") {
            output.host.stop();
            return output.timestampErrors.load() || (!selected.metadata && !output.frames.load())
                       ? 1
                       : 0;
        }
        if (next != "status")
            throw std::invalid_argument("Unexpected child command");
        emit(@{
            @"event" : @"status",
            @"frames" : @(output.frames.load()),
            @"timestampErrors" : @(output.timestampErrors.load())
        });
    }
    throw std::runtime_error("Family helper deadline expired");
}
struct Child {
    NSTask* task;
    NSPipe* input;
    NSPipe* output;
    NSString* bundle;
    Child(char name, const Options& selected) {
        bundle = [NSString stringWithFormat:@"com.lexasoft.loopz.fixture.family.helper.%c", name];
        NSString* path = [NSBundle.mainBundle.bundlePath
            stringByAppendingPathComponent:
                [NSString
                    stringWithFormat:@"Contents/Helpers/%c.app/Contents/MacOS/Fixture", name]];
        task = [NSTask new];
        task.executableURL = [NSURL fileURLWithPath:path];
        NSMutableArray* args = [NSMutableArray arrayWithArray:@[
            @"--child", @"--seconds", @(std::to_string(selected.seconds).c_str())
        ]];
        if (selected.metadata)
            [args addObject:@"--metadata-only"];
        else
            [args addObjectsFromArray:@[
                @"--device-a", @((name == 'a' ? selected.uidA : selected.uidB).c_str())
            ]];
        task.arguments = args;
        task.standardInput = input = [NSPipe pipe];
        task.standardOutput = output = [NSPipe pipe];
        task.standardError = NSFileHandle.fileHandleWithStandardError;
        NSError* error = nil;
        if (![task launchAndReturnError:&error])
            throw std::runtime_error(error.localizedDescription.UTF8String);
        try {
            const auto deadline = Clock::now() + std::chrono::seconds(5);
            std::string line;
            bool complete = false;
            const auto fd = output.fileHandleForReading.fileDescriptor;
            while (Clock::now() < deadline) {
                pollfd descriptor{fd, POLLIN, 0};
                if (::poll(&descriptor, 1, 20) <= 0)
                    continue;
                char byte;
                if (::read(fd, &byte, 1) != 1)
                    break;
                if (byte == '\n') {
                    complete = true;
                    break;
                }
                if (line.size() >= 4096)
                    break;
                line += byte;
            }
            NSData* bytes = [NSData dataWithBytes:line.data() length:line.size()];
            id ready =
                complete ? [NSJSONSerialization JSONObjectWithData:bytes options:0 error:nil] : nil;
            if (![ready isKindOfClass:NSDictionary.class] || ![ready[@"event"] isEqual:@"ready"] ||
                ![ready[@"bundle"] isEqual:bundle] ||
                [ready[@"pid"] intValue] != task.processIdentifier ||
                [ready[@"metadataOnly"] boolValue] != selected.metadata)
                throw std::runtime_error("Family helper failed to acknowledge readiness");
        } catch (...) {
            stop();
            throw;
        }
    }
    bool stop() {
        if (task.running) {
            // Closing the owned pipe asks the fixture child to stop and join IO.
            [input.fileHandleForWriting closeFile];
            const auto deadline = Clock::now() + std::chrono::seconds(3);
            while (task.running && Clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (task.running)
                [task terminate];
            [task waitUntilExit];
        }
        return task.terminationReason == NSTaskTerminationReasonExit && task.terminationStatus == 0;
    }
    ~Child() {
        stop();
    }
};
int parent(const Options& selected) {
    if (![NSBundle.mainBundle.bundleIdentifier isEqualToString:@(parentID)])
        throw std::invalid_argument("Parent must run from its prepared fixture app");
    if (!selected.metadata) {
        const auto a = diagnostic::requireRoutingDevice(selected.uidA);
        if (a == diagnostic::requireRoutingDevice(selected.uidB))
            throw std::invalid_argument("Family outputs resolve to the same device");
    }
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSApp finishLaunching];
    std::unique_ptr<Child> a = std::make_unique<Child>('a', selected), b;
    const auto stop = [](std::unique_ptr<Child>& child) {
        if (child && !child->stop())
            throw std::runtime_error("Family helper exited unsuccessfully");
        child.reset();
    };
    const auto report = [&](const std::string& event) {
        ApplicationOwnership ownership(parentID);
        std::vector<AudioProcessAttribution> observed;
        for (const auto* item : {a.get(), b.get()})
            if (item && item->task.running) {
                const auto pid = item->task.processIdentifier;
                if (auto value = ownership.inspect(pid, pid, item->bundle.UTF8String))
                    observed.push_back(*value);
            }
        const auto matches = selectApplicationProcesses(parentID, ownership.roots(), observed);
        NSMutableArray* included = [NSMutableArray array];
        for (const auto& value : observed)
            [included addObject:@{
                @"pid" : @(value.process.id),
                @"generation" : @(value.process.generation),
                @"bundle" : @(value.application.c_str())
            }];
        emit(@{
            @"event" : @(event.c_str()),
            @"parentPID" : @(getpid()),
            @"children" : included,
            @"selectedCount" : @(matches.size()),
            @"coherent" : @(ownership.coherent(observed)),
            @"roots" : @(ownership.roots().size()),
            @"registeredBundle" : NSRunningApplication.currentApplication.bundleIdentifier ?: @"",
            @"registeredExecutable" : NSRunningApplication.currentApplication.executableURL.path
                ?: @"",
            @"bundleExecutable" : NSBundle.mainBundle.executablePath ?: @"",
            @"metadataOnly" : @(selected.metadata),
            @"parentOpenedAudio" : @NO
        });
    };
    report("ready");
    const auto deadline = Clock::now() + std::chrono::seconds(selected.seconds);
    std::string pending, next;
    while (Clock::now() < deadline) {
        [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate date]];
        if (!command(pending, next, 20))
            continue;
        if (next == "quit") {
            stop(a);
            stop(b);
            report("complete");
            return 0;
        }
        if (next == "start-b" && !b)
            b = std::make_unique<Child>('b', selected);
        else if (next == "stop-b" && b)
            stop(b);
        else if (next == "restart-b" && b) {
            stop(b);
            b = std::make_unique<Child>('b', selected);
        } else if (next != "status")
            throw std::invalid_argument("Unexpected parent command");
        report(next);
    }
    throw std::runtime_error("Family parent deadline expired");
}
} // namespace
int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            const auto selected = options(argc, argv);
            return selected.child ? child(selected) : parent(selected);
        } catch (const std::invalid_argument& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 2;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 1;
        }
    }
}
