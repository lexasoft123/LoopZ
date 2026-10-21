#pragma once

#include <zcore/device/audio_host.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>

namespace loopz::diagnostic {
struct SilentMonitorCounters {
    std::atomic<uint64_t> callbacks{0}, frames{0}, nonzeroSamples{0}, invalidBlocks{0}, opens{0};
};

// Exercise the actual native output backend and monitor renderer, but clear
// every output sample at the final boundary regardless of the renderer result.
// No exclusive access, requested rate or requested buffer period is accepted.
class SilentMonitorBackend final : public singz::AudioHostBackend {
  public:
    SilentMonitorBackend(
        std::string affected, std::string peer, std::shared_ptr<SilentMonitorCounters> counters,
        std::unique_ptr<singz::AudioHostBackend> native = singz::createPlatformAudioHostBackend())
        : affected_(std::move(affected)), peer_(std::move(peer)), counters_(std::move(counters)),
          native_(std::move(native)) {
        if (!native_ || !counters_)
            throw std::invalid_argument("Silent monitor guard needs a backend and counters");
    }
    ~SilentMonitorBackend() override {
        native_->stop();
    }

    singz::AudioHostInventory enumerate() const override {
        return native_->enumerate();
    }
    std::optional<singz::AudioHostDeviceInfo>
    describeOutputDevice(const std::string& uid) const override {
        return native_->describeOutputDevice(uid);
    }
    singz::AudioHostResult open(const singz::AudioHostConfig& config, singz::AudioHostRender render,
                                void* context) override {
        if ((config.outputDeviceUid != affected_ && config.outputDeviceUid != peer_) ||
            config.exclusive || config.requestedSampleRate || config.requestedBufferFrames)
            return {false,
                    singz::AudioHostError::InvalidConfiguration,
                    singz::AudioHostState::Error,
                    {},
                    {},
                    "Diagnostic output must retain shared hardware format"};
        render_ = render;
        context_ = context;
        counters_->opens.fetch_add(1);
        return native_->open(config, callback, this);
    }
    singz::AudioHostResult start() override {
        return native_->start();
    }
    void stop() noexcept override {
        native_->stop();
    }
    singz::AudioHostStatus status() const noexcept override {
        return native_->status();
    }

  private:
    static bool callback(void* context, const singz::AudioHostRenderBlock& block) noexcept {
        auto& self = *static_cast<SilentMonitorBackend*>(context);
        const bool valid = block.output && block.frames && block.frames <= block.maximumFrames &&
                           block.maximumFrames <= singz::kAudioHostMaxFrames &&
                           block.outputChannels && block.outputChannels <= 64;
        const bool rendered = valid && self.render_ && self.render_(self.context_, block);
        if (!valid)
            self.counters_->invalidBlocks.fetch_add(1);
        uint64_t nonzero = 0;
        if (block.output)
            for (uint32_t channel = 0; channel < std::min(block.outputChannels, 64U); ++channel)
                if (block.output[channel])
                    for (uint32_t frame = 0; frame < std::min({block.frames, block.maximumFrames,
                                                               singz::kAudioHostMaxFrames});
                         ++frame) {
                        nonzero += block.output[channel][frame] != 0;
                        block.output[channel][frame] = 0;
                    }
        self.counters_->callbacks.fetch_add(1);
        self.counters_->frames.fetch_add(valid ? block.frames : 0);
        self.counters_->nonzeroSamples.fetch_add(nonzero);
        return rendered;
    }
    const std::string affected_, peer_;
    const std::shared_ptr<SilentMonitorCounters> counters_;
    const std::unique_ptr<singz::AudioHostBackend> native_;
    singz::AudioHostRender render_{};
    void* context_{};
};
} // namespace loopz::diagnostic
