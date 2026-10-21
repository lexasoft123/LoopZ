#pragma once

#include <loopz/macos/capture.hpp>
#include <loopz/recovery.hpp>
#include <zcore/device/audio_host.h>

namespace loopz::macos {

struct Monitor {
    loopz::SharedAudio* audio;
    double rate;
    bool muted;
    std::atomic<uint64_t> missing{0}, rendered{0}, timestampErrors{0};
    std::atomic<int64_t> driftPpb{0};
    std::atomic<uint64_t> lastCallbackNs{0};
    uint64_t originNs{0}, originFrame{0}, expectedFrame{0}, streamGeneration{0};
    bool havePrevious{false};
    static bool render(void* context, const singz::AudioHostRenderBlock& block) noexcept;
};

struct EngineConnectionConfig {
    bool application{false}, microphone{false}, muteApplication{false};
    std::string appBundle, microphoneUID, monitorUID;
    ApplicationCaptureConfig capture;
};

// One control-thread owner. Buffers and callback contexts remain alive across
// endpoint reconnection and until the caller has joined the virtual renderer.
class EngineConnections {
  public:
    EngineConnections(SharedAudio* audio, double rate, EngineConnectionConfig config);
    ~EngineConnections();
    void poll(uint64_t now);
    // Terminal and idempotent; later poll calls do not reopen resources.
    void stop();
    bool restored() const noexcept {
        return !stopped_ && recovery_.restored();
    }
    bool applicationActive() const noexcept {
        return applicationActive_.load();
    }
    bool microphoneActive() const noexcept {
        return microphoneActive_.load();
    }
    CaptureBuffer& app() {
        return application_.app();
    }
    CaptureBuffer& mic() {
        return microphone_.mic();
    }
    const Monitor& monitor() const noexcept {
        return monitor_;
    }

  private:
    EngineConnectionConfig config_;
    Capture application_, microphone_;
    std::atomic<bool> applicationActive_{false}, microphoneActive_{false};
    std::unique_ptr<singz::AudioHost> monitorHost_;
    Monitor monitor_;
    GraphRecovery recovery_;
    uint64_t microphoneOpenedAt_{0}, monitorOpenedAt_{0};
    bool previouslyRestored_{false}, stopped_{false};
};
} // namespace loopz::macos
