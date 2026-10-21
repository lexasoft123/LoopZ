#include "../../native/platform/macos/desktop/microphone_setup.hpp"
#import <Foundation/Foundation.h>
#include <cassert>
#include <cerrno>
#include <filesystem>
#include <functional>
#include <sys/wait.h>
#include <thread>

using namespace loopz::macos;
using namespace std::chrono_literals;

static void refuses(const std::function<void()>& operation) {
    bool refused = false;
    try {
        operation();
    } catch (const std::exception&) {
        refused = true;
    }
    assert(refused);
}

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "--sleep") {
        std::this_thread::sleep_for(10s);
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--flood") {
        for (int i = 0; i < 100000; ++i)
            std::puts("bounded-child-output");
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--denied") {
        std::fputs("Simulated foreground launch diagnostic\n", stderr);
        std::puts("{\"microphone\":\"denied\",\"requested\":true,\"requestCompleted\":true}");
        return 1;
    }
    @autoreleasepool {
        auto allowed = decodeMicrophoneSetup(
            {0, R"({"microphone":"authorized","requested":true,"requestCompleted":true})"});
        assert(allowed.status == MicrophonePermission::Authorized && allowed.requested &&
               allowed.completed);
        auto existing = decodeMicrophoneSetup(
            {0, R"({"microphone":"authorized","requested":false,"requestCompleted":false})"});
        assert(existing.status == MicrophonePermission::Authorized && !existing.requested &&
               existing.completed);
        auto pending = decodeMicrophoneSetup(
            {1, R"({"microphone":"not-determined","requested":true,"requestCompleted":false})"});
        assert(pending.status == MicrophonePermission::NotDetermined && pending.requested &&
               !pending.completed);
        for (const auto* name : {"denied", "restricted", "unknown"}) {
            const auto result = decodeMicrophoneSetup(
                {1, std::string("{\"microphone\":\"") + name +
                        "\",\"requested\":false,\"requestCompleted\":false}"});
            assert(!result.requested && result.completed);
        }
        for (const auto* malformed :
             {"null", "[]", "{}",
              R"({"microphone":"authorized","requested":1,"requestCompleted":true})",
              R"({"microphone":"authorized","requested":false,"requestCompleted":true})",
              R"({"microphone":"invented","requested":true,"requestCompleted":true})",
              R"({"microphone":"authorized","requested":true,"requestCompleted":true,"extra":1})"})
            refuses([&] {
                (void)decodeMicrophoneSetup({0, malformed});
            });
        refuses([] {
            (void)decodeMicrophoneSetup(
                {1, R"({"microphone":"authorized","requested":true,"requestCompleted":true})"});
        });
        refuses([] {
            (void)decodeMicrophoneSetup(
                {0, R"({"microphone":"denied","requested":true,"requestCompleted":true})"});
        });

        const auto executable = std::filesystem::absolute(argv[0]).string();
        const auto denied = decodeMicrophoneSetup(sessionCommand(
            {executable, "--denied"}, 5s, nullptr, SessionCommandOutput::StandardOutput));
        assert(denied.status == MicrophonePermission::Denied && denied.requested &&
               denied.completed);
        assert(sessionCommand({executable, "--denied"}).output.find("Simulated foreground") !=
               std::string::npos);
        refuses([&] {
            (void)sessionCommand({executable, "--sleep"}, 0ms);
        });
        refuses([&] {
            (void)sessionCommand({executable, "--sleep"}, 71s);
        });
        auto started = std::chrono::steady_clock::now();
        refuses([&] {
            (void)sessionCommand({executable, "--sleep"}, 30ms);
        });
        assert(std::chrono::steady_clock::now() - started < 2s);
        std::atomic<bool> running{true};
        std::jthread cancel([&] {
            std::this_thread::sleep_for(30ms);
            running.store(false);
        });
        started = std::chrono::steady_clock::now();
        refuses([&] {
            (void)sessionCommand({executable, "--sleep"}, 5s, &running);
        });
        assert(std::chrono::steady_clock::now() - started < 2s);
        refuses([&] {
            (void)sessionCommand({executable, "--denied"}, 5s, &running);
        });
        refuses([&] {
            (void)sessionCommand({executable, "--flood"});
        });
        int status = 0;
        assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    }
}
