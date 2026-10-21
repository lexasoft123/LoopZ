#include "connections.hpp"
#include <loopz/macos/permissions.hpp>
#include <algorithm>
#include <cstdio>
#include <utility>

namespace loopz::macos {
bool Monitor::render(void* context, const singz::AudioHostRenderBlock& block) noexcept {
    auto& self = *static_cast<Monitor*>(context);
    self.lastCallbackNs.store(loopz::hostNowNs());
    if (block.outputChannels < 2 || !block.outputTimestampValid) {
        self.timestampErrors.fetch_add(1);
        return false;
    }
    if (!block.outputTimestampHardware ||
        (self.havePrevious && (block.outputFrame != self.expectedFrame ||
                               block.streamGeneration != self.streamGeneration)))
        self.timestampErrors.fetch_add(1);
    self.expectedFrame = block.outputFrame + block.frames;
    self.streamGeneration = block.streamGeneration;
    self.havePrevious = true;
    if (!self.originNs) {
        self.originNs = block.outputHostTimeNs;
        self.originFrame = block.outputFrame;
    }
    if (block.outputHostTimeNs > self.originNs + 1000000000) {
        const double measured =
            (block.outputFrame - self.originFrame) * 1e9 / (block.outputHostTimeNs - self.originNs);
        self.driftPpb.store(static_cast<int64_t>((measured / self.rate - 1) * 1e9));
    }
    const auto first = loopz::hostFrame(block.outputHostTimeNs, self.rate);
    for (uint32_t i = 0; i < block.frames; ++i) {
        float al = 0, ar = 0, bl = 0, br = 0;
        const double source = static_cast<double>(first + i) - 2048;
        // The zcore provider supplies hardware sample timestamps; convert each
        // monitor's nominal rate into the virtual clock rather than assuming
        // its callback partition or sample rate matches the graph.
        const double position = source * self.audio->rate.load() / self.rate;
        const auto frame = position > 0 ? static_cast<uint64_t>(position) : 0;
        if (!self.audio->capture.read(frame, al, ar) ||
            !self.audio->capture.read(frame + 1, bl, br))
            self.missing.fetch_add(1);
        const float fraction = static_cast<float>(position - frame);
        block.output[0][i] = self.muted ? 0 : al + (bl - al) * fraction;
        block.output[1][i] = self.muted ? 0 : ar + (br - ar) * fraction;
    }
    self.rendered.fetch_add(block.frames);
    return true;
}
EngineConnections::EngineConnections(SharedAudio* audio, double rate, EngineConnectionConfig config)
    : config_(std::move(config)), monitor_{audio, rate, true} {
    std::vector<RecoveryNode> desired;
    if (config_.application)
        desired.push_back({"application", {}});
    if (config_.microphone)
        desired.push_back({"microphone", {}});
    if (!config_.monitorUID.empty())
        desired.push_back({"monitor", {}});
    recovery_.setDesired(std::move(desired), hostNowNs() / 1000000);
}
EngineConnections::~EngineConnections() {
    stop();
}
void EngineConnections::stop() {
    stopped_ = true;
    applicationActive_.store(false);
    microphoneActive_.store(false);
    if (monitorHost_) {
        monitorHost_->stop();
        monitorHost_.reset();
    }
    application_.stop();
    microphone_.stop();
}
void EngineConnections::poll(uint64_t now) {
    if (stopped_)
        return;
    std::string error;

    const auto milliseconds = now / 1000000;
    auto lost = [&](const char* id, const std::string& reason) {
        recovery_.lost(id, milliseconds);
        std::fprintf(stderr, "%s disconnected: %s; restoring saved route\n", id, reason.c_str());
    };
    if (config_.application && recovery_.ready("application") && !application_.healthy(error)) {
        applicationActive_.store(false);
        application_.stop();
        lost("application", error);
    }
    if (config_.microphone && recovery_.ready("microphone")) {
        const auto clock = microphone_.mic().clock();
        const auto last = std::max(clock.anchorNs, microphoneOpenedAt_);
        if (loopz::macos::microphonePermission() !=
                loopz::macos::MicrophonePermission::Authorized ||
            !microphone_.healthy(error) || (now >= last && now - last > 2000000000ULL)) {
            microphoneActive_.store(false);
            microphone_.stop();
            lost("microphone", "permission, device, format, or callback stream lost");
        }
    }
    if (!config_.monitorUID.empty() && recovery_.ready("monitor")) {
        const auto status = monitorHost_->status();
        const auto device = monitorHost_->describeOutputDevice(config_.monitorUID);
        const auto lastCallback = monitor_.lastCallbackNs.load();
        if (status.state != singz::AudioHostState::Running || !device ||
            device->nominalSampleRate != monitor_.rate ||
            (now > monitorOpenedAt_ + 2000000000ULL &&
             (!lastCallback || (now >= lastCallback && now - lastCallback > 2000000000ULL)))) {
            monitorHost_->stop();
            monitorHost_.reset();
            lost("monitor", "device, format, or callback stream lost");
        }
    }
    for (const auto& attempt : recovery_.attempts(milliseconds)) {
        bool ok = false;
        error.clear();
        if (attempt.id == "application") {
            ok = application_.startApplication(config_.appBundle, config_.muteApplication, error,
                                               config_.capture);

        } else if (attempt.id == "microphone") {
            // Recovery checks consent; only explicit permission setup
            // is allowed to ask macOS for a new protected grant.
            if (loopz::macos::microphonePermission() ==
                loopz::macos::MicrophonePermission::Authorized) {
                const auto uid = config_.microphoneUID;
                ok = microphone_.startMicrophone(uid, error);
            } else
                error = "Microphone access needs LoopZ permission setup";
            microphoneOpenedAt_ = loopz::hostNowNs();

        } else {
            monitorHost_ = std::make_unique<singz::AudioHost>();
            auto device = monitorHost_->describeOutputDevice(config_.monitorUID);
            if (device && device->outputChannels >= 2) {
                monitor_.rate = device->nominalSampleRate;
                monitor_.havePrevious = false;
                monitor_.originNs = 0;
                monitor_.lastCallbackNs.store(0);
                singz::AudioHostConfig config;
                config.outputDeviceUid = config_.monitorUID;
                config.outputChannels = {0, 1};
                config.requestedBufferFrames = loopz::kBlock;
                const auto opened = monitorHost_->open(config, Monitor::render, &monitor_);
                ok = opened.ok && monitorHost_->start().ok;
                error = opened.message;
            } else
                error = "Stereo monitor device not found";
            monitorOpenedAt_ = loopz::hostNowNs();
            if (!ok) {
                monitorHost_->stop();
                monitorHost_.reset();
            }
        }
        if (!recovery_.complete(attempt, ok, loopz::hostNowNs() / 1000000)) {
            // A slow platform call must not publish a stale acquisition.
            if (attempt.id == "application") {
                applicationActive_.store(false);
                application_.stop();
            } else if (attempt.id == "microphone") {
                microphoneActive_.store(false);
                microphone_.stop();
            } else if (monitorHost_) {
                monitorHost_->stop();
                monitorHost_.reset();
            }
            ok = false;
        }
        if (attempt.id == "application")
            applicationActive_.store(ok);
        else if (attempt.id == "microphone")
            microphoneActive_.store(ok);
        std::fprintf(stderr, "%s: %s%s%s\n", attempt.id.c_str(),
                     ok ? "connected" : "waiting to reconnect", error.empty() ? "" : ": ",
                     error.c_str());
    }
    const bool restored = recovery_.restored();
    if (restored != previouslyRestored_) {
        std::printf("{\"event\":\"%s\"}\n", restored ? "graph-restored" : "graph-degraded");
        std::fflush(stdout);
        previouslyRestored_ = restored;
    }
}
} // namespace loopz::macos
