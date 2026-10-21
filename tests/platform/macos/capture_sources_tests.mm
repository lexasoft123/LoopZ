#include <loopz/macos/capture_sources.hpp>
#include <loopz/macos/capture_layout.hpp>
#include <loopz/macos/source_diagnostics.hpp>
#include <loopz/timeline.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace loopz;
using namespace loopz::macos;
void speakerLayouts() {
    constexpr auto headerBytes = offsetof(AudioChannelLayout, mChannelDescriptions);
    const auto make = [&](AudioChannelLayoutTag tag, uint32_t count) {
        AudioChannelLayout layout{};
        layout.mChannelLayoutTag = tag;
        std::string error = "previous error";
        const auto result = stereoCapturePlan(layout, headerBytes, count, error);
        assert(result && error.empty());
        return *result;
    };
    const auto coefficient = [](const CaptureChannelPlan& plan, uint32_t source, uint32_t output) {
        for (const auto& wire : plan.connections)
            if (wire.sourceChannel == source && wire.outputChannel == output)
                return wire.gain;
        return 0.0f;
    };
    const auto equal = [](float actual, float expected) {
        assert(std::fabs(actual - expected) < 0.00001f);
    };
    const auto mono = make(kAudioChannelLayoutTag_Mono, 1);
    equal(coefficient(mono, 0, 0), 1);
    equal(coefficient(mono, 0, 1), 1);
    const auto stereo = make(kAudioChannelLayoutTag_Stereo, 2);
    assert(identityStereoPlan(stereo));
    const auto a = make(kAudioChannelLayoutTag_MPEG_5_1_A, 6);
    const auto d = make(kAudioChannelLayoutTag_MPEG_5_1_D, 6);
    assert(!a.discrete && !d.discrete && !sameCapturePlan(a, d));
    assert(sameCapturePlan(a, make(kAudioChannelLayoutTag_MPEG_5_1_A, 6)));
    const auto centerGain = static_cast<float>(std::sqrt(0.5));
    for (auto layout : {a, d}) {
        const bool reordered = sameCapturePlan(layout, d);
        const auto left = reordered ? 1U : 0U, right = reordered ? 2U : 1U;
        const auto center = reordered ? 0U : 2U, lfe = reordered ? 5U : 3U;
        equal(coefficient(layout, left, 0), 1);
        equal(coefficient(layout, right, 1), 1);
        equal(coefficient(layout, center, 0), centerGain);
        equal(coefficient(layout, center, 1), centerGain);
        equal(coefficient(layout, lfe, 0), 0);
        equal(coefficient(layout, lfe, 1), 0);
    }
    const auto wide = make(kAudioChannelLayoutTag_MPEG_7_1_A, 8);
    equal(coefficient(wide, 0, 0), 1);
    equal(coefficient(wide, 1, 1), 1);
    equal(coefficient(wide, 6, 0), std::sqrt(0.75f));
    equal(coefficient(wide, 6, 1), 0.5f);
    equal(coefficient(wide, 7, 0), 0.5f);
    equal(coefficient(wide, 7, 1), std::sqrt(0.75f));

    // Feed channel impulses through the real portable SingZ graph. Reordered
    // layouts must send each speaker to its matrix destination, without a
    // limiter or channel-count normalization in the capture stage.
    for (const auto& plan : {a, d, wide}) {
        const uint32_t channels = sameCapturePlan(plan, wide) ? 8 : 6;
        RoutingConfiguration configuration;
        configuration.safetyLimiter = false;
        configuration.sources = {{"speaker-stream", channels, 1, true, false, plan.connections}};
        for (double rate : {44100.0, 48000.0}) {
            RoutingGraph graph(rate, configuration);
            std::array<std::array<float, kBlock>, 8> samples{};
            std::array<const float*, 8> planes{};
            for (uint32_t channel = 0; channel < channels; ++channel)
                planes[channel] = samples[channel].data();
            const std::array<RoutingInput, 1> input{{{{planes.data(), channels}, kBlock}}};
            std::array<float, kBlock> left{}, right{};
            float* output[]{left.data(), right.data()};
            for (uint32_t active = 0; active < channels; ++active) {
                samples[active].fill(0.25f);
                assert(graph.render(input, {output, kBlock}, kBlock, active * kBlock));
                for (uint32_t frame = 0; frame < kBlock; ++frame) {
                    equal(left[frame], 0.25f * coefficient(plan, active, 0));
                    equal(right[frame], 0.25f * coefficient(plan, active, 1));
                }
                samples[active].fill(0);
            }
        }
    }
    // A description-based reversed stereo pair must not use the identity fast
    // path in either interleaved or planar capture.
    struct {
        AudioChannelLayoutTag tag;
        AudioChannelBitmap bitmap;
        UInt32 count;
        AudioChannelDescription channels[2];
    } reversed{kAudioChannelLayoutTag_UseChannelDescriptions, 0, 2, {}};
    reversed.channels[0].mChannelLabel = kAudioChannelLabel_Right;
    reversed.channels[1].mChannelLabel = kAudioChannelLabel_Left;
    std::string error;
    const auto reverse = stereoCapturePlan(*reinterpret_cast<AudioChannelLayout*>(&reversed),
                                           sizeof(reversed), 2, error);
    assert(reverse && !identityStereoPlan(*reverse));
    equal(coefficient(*reverse, 0, 1), 1);
    equal(coefficient(*reverse, 1, 0), 1);
    AudioChannelLayout broken{};
    broken.mChannelLayoutTag = kAudioChannelLayoutTag_MPEG_5_1_A;
    assert(!stereoCapturePlan(broken, headerBytes, 2, error) && !error.empty());
    broken.mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
    broken.mNumberChannelDescriptions = 64;
    assert(!stereoCapturePlan(broken, sizeof(broken), 64, error));
    assert(!stereoCapturePlan(broken, headerBytes - 1, 2, error));
}
void channelLayouts() {
    constexpr auto headerBytes = offsetof(AudioChannelLayout, mChannelDescriptions);
    for (uint32_t channels : {1U, 2U, 16U, 64U}) {
        const auto bytes = headerBytes + channels * sizeof(AudioChannelDescription);
        std::vector<uint64_t> storage((bytes + 7) / 8);
        auto& layout = *reinterpret_cast<AudioChannelLayout*>(storage.data());
        layout.mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
        layout.mNumberChannelDescriptions = channels;
        for (uint32_t channel = 0; channel < channels; ++channel)
            layout.mChannelDescriptions[channel].mChannelLabel = kAudioChannelLabel_Unknown;
        assert(discreteCaptureLayout(layout, bytes, channels));
        assert(!discreteCaptureLayout(layout, bytes - 1, channels));
        for (uint32_t channel = 0; channel < channels; ++channel)
            layout.mChannelDescriptions[channel].mChannelLabel =
                kAudioChannelLabel_Discrete_0 | channel;
        assert(discreteCaptureLayout(layout, bytes, channels));
        layout.mChannelDescriptions[channels - 1].mChannelLabel = kAudioChannelLabel_Center;
        assert(!discreteCaptureLayout(layout, bytes, channels));
        layout.mChannelDescriptions[channels - 1].mChannelLabel = kAudioChannelLabel_Unknown;
        layout.mChannelDescriptions[channels - 1].mChannelFlags =
            kAudioChannelFlags_RectangularCoordinates;
        assert(!discreteCaptureLayout(layout, bytes, channels));
        layout.mChannelLayoutTag = kAudioChannelLayoutTag_MPEG_5_1_A;
        assert(!discreteCaptureLayout(layout, bytes, channels));
        layout.mChannelLayoutTag = kAudioChannelLayoutTag_DiscreteInOrder | channels;
        assert(discreteCaptureLayout(layout, headerBytes, channels));
        assert(!discreteCaptureLayout(layout, headerBytes - 1, channels));
        assert(!discreteCaptureLayout(layout, bytes, channels + 1));
        layout.mChannelLayoutTag = kAudioChannelLayoutTag_Unknown | channels;
        assert(discreteCaptureLayout(layout, headerBytes, channels));
        assert(!discreteCaptureLayout(layout, bytes, 0));
        assert(!discreteCaptureLayout(layout, bytes, 65));
    }
}
template <typename F> void rejects(F operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}
void unopenedSources() {
    for (auto period : {32U, 64U, 128U}) {
        auto factory = captureSourceFactory({period});
        auto app = factory(SourceKind::Application, "com.example.not-running", 2);
        assert(app->channels() == 2);
        app->close();
        auto input = factory(SourceKind::InputDevice, "not-an-installed-device", 8);
        assert(input->channels() == 8);
        input->close();
    }
    for (auto period : {0U, 16U, 96U, 256U, UINT32_MAX})
        rejects([period] {
            captureSourceFactory({period});
        });
    rejects([] {
        captureSourceFactory({128, false, static_cast<ResamplingFilter>(99)});
    });
    for (const auto filter : {ResamplingFilter::LinearPhase, ResamplingFilter::MinimumPhase})
        for (auto count : {1U, 2U, 8U, 64U}) {
            auto source = captureSourceFactory({128, false, filter})(
                SourceKind::InputDevice, "not-an-installed-device", count);
            assert(source->channels() == count && !source->complete());
            std::array<std::array<float, 64>, 64> values{};
            std::array<float*, 64> planes{};
            for (unsigned channel = 0; channel < count; ++channel) {
                planes[channel] = values[channel].data();
                values[channel].fill(9);
            }
            AudioReadCursor cursor;
            auto result = source->read(1000000000, 48000, {{planes.data(), count}, 64}, 64, cursor);
            assert(!result.online && result.missingFrames == 64);
            for (unsigned channel = 0; channel < count; ++channel)
                for (auto value : values[channel])
                    assert(value == 0);
            source->close();
            source->close();
        }
    auto app = createCaptureSource(SourceKind::Application, "com.example.not-running", 2);
    std::array<float, 64> left{}, right{};
    float* planes[]{left.data(), right.data()};
    AudioReadCursor cursor;
    assert(!app->read(1000000000, 48000, {planes, 64}, 64, cursor).online);
    assert(!inputHistoryReadDiagnostics(*app));
    app->close();
    for (bool enabled : {false, true}) {
        auto source = captureSourceFactory({128, enabled})(SourceKind::InputDevice,
                                                           "not-an-installed-device", 2);
        assert(source->read(1000000000, 48000, {planes, 64}, 64, cursor).missingFrames == 64);
        source->close();
        const auto observed = inputHistoryReadDiagnostics(*source);
        assert(observed.has_value() == enabled);
        const auto diagnostic = source->diagnostics();
        assert(bool(diagnostic) == enabled);
        if (observed) {
            assert(observed->enabled && observed->reads == 1 && observed->requestedFrames == 64);
            assert(
                observed->missingFrames[static_cast<size_t>(AudioHistoryMissingReason::Offline)] ==
                64);
            assert(diagnostic->stoppedDelivery);
            NSDictionary* encoded = sourceDiagnosticsJSON(*diagnostic);
            assert([encoded[@"history"][@"requestedFrames"] isEqual:@"64"]);
            assert([encoded[@"history"][@"missingOutputFrames"][@"offline"] isEqual:@"64"]);
            assert([NSJSONSerialization isValidJSONObject:encoded]);
        }
    }
    SourceDiagnostics diagnostic;
    diagnostic.history.reads = UINT64_MAX;
    diagnostic.stoppedDelivery.emplace();
    diagnostic.stoppedDelivery->minimumReadyEndAgeNs = -123;
    diagnostic.stoppedDelivery->eventCount = 1;
    diagnostic.stoppedDelivery->events[0].sourceFrameDelta = NAN;
    NSDictionary* encoded = sourceDiagnosticsJSON(diagnostic);
    assert([encoded[@"history"][@"reads"] isEqual:@"18446744073709551615"]);
    assert([encoded[@"stoppedDelivery"][@"minimumReadyEndAgeNs"] isEqual:@"-123"]);
    assert([NSJSONSerialization isValidJSONObject:encoded]);
    rejects([] {
        createCaptureSource(SourceKind::InputDevice, "com.lexasoft.loopz.audio.feasibility.v1", 2);
    });
    rejects([] {
        createCaptureSource(SourceKind::InputDevice, "com.lexasoft.loopz.device.saved-device", 2);
    });
    rejects([] {
        createCaptureSource(SourceKind::Application, "com.lexasoft.loopz.engine", 2);
    });
    rejects([] {
        createCaptureSource(SourceKind::Application, "com.example.app", 64);
    });
    rejects([] {
        createCaptureSource(SourceKind::PassThrough, "other", 2);
    });
    rejects([] {
        createCaptureSource(SourceKind::InputDevice, std::string("wrong\0device", 12), 2);
    });
}
} // namespace
int main() {
    @autoreleasepool {
        channelLayouts();
        speakerLayouts();
        unopenedSources();
        std::puts("Native source construction: no capture/permission acquisition; 1/2/8/64-channel "
                  "layout and feedback guards passed");
    }
}
