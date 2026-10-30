// Test-only bridge: the production engine control path renders into private
// memory. No service registration, installed driver, or audio permissions.
#include <loopz/macos/bridge.hpp>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

namespace loopz {
namespace {
unsigned connections = 0;
} // namespace
struct BridgeClient::Impl {
    std::unique_ptr<SharedAudio> memory;
    std::atomic<bool> connected{false};
    std::jthread faults;
    ~Impl() {
        faults.request_stop();
        if (faults.joinable())
            faults.join();
        // The owner must join the renderer and clear its publication flag
        // before unmapping even after an interruption.
        assert(!memory || !memory->helperOnline.load());
    }
};
BridgeClient::BridgeClient() : impl_(std::make_unique<Impl>()) {}
BridgeClient::~BridgeClient() = default;
bool BridgeClient::connect(const char*, std::string& error) {
    const auto attempt = ++connections;
    std::printf("{\"event\":\"test-bridge-connect\",\"attempt\":%u}\n", attempt);
    std::fflush(stdout);
    if (attempt < 3) {
        error = "Injected broker unavailability";
        return false;
    }
    impl_->memory = std::make_unique<SharedAudio>();
    impl_->memory->rate.store(attempt == 3 ? 48000 : 44100);
    impl_->connected.store(true);
    if (attempt == 3) {
        auto* state = impl_.get();
        impl_->faults = std::jthread([state](std::stop_token stop) {
            // Connection is only a control-plane milestone. Keep the initial
            // rate stable until the realtime renderer has been admitted and
            // has published at least one block, so injected service loss and
            // rate change exercise recovery rather than startup admission.
            while (!stop.stop_requested() && !state->memory->helperOnline.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            while (!stop.stop_requested() && state->memory->capture.newest.load() == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (stop.stop_requested())
                return;

            const auto start = std::chrono::steady_clock::now();
            while (!stop.stop_requested()) {
                const auto elapsed = std::chrono::steady_clock::now() - start;
                if (elapsed >= std::chrono::milliseconds(200))
                    state->memory->rate.store(44100);
                if (elapsed >= std::chrono::milliseconds(1000)) {
                    state->connected.store(false);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
        // Force attachment to outlast the former 200 ms startup-based fault
        // deadline. Faults must wait for rendering rather than this slow return.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    return true;
}
bool BridgeClient::connected() const noexcept {
    return impl_->connected.load();
}
SharedAudio* BridgeClient::audio() const noexcept {
    return impl_->memory.get();
}
BridgeStatus inspectBridge(std::string&) {
    return {kProtocol, true, true};
}
} // namespace loopz
