#include "silent_monitor_backend.hpp"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>

namespace {
class FakeOutput final : public singz::AudioHostBackend {
  public:
    singz::AudioHostInventory enumerate() const override {
        return {};
    }
    singz::AudioHostResult open(const singz::AudioHostConfig&, singz::AudioHostRender callback,
                                void* context) override {
        render = callback;
        owner = context;
        ++opens;
        singz::AudioHostResult result;
        result.ok = true;
        return result;
    }
    singz::AudioHostResult start() override {
        singz::AudioHostResult result;
        result.ok = true;
        return result;
    }
    void stop() noexcept override {}
    singz::AudioHostStatus status() const noexcept override {
        return {};
    }
    singz::AudioHostRender render{};
    void* owner{};
    unsigned opens{};
};

bool badRenderer(void*, const singz::AudioHostRenderBlock& block) noexcept {
    std::fill_n(block.output[0], block.frames, .25f);
    block.output[1][0] = NAN;
    // A failed renderer must not bypass the final silence boundary.
    return false;
}
} // namespace

int main() {
    using namespace loopz::diagnostic;
    auto fake = std::make_unique<FakeOutput>();
    auto* output = fake.get();
    auto counts = std::make_shared<SilentMonitorCounters>();
    SilentMonitorBackend guarded("affected", "peer", counts, std::move(fake));
    singz::AudioHostConfig config;
    config.outputDeviceUid = "affected";
    config.outputChannels = {0, 1};
    for (const auto option : {1, 2, 3, 4}) {
        auto invalid = config;
        if (option == 1)
            invalid.requestedBufferFrames = 64;
        if (option == 2)
            invalid.requestedSampleRate = 44100;
        if (option == 3)
            invalid.exclusive = true;
        if (option == 4)
            invalid.outputDeviceUid = "unrelated";
        assert(!guarded.open(invalid, badRenderer, nullptr).ok && !output->opens);
    }
    assert(guarded.open(config, badRenderer, nullptr).ok && output->opens == 1);
    std::array<float, 64> left{}, right{};
    float* planes[]{left.data(), right.data()};
    singz::AudioHostRenderBlock block;
    block.output = planes;
    block.outputChannels = 2;
    block.frames = block.maximumFrames = 64;
    assert(!output->render(output->owner, block));
    assert(std::all_of(left.begin(), left.end(), [](float value) {
        return value == 0;
    }));
    assert(std::all_of(right.begin(), right.end(), [](float value) {
        return value == 0;
    }));
    assert(counts->callbacks == 1 && counts->frames == 64 && counts->nonzeroSamples == 65);
    assert(!counts->invalidBlocks && counts->opens == 1);
    block.frames = 0;
    assert(!output->render(output->owner, block));
    assert(counts->callbacks == 2 && counts->frames == 64 && counts->invalidBlocks == 1);
    left.fill(1);
    right.fill(1);
    block.frames = 65;
    assert(!output->render(output->owner, block));
    assert(std::all_of(left.begin(), left.end(), [](float value) {
        return value == 0;
    }));
    assert(std::all_of(right.begin(), right.end(), [](float value) {
        return value == 0;
    }));
    assert(counts->callbacks == 3 && counts->frames == 64 && counts->invalidBlocks == 2);
    std::puts("Silent monitor boundary clears nonfinite/nonzero failed-render output and rejects "
              "hardware format changes");
}
