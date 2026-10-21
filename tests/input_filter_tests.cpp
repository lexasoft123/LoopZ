#include <loopz/audio_history.hpp>
#include <loopz/input_timing.hpp>
#include <loopz/timeline.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <stdexcept>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace {
using namespace loopz;
VirtualDeviceDefinition device(std::string id) {
    VirtualDeviceDefinition result;
    result.id = id;
    result.name = id;
    result.routing.outputChannels = 2;
    result.routing.safetyLimiter = false;
    return result;
}
void source(VirtualDeviceDefinition& device, std::string id, SourceKind kind, std::string selector,
            bool enabled = true) {
    device.bindings.push_back({id, kind, selector});
    device.routing.sources.push_back({id, 2, 1, enabled, false, {{0, 0, 1}}});
}
ResamplingFilter selected(const std::vector<DeviceInputFilter>& plan, const std::string& device,
                          const std::string& source = "mic") {
    const auto found = std::find_if(plan.begin(), plan.end(), [&](const auto& item) {
        return item.device == device && item.source == source;
    });
    assert(found != plan.end());
    return found->filter;
}
void graphPolicy() {
    auto solo = device("solo"), upstream = device("upstream"), mixed = device("mixed");
    source(solo, "mic", SourceKind::InputDevice, "same-hardware");
    source(upstream, "mic", SourceKind::InputDevice, "same-hardware");
    source(mixed, "virtual", SourceKind::VirtualDevice, upstream.id);
    source(mixed, "app", SourceKind::Application, "app.bundle");
    auto duplicate = device("duplicate");
    source(duplicate, "mic", SourceKind::InputDevice, "same-hardware");
    source(duplicate, "other-channel", SourceKind::InputDevice, "same-hardware");
    auto twoInputs = device("two-inputs");
    source(twoInputs, "mic", SourceKind::InputDevice, "first");
    source(twoInputs, "other", SourceKind::InputDevice, "second");
    auto pass = device("pass");
    source(pass, "mic", SourceKind::InputDevice, "first");
    source(pass, "pass", SourceKind::PassThrough, "");
    auto disabledSource = device("disabled-source");
    source(disabledSource, "mic", SourceKind::InputDevice, "first");
    source(disabledSource, "app", SourceKind::Application, "app.bundle", false);
    auto disabledDevice = mixed;
    disabledDevice.id = disabledDevice.name = "disabled-device";
    disabledDevice.routing.enabled = false;
    auto muted = device("muted");
    source(muted, "mic", SourceKind::InputDevice, "first");
    source(muted, "app", SourceKind::Application, "app.bundle");
    muted.routing.sources.back().muted = true;
    RouteDocument document{
        kRouteDocumentVersion,
        {mixed, solo, duplicate, twoInputs, pass, disabledSource, disabledDevice, muted, upstream}};
    const auto plan = planPhysicalInputFilters(document, ResamplingFilter::MinimumPhase);
    assert(selected(plan, "solo") == ResamplingFilter::MinimumPhase);
    assert(selected(plan, "upstream") == ResamplingFilter::LinearPhase);
    assert(selected(plan, "duplicate") == ResamplingFilter::MinimumPhase);
    assert(selected(plan, "duplicate", "other-channel") == ResamplingFilter::MinimumPhase);
    assert(selected(plan, "two-inputs") == ResamplingFilter::LinearPhase);
    assert(selected(plan, "two-inputs", "other") == ResamplingFilter::LinearPhase);
    assert(selected(plan, "pass") == ResamplingFilter::LinearPhase);
    assert(selected(plan, "muted") == ResamplingFilter::LinearPhase);
    assert(selected(plan, "disabled-source") == ResamplingFilter::MinimumPhase);
    const auto linear = planPhysicalInputFilters(document, ResamplingFilter::LinearPhase);
    assert(linear.size() == plan.size());
    assert(std::all_of(linear.begin(), linear.end(), [](const auto& item) {
        return item.filter == ResamplingFilter::LinearPhase;
    }));
    bool rejected = false;
    try {
        planPhysicalInputFilters(document, static_cast<ResamplingFilter>(99));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}
class Sink final : public AudioSink {
  public:
    std::array<float, 32> samples{};
    uint32_t channels() const noexcept override {
        return 2;
    }
    bool requested() const noexcept override {
        return true;
    }
    bool publish(uint64_t, uint32_t, RoutingInput input, uint32_t frames) noexcept override {
        assert(frames == samples.size());
        std::copy_n(input.channels[0], frames, samples.begin());
        return true;
    }
    void idle() noexcept override {}
};
void correlatedSignalAndIndependentReaders() {
    auto physical = std::make_shared<AudioHistory>(2, 32768, ResamplingFilter::MinimumPhase);
    auto application = std::make_shared<AudioHistory>(2, 32768);
    std::array<float, 64> input{}, inverse{};
    std::array<const float*, 2> inputPlane{input.data(), input.data()},
        inversePlane{inverse.data(), inverse.data()};
    constexpr uint64_t originFrame = 48000;
    for (size_t frame = 0; frame < 8192; frame += input.size()) {
        for (size_t i = 0; i < input.size(); ++i) {
            input[i] = static_cast<float>(.2 * std::sin(2 * std::numbers::pi * 8000 *
                                                      static_cast<double>(frame + i) / 48000));
            inverse[i] = -input[i];
        }
        const auto time = frameNs(originFrame + frame, 48000);
        assert(physical->append({inputPlane, 64}, 64, time, 48000, frame, true));
        assert(application->append({inversePlane, 64}, 64, time, 48000, frame, true));
    }
    auto mix = device("mix");
    source(mix, "mic", SourceKind::InputDevice, "hardware");
    source(mix, "app", SourceKind::Application, "app.bundle");
    RouteDocument document{kRouteDocumentVersion, {mix}};
    auto sink = std::make_shared<Sink>();
    const auto resolve = [&](const auto&, const SourceBinding& binding, const auto&) {
        return ResolvedAudioSource{binding.kind == SourceKind::InputDevice ? physical : application,
                                   std::make_shared<AudioReadCursor>()};
    };
    const auto output = [&](const auto&) {
        return sink;
    };
    const SceneTiming timing{48000, 32, 1024, 128};
    AudioScene mismatched(document, timing, resolve, output);
    assert(mismatched.render(originFrame + 6000 + 1024, 32).validBlock);
    const auto peak = [](const auto& samples) {
        double result = 0;
        for (const auto sample : samples)
            result = std::max(result, std::abs(double(sample)));
        return result;
    };
    const auto mismatch = peak(sink->samples);
    assert(mismatch > .01);
    const std::vector<DeviceInputDelay> aligned{
        {mix.id, "mic", 1024, ResamplingFilter::LinearPhase}};
    AudioScene corrected(document, timing, resolve, output, {}, {}, aligned);
    assert(corrected.render(originFrame + 6000 + 1024, 32).validBlock);
    assert(peak(sink->samples) < 1e-6);
    assert(corrected.sourceStatus(0, 0).missingFrames == 0);
    assert(corrected.sourceStatus(0, 1).missingFrames == 0);
    assert(corrected.sourceStatus(0, 0).resamplingFilter == ResamplingFilter::LinearPhase);
    assert(!corrected.sourceStatus(0, 1).resamplingFilter);

    // The same producer still supplies its causal reader independently. A
    // prepared replacement has not mutated the old scene or its shared history.
    assert(mismatched.render(originFrame + 6000 + 1024, 32).validBlock);
    assert(std::abs(peak(sink->samples) - mismatch) < 1e-6);
    assert(physical->status().callbacks == 128);
    std::printf("Correlated 8 kHz mix: mismatched peak %.6f; linear fallback below 1e-6\n",
                mismatch);

    auto upstream = device("upstream");
    source(upstream, "mic", SourceKind::InputDevice, "hardware");
    auto downstream = device("downstream");
    source(downstream, "virtual", SourceKind::VirtualDevice, upstream.id);
    source(downstream, "app", SourceKind::Application, "app.bundle");
    RouteDocument graph{kRouteDocumentVersion, {downstream, upstream}};
    const auto filters = planPhysicalInputFilters(graph, ResamplingFilter::MinimumPhase);
    assert(filters.size() == 1 && filters[0].filter == ResamplingFilter::LinearPhase);
    const std::vector<DeviceInputDelay> upstreamTiming{
        {upstream.id, "mic", 384, filters[0].filter}};
    AudioScene dependency(
        graph, timing, resolve,
        [&](const auto& definition) -> std::shared_ptr<AudioSink> {
            return definition.id == downstream.id ? sink : nullptr;
        },
        {}, {}, upstreamTiming);
    const auto target = originFrame + 6000 + 1024;
    for (auto frame = target - 640; frame <= target; frame += 32)
        assert(dependency.render(frame, 32).validBlock);
    assert(peak(sink->samples) < 1e-6);
    assert(dependency.sourceStatus(1, 0).missingFrames == 0);
    assert(dependency.sourceStatus(1, 0).resamplingFilter == ResamplingFilter::LinearPhase);

    std::array<float, 32> scratch{};
    std::array<float*, 1> plane{scratch.data()};
    AudioReadCursor cursor;
    const auto result =
        application->readWithFilter(frameNs(originFrame + 6000, 48000), 48000, {plane, 32}, 32,
                                    cursor, ResamplingFilter::MinimumPhase);
    assert(!result.online && result.missingFrames == 32);
    assert(peak(scratch) == 0);
}
void mixedPhaseAcrossEndpointRates() {
    constexpr uint64_t origin = 5000000000ULL;
    double worst = 0;
    for (const auto sourceRate : {8000U, 44100U, 48000U, 96000U, 192000U}) {
        for (const auto renderRate : {8000U, 44100U, 48000U, 96000U, 192000U}) {
            for (const auto frequency : {997., .38 * std::min(sourceRate, renderRate)}) {
                AudioHistory physical(2, 32768, ResamplingFilter::MinimumPhase);
                AudioHistory application(2, 32768);
                const auto append = [&](AudioHistory& history, uint32_t rate, double sign) {
                    std::array<float, 64> samples{};
                    const std::array<const float*, 2> planes{samples.data(), samples.data()};
                    for (uint32_t first = 0; first < rate / 10; first += 64) {
                        for (uint32_t frame = 0; frame < samples.size(); ++frame)
                            samples[frame] =
                                static_cast<float>(sign * .2 *
                                std::sin(2 * std::numbers::pi * frequency * (first + frame) / rate));
                        assert(history.append({planes, 64}, 64, origin + frameNs(first, rate), rate,
                                              first, true));
                    }
                };
                append(physical, sourceRate, 1);
                append(application, renderRate, -1);
                std::array<float, 32> left{}, right{};
                const std::array<float*, 1> physicalOutput{left.data()}, appOutput{right.data()};
                AudioReadCursor micCursor, appCursor;
                for (const uint64_t offset : {30000000ULL, 30000317ULL, 40000771ULL}) {
                    const auto microphone =
                        physical.readWithFilter(origin + offset, renderRate, {physicalOutput, 32},
                                                32, micCursor, ResamplingFilter::LinearPhase);
                    const auto app = application.read(origin + offset, renderRate, {appOutput, 32},
                                                      32, appCursor);
                    assert(microphone.online && !microphone.missingFrames);
                    assert(app.online && !app.missingFrames);
                    for (uint32_t frame = 0; frame < 32; ++frame)
                        worst = std::max(worst, std::abs(double(left[frame]) + right[frame]));
                }
            }
        }
    }
    std::printf("Mixed linear input phase across 8..192 kHz: maximum residual %.9f\n", worst);
    assert(worst < .00001);
}
} // namespace
int main() {
    graphPolicy();
    correlatedSignalAndIndependentReaders();
    mixedPhaseAcrossEndpointRates();
}
