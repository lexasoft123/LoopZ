#include <loopz/runtime/audio_runtime.hpp>
#include <loopz/audio_resampler.hpp>
#include <cassert>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <numbers>
#include <cstdio>
#include <limits>

namespace {
using namespace loopz;
struct Counts {
    unsigned opened{0}, stopped{0}, live{0}, maximumLive{0};
    bool fail{false}, lost{false};
    std::string failedUid;
    uint32_t outputBufferFrames{1024}, outputPresentationLeadFrames{0};
    singz::AudioInputBusPush push{nullptr};
    void* context{nullptr};
    std::string lastInputUid;
    singz::AudioHostRender render{nullptr};
    void* renderContext{nullptr};
};
class Host final : public singz::AudioHostBackend {
    std::shared_ptr<Counts> counts_;
    singz::AudioHostStatus status_;
    void* context_{nullptr};
    bool opened_{false};

  public:
    explicit Host(std::shared_ptr<Counts> counts) : counts_(std::move(counts)) {}
    singz::AudioHostInventory enumerate() const override {
        singz::AudioHostInventory result;
        result.defaultInputUid = "physical-input";
        return result;
    }
    singz::AudioHostResult open(const singz::AudioHostConfig& config, singz::AudioHostRender render,
                                void* context) override {
        ++counts_->opened;
        // The macOS provider refuses a buffer request without explicit consent.
        assert(config.requestedBufferFrames == 0 || config.allowSharedDeviceBufferChange);
        if (counts_->fail || config.outputDeviceUid == counts_->failedUid)
            return {false,
                    singz::AudioHostError::ProviderFailure,
                    singz::AudioHostState::Error,
                    {},
                    {},
                    "Output fixture failure"};
        if (!opened_) {
            ++counts_->live;
            counts_->maximumLive = std::max(counts_->maximumLive, counts_->live);
            opened_ = true;
        }
        counts_->render = render;
        counts_->renderContext = context;
        context_ = context;
        status_.state = singz::AudioHostState::Open;
        status_.format.sampleRate = 48000;
        status_.format.nominalBufferFrames = 256;
        status_.diagnostics.outputPeriodFrames = 256;
        status_.diagnostics.outputBufferFrames = counts_->outputBufferFrames;
        counts_->lost = false;
        status_.format.outputChannels = static_cast<uint32_t>(config.outputChannels.size());
        return {true, singz::AudioHostError::None, status_.state, status_.format, {}, {}};
    }
    singz::AudioHostResult start() override {
        status_.state = singz::AudioHostState::Running;
        return {true, singz::AudioHostError::None, status_.state, status_.format, {}, {}};
    }
    void stop() noexcept override {
        ++counts_->stopped;
        if (opened_) {
            --counts_->live;
            opened_ = false;
        }
        if (counts_->renderContext == context_) {
            counts_->render = nullptr;
            counts_->renderContext = nullptr;
        }
        context_ = nullptr;
        status_.state = singz::AudioHostState::Stopped;
    }
    singz::AudioHostStatus status() const noexcept override {
        auto result = status_;
        if (counts_->lost)
            result.state = singz::AudioHostState::DeviceLost;
        return result;
    }
};
class Input final : public singz::AudioInputBackend {
    std::shared_ptr<Counts> counts_;
    bool opened_{false};

  public:
    explicit Input(std::shared_ptr<Counts> counts) : counts_(std::move(counts)) {}
    singz::AudioInputResult open(const singz::AudioInputConfig&, singz::AudioInputPush,
                                 void*) override {
        return {};
    }
    singz::AudioInputBusResult openBus(const singz::AudioInputBusConfig& config,
                                       singz::AudioInputBusPush push, void* context) override {
        ++counts_->opened;
        counts_->lastInputUid = config.deviceUid;
        if (counts_->fail || config.deviceUid == counts_->failedUid)
            return {singz::AudioInputBusOpenStatus::Error,
                    singz::AudioInputResult::failure(singz::AudioInputState::Error,
                                                     "Capture fixture failure", 0)};
        if (!opened_) {
            ++counts_->live;
            counts_->maximumLive = std::max(counts_->maximumLive, counts_->live);
            opened_ = true;
        }
        counts_->push = push;
        counts_->context = context;
        return {singz::AudioInputBusOpenStatus::Ready,
                singz::AudioInputResult::success(singz::AudioInputState::Starting, 48000,
                                                 config.channels.front())};
    }
    singz::AudioInputResult start() override {
        return singz::AudioInputResult::success(singz::AudioInputState::Running, 48000, 0);
    }
    void stop() override {
        ++counts_->stopped;
        if (opened_) {
            --counts_->live;
            opened_ = false;
        }
    }
    bool takeFailure(std::string& error) override {
        if (!counts_->lost)
            return false;
        error = "Capture disconnected";
        return true;
    }
};
class Native final : public AudioSource {
  public:
    uint32_t channels() const noexcept override {
        return 2;
    }
    AudioReadResult read(uint64_t, double, RoutingOutput output, uint32_t frames,
                         AudioReadCursor&) const noexcept override {
        for (size_t channel = 0; channel < output.channels.size(); ++channel)
            std::fill_n(output.channels[channel], frames, channel ? 0.25f : 0.125f);
        return {0, true};
    }
};
RouteDocument route() {
    RouteDocument result;
    VirtualDeviceDefinition device;
    device.id = "mix";
    device.name = "Mix";
    SourceRouting input;
    input.id = "input";
    input.connections = defaultChannelConnections(2, 2);
    device.routing.sources.push_back(input);
    device.bindings.push_back({"input", SourceKind::InputDevice, "capture", false});
    MonitorDefinition monitor;
    monitor.id = "monitor";
    monitor.deviceSelector = "output";
    monitor.connections = defaultChannelConnections(2, 2);
    device.monitors.push_back(monitor);
    result.devices.push_back(device);
    return result;
}
template <class Function> void rejects(Function action) {
    bool failed = false;
    try {
        action();
    } catch (const std::exception&) {
        failed = true;
    }
    assert(failed);
}
void awaitRestored(runtime::AudioRuntime& engine) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!engine.status().restored && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(engine.status().restored);
}
// Exercise the actual output callback and common-clock histories, with no
// physical device and no hard dependence on the renderer's scheduling phase.
void awaitSignal(const std::shared_ptr<Counts>& hosts, float left, float right,
                 const std::function<void()>& feed = {}) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        if (feed)
            feed();
        std::array<float, 256> first{}, second{};
        float* planes[] = {first.data(), second.data()};
        const auto ns =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count());
        singz::AudioHostRenderBlock block;
        block.output = planes;
        block.outputChannels = 2;
        block.frames = block.maximumFrames = 256;
        block.sampleRate = 48000;
        block.callbackHostTimeNs = ns;
        block.outputHostTimeNs =
            ns + uint64_t(hosts->outputPresentationLeadFrames) * 1000000000 / 48000;
        block.outputFrame = ns / 1000000000 * 48000 + ns % 1000000000 * 48000 / 1000000000;
        block.outputTimestampValid = true;
        assert(hosts->render && hosts->render(hosts->renderContext, block));
        const auto agrees = [](const auto& samples, float expected) {
            return std::all_of(samples.begin(), samples.end(), [&](float value) {
                return std::isfinite(value) && std::abs(value - expected) < 0.00001f;
            });
        };
        if (agrees(first, left) && agrees(second, right))
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    assert(false && "Native routing output did not reach the expected samples");
}
uint64_t steadyNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}
uint64_t sampleNs(uint64_t frames) {
    return frames / 48000 * 1000000000 + frames % 48000 * 1000000000 / 48000;
}
struct CaptureFeed {
    std::shared_ptr<Counts> counts;
    uint32_t channels;
    std::array<std::array<float, 256>, singz::kAudioInputBusMaxChannels> samples{};
    std::array<const float*, singz::kAudioInputBusMaxChannels> planes{};
    uint64_t origin{steadyNs() - 150000000}, frame{0}, sequence{0};
    double clockRate{48000};
    int64_t jitterNs{0};
    singz::AudioInputBusBlock last{};
    CaptureFeed(std::shared_ptr<Counts> state, uint32_t width)
        : counts(std::move(state)), channels(width) {
        for (uint32_t channel = 0; channel < channels; ++channel) {
            samples[channel].fill(float(channel + 1) / 20.f);
            planes[channel] = samples[channel].data();
        }
    }
    void run() {
        const auto now = steadyNs();
        while (origin + static_cast<uint64_t>((frame + 256) * 1e9L / clockRate) <= now) {
            const auto timestamp =
                static_cast<uint64_t>(static_cast<long double>(origin) + frame * 1e9L / clockRate +
                                      ((sequence & 1) ? jitterNs : -jitterNs));
            singz::AudioInputBusBlock block{planes.data(),
                                            channels,
                                            256,
                                            256,
                                            timestamp,
                                            now,
                                            singz::AudioInputTimestampQuality::CallbackEstimate};
            block.capture = {
                11,
                7,
                sequence++,
                frame,
                block.firstNs,
                block.deliveryNs,
                block.quality,
                singz::AudioInputDiscontinuityReason::None,
                singz::AudioInputSourceFrameValid | singz::AudioInputSampleHostTimeValid |
                    singz::AudioInputCallbackHostTimeValid |
                    singz::AudioInputTimestampQualityValid | singz::AudioInputStaleAnchor};
            assert(counts->push(counts->context, block));
            last = block;
            frame += block.frames;
        }
    }
};
class SineClockSource final : public AudioSource {
    const uint64_t origin_;

  public:
    explicit SineClockSource(uint64_t origin) : origin_(origin) {}
    uint32_t channels() const noexcept override {
        return 2;
    }
    AudioReadResult read(uint64_t host, double hz, RoutingOutput out, uint32_t frames,
                         AudioReadCursor&) const noexcept override {
        const auto start = (static_cast<long double>(host) - origin_) / 1e9L;
        for (uint32_t c = 0; c < out.channels.size(); ++c)
            for (uint32_t frame = 0; frame < frames; ++frame)
                out.channels[c][frame] =
                    static_cast<float>(.1L * std::sin(2 * std::numbers::pi_v<long double> *
                                                      (c ? 1499 : 997) * (start + frame / hz)));
        return {0, true};
    }
};
std::array<double, 3> fitSine(std::span<const float> samples, double frequency, double clockRate) {
    // Independent least-squares DC/sine/cosine fit; a constant routing delay is
    // free phase, but callback-boundary phase modulation remains in residual.
    std::array<std::array<double, 4>, 3> rows{};
    for (size_t index = 0; index < samples.size(); ++index) {
        const double angle = 2 * std::numbers::pi_v<double> * frequency * index / clockRate;
        const std::array<double, 3> basis{1, std::sin(angle), std::cos(angle)};
        for (size_t a = 0; a < 3; ++a) {
            rows[a][3] += samples[index] * basis[a];
            for (size_t b = 0; b < 3; ++b)
                rows[a][b] += basis[a] * basis[b];
        }
    }
    for (size_t column = 0; column < 3; ++column) {
        size_t pivot = column;
        for (size_t row = column + 1; row < 3; ++row)
            if (std::abs(rows[row][column]) > std::abs(rows[pivot][column]))
                pivot = row;
        std::swap(rows[pivot], rows[column]);
        const auto divisor = rows[column][column];
        assert(std::abs(divisor) > 1e-9);
        for (auto& value : rows[column])
            value /= divisor;
        for (size_t row = 0; row < 3; ++row)
            if (row != column) {
                const auto factor = rows[row][column];
                for (size_t entry = 0; entry < 4; ++entry)
                    rows[row][entry] -= factor * rows[column][entry];
            }
    }
    return {rows[0][3], rows[1][3], rows[2][3]};
}
double sineResidual(const std::vector<float>& samples, double frequency, double clockRate) {
    // Fit a small constant frequency offset independently; clock-estimation
    // error is not distortion. Time-varying phase noise remains in residual.
    const auto half = samples.size() / 2;
    const std::span<const float> view(samples);
    const auto first = fitSine(view.first(half), frequency, clockRate);
    const auto second = fitSine(view.subspan(half), frequency, clockRate);
    const auto phase = std::atan2(second[2], second[1]) - std::atan2(first[2], first[1]) -
                       2 * std::numbers::pi_v<double> * frequency * half / clockRate;
    const auto adjustment = std::atan2(std::sin(phase), std::cos(phase)) * clockRate /
                            (2 * std::numbers::pi_v<double> * half);
    assert(std::abs(adjustment / frequency) < .0001); // Drift must be tracked, not ignored.
    frequency += adjustment;
    const auto fit = fitSine(samples, frequency, clockRate);
    double error = 0;
    for (size_t index = 0; index < samples.size(); ++index) {
        const double angle = 2 * std::numbers::pi_v<double> * frequency * index / clockRate;
        const auto delta =
            samples[index] - fit[0] - fit[1] * std::sin(angle) - fit[2] * std::cos(angle);
        error += delta * delta;
    }
    const auto amplitude = std::hypot(fit[1], fit[2]);
    assert(amplitude > .09 && amplitude < .11); // Silence cannot pass the oracle.
    return 10 * std::log10(error / samples.size() / (amplitude * amplitude / 2));
}
void estimatedOutputBoundaryControls(const std::shared_ptr<Counts>& hosts) {
    std::array<std::array<float, 256>, 2> pcm{};
    float* planes[]{pcm[0].data(), pcm[1].data()};
    singz::AudioHostRenderBlock block{};
    block.output = planes;
    block.outputChannels = 2;
    block.frames = block.maximumFrames = 256;
    block.sampleRate = 48000;
    block.clockDomain = block.routeGeneration = block.streamGeneration = 1;
    block.outputTimestampValid = true;
    const auto origin = steadyNs() - 300000000;
    const auto render = [&] {
        return hosts->render(hosts->renderContext, block);
    };
    const auto equalsRawHardware = [&] {
        block.outputTimestampHardware = false;
        assert(render());
        const auto projected = pcm;
        block.outputTimestampHardware = true;
        assert(render());
        for (size_t c = 0; c < 2; ++c)
            for (size_t frame = 0; frame < 256; ++frame)
                assert(std::abs(projected[c][frame] - pcm[c][frame]) < 1e-6f);
    };
    for (unsigned boundary = 0; boundary < 7; ++boundary) {
        block.clockDomain = block.routeGeneration = block.streamGeneration = 1;
        block.outputTimestampHardware = false;
        block.discontinuity = singz::AudioHostDiscontinuityStart;
        block.outputFrame = 1000000 + boundary * 1024;
        block.outputHostTimeNs = origin + sampleNs(boundary * 1024);
        assert(render());
        block.discontinuity = singz::AudioHostDiscontinuityNone;
        block.outputFrame += 256;
        block.outputHostTimeNs += sampleNs(256) + 90000;
        assert(render());
        const auto previousNs = block.outputHostTimeNs;
        block.outputFrame += 256;
        block.outputHostTimeNs += sampleNs(256) + 400000;
        if (boundary == 0)
            ++block.clockDomain;
        if (boundary == 1)
            ++block.streamGeneration;
        if (boundary == 2)
            ++block.routeGeneration;
        if (boundary == 3)
            block.discontinuity = singz::AudioHostDiscontinuityClockReanchored;
        if (boundary == 4)
            block.outputFrame += 32;
        if (boundary == 5)
            block.outputHostTimeNs = previousNs - 40000;
        if (boundary == 6)
            block.outputHostTimeNs = previousNs + sampleNs(256) + 3000000;
        equalsRawHardware(); // An explicit boundary must reset, not slew across it.
        // Returning from hardware to estimated quality starts a fresh model.
        block.discontinuity = singz::AudioHostDiscontinuityNone;
        block.outputFrame += 256;
        block.outputHostTimeNs += sampleNs(256) + 400000;
        equalsRawHardware();
    }
    for (unsigned malformed = 0; malformed < 5; ++malformed) {
        block.sampleRate = 48000;
        block.frames = block.maximumFrames = 256;
        block.outputFrame = 2000000;
        const auto validNs = steadyNs() - 100000000;
        block.outputHostTimeNs = validNs;
        block.outputTimestampHardware = false;
        if (malformed == 0)
            block.outputHostTimeNs = UINT64_MAX - 1;
        if (malformed == 1)
            block.outputFrame = UINT64_MAX - 127;
        if (malformed == 2)
            block.sampleRate = std::numeric_limits<double>::quiet_NaN();
        if (malformed == 3)
            block.frames = 257;
        if (malformed == 4)
            block.frames = 0;
        assert(!render());
        block.sampleRate = 48000;
        block.frames = 256;
        // For the oversized 257-frame request, match its forged continuation
        // so a counter-gap reset cannot conceal accidental estimator mutation.
        block.outputFrame = malformed == 3 ? 2000257 : 2000256;
        block.outputHostTimeNs = validNs + sampleNs(malformed == 3 ? 257 : 256) + 400000;
        equalsRawHardware(); // Rejected input cannot poison the next valid model.
    }
}
void estimatedOutputDoesNotModulateSines() {
    for (const auto& [ppm, hardware] :
         {std::pair{200, false}, std::pair{-200, false}, std::pair{0, true}}) {
        auto hosts = std::make_shared<Counts>();
        runtime::AudioRuntime engine([hosts] {
            return std::make_unique<Host>(hosts);
        });
        auto document = route();
        document.devices[0].bindings[0] = {"input", SourceKind::Application, "native:sine", false};
        const uint64_t origin = steadyNs();
        engine.bindNativeSource("native:sine", std::make_shared<SineClockSource>(origin));
        engine.commit(engine.prepare(document));
        const double clockRate = 48000 * (1 + ppm / 1e6);
        const auto started = std::chrono::steady_clock::now();
        constexpr unsigned warmup = 192, measured = 192;
        std::array<std::vector<float>, 2> samples;
        for (auto& lane : samples)
            lane.reserve(measured * 256);
        for (unsigned index = 0; index < warmup + measured; ++index) {
            const auto elapsed = static_cast<uint64_t>(index * 256 * 1e9L / clockRate);
            std::this_thread::sleep_until(started + std::chrono::nanoseconds(elapsed));
            // ALSA status+delay observations can move by USB packet phase even
            // when the device frame counter is perfectly continuous.
            constexpr std::array<int64_t, 6> packetPhase{90000, -70000, 40000, -90000, 30000, 0};
            const int64_t jitter = ppm < 0       ? packetPhase[index % packetPhase.size()]
                                   : (index & 1) ? 31250
                                                 : -31250;
            const uint64_t presentation = origin + elapsed + sampleNs(1024) + jitter;
            std::array<std::array<float, 256>, 2> pcm{};
            float* planes[]{pcm[0].data(), pcm[1].data()};
            singz::AudioHostRenderBlock block{};
            block.output = planes;
            block.outputChannels = 2;
            block.frames = block.maximumFrames = 256;
            block.sampleRate = 48000;
            block.clockDomain = 1;
            block.routeGeneration = 1;
            block.streamGeneration = 1;
            block.outputFrame = uint64_t(index) * 256;
            block.callbackSequence = index + 1;
            block.callbackHostTimeNs = origin + elapsed;
            block.outputHostTimeNs = presentation;
            block.outputTimestampValid = true;
            block.outputTimestampHardware = hardware;
            block.discontinuity =
                index ? singz::AudioHostDiscontinuityNone : singz::AudioHostDiscontinuityStart;
            assert(hosts->render && hosts->render(hosts->renderContext, block));
            if (index >= warmup)
                for (size_t c = 0; c < 2; ++c)
                    samples[c].insert(samples[c].end(), pcm[c].begin(), pcm[c].end());
        }
        for (size_t c = 0; c < 2; ++c) {
            const auto residual = sineResidual(samples[c], c ? 1499 : 997, clockRate);
            std::printf("Output clock: ppm %d hardware %d channel %zu residual %.2f dBc\n", ppm,
                        hardware, c, residual);
            if (hardware)
                assert(residual > -25); // Raw certified anchors remain authoritative.
            else
                assert(residual < -55); // Old raw estimated anchors produce roughly -14/-10dBc.
        }
        if (ppm == 200 && !hardware)
            estimatedOutputBoundaryControls(hosts);
        assert(engine.status().outputs.size() == 1);
    }
}
void captureSamplesAndBoundaries() {
    auto hosts = std::make_shared<Counts>(), inputs = std::make_shared<Counts>();
    runtime::AudioRuntime engine(
        [hosts] {
            return std::make_unique<Host>(hosts);
        },
        [inputs] {
            return std::make_unique<Input>(inputs);
        });
    auto document = route();
    document.devices[0].routing.sources[0].channels = 10;
    document.devices[0].routing.sources[0].connections = {{8, 0, 1}, {9, 1, 1}};
    engine.commit(engine.prepare(document));
    CaptureFeed feed(inputs, 10);
    awaitSignal(hosts, .45f, .5f, [&] {
        feed.run();
    });
    auto status = engine.status();
    assert(status.restored && status.inputs[0].sourceSequenceAvailable);
    assert(status.inputs[0].captureMetadataValid && !status.inputs[0].sourceCounterHardware);
    assert(status.devices[0].sources[0].meter.channels == 10);
    assert(std::abs(status.devices[0].sources[0].meter.peak[8] - .45f) < .00001f);
    auto resets = status.inputs[0].captureBoundaries;
    // A 32-frame transport omission is below the legacy 2ms timestamp gap
    // threshold. Explicit sequence/frame provenance must still reset history.
    auto gap = feed.last;
    gap.frames = 32;
    gap.firstNs += sampleNs(256 + 32);
    gap.capture.sampleHostTimeNs = gap.firstNs;
    gap.capture.sequence += 2;
    gap.capture.sourceFrame += 256 + 32;
    assert(inputs->push(inputs->context, gap));
    status = engine.status();
    assert(status.inputs[0].captureBoundaries == resets + 1);
    assert(status.inputs[0].sourceSequenceAvailable);
    // ALSA recovered overruns rotate the received-frame segment. No physical
    // lost-frame duration is inferred from this software frame counter.
    auto recovered = gap;
    recovered.xruns = 2;
    recovered.firstNs = 0;
    recovered.quality = singz::AudioInputTimestampQuality::Unknown;
    ++recovered.capture.streamGeneration;
    ++recovered.capture.sequence;
    recovered.capture.sourceFrame += gap.frames;
    recovered.capture.sampleHostTimeNs = 0;
    recovered.capture.timestampQuality = recovered.quality;
    recovered.capture.discontinuity = singz::AudioInputDiscontinuityReason::ClockReanchored;
    recovered.capture.flags = singz::AudioInputSourceFrameValid | singz::AudioInputDiscontinuous;
    assert(inputs->push(inputs->context, recovered));
    status = engine.status();
    assert(status.inputs[0].observedXruns == 2 && !status.inputs[0].captureHistory.online);
    assert(!status.restored && status.inputs[0].captureBoundaries == resets + 2);
    recovered.firstNs = gap.firstNs + sampleNs(64);
    recovered.quality = singz::AudioInputTimestampQuality::CallbackEstimate;
    ++recovered.capture.sequence;
    recovered.capture.sourceFrame += gap.frames;
    recovered.capture.sampleHostTimeNs = recovered.firstNs;
    recovered.capture.timestampQuality = recovered.quality;
    recovered.capture.discontinuity = singz::AudioInputDiscontinuityReason::None;
    recovered.capture.flags = singz::AudioInputSourceFrameValid |
                              singz::AudioInputSampleHostTimeValid |
                              singz::AudioInputTimestampQualityValid;
    assert(inputs->push(inputs->context, recovered));
    assert(engine.status().inputs[0].captureHistory.online);
    // Legacy providers expose no provenance; default-zero metadata must never
    // be mistaken for a valid counter or generation.
    recovered.firstNs += sampleNs(32);
    recovered.capture = {};
    assert(inputs->push(inputs->context, recovered));
    status = engine.status();
    assert(!status.inputs[0].captureMetadataValid && !status.inputs[0].sourceSequenceAvailable);
    assert(!status.inputs[0].sourceCounterHardware);
}
void estimatedClockAndOutputBudget() {
    // Control fixture: already-published continuous PCM with estimated USB
    // packet-phase jitter creates closed invalid clock intervals. Extra delay
    // cannot repair those intervals after publication.
    AudioHistory raw(1, AudioHistory::kDefaultCapacityFrames, ResamplingFilter::LinearPhase, true);
    std::array<float, 256> samples{};
    samples.fill(.25f);
    const float* plane = samples.data();
    constexpr uint64_t origin = 10000000000;
    for (uint64_t index = 0; index < 80; ++index) {
        const auto timestamp = origin + sampleNs(index * 256) + ((index & 1) ? 120000 : -120000);
        assert(raw.append({{&plane, 1}, 256}, 256, timestamp, 48000, double(index * 256), true));
    }
    std::array<float, 256> read{};
    float* destination = read.data();
    AudioReadCursor cursor;
    raw.read(origin + sampleNs(20 * 256), 48000, {{&destination, 1}, 256}, 256, cursor);
    assert(raw.readDiagnostics()
               .missingFrames[static_cast<size_t>(AudioHistoryMissingReason::ClockMapping)] > 0);

    auto hosts = std::make_shared<Counts>(), inputs = std::make_shared<Counts>();
    hosts->outputPresentationLeadFrames = hosts->outputBufferFrames;
    runtime::AudioRuntime engine(
        [hosts] {
            return std::make_unique<Host>(hosts);
        },
        [inputs] {
            return std::make_unique<Input>(inputs);
        });
    auto document = route();
    document.devices[0].routing.sources[0].channels = 10;
    document.devices[0].routing.sources[0].connections = {{8, 0, 1}, {9, 1, 1}};
    engine.commit(engine.prepare(document));
    CaptureFeed feed(inputs, 10);
    feed.jitterNs = 120000;
    feed.clockRate = 48008;
    awaitSignal(hosts, .45f, .5f, [&] {
        feed.run();
    });
    auto baseline = engine.status();
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (std::chrono::steady_clock::now() < until) {
        feed.run();
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    awaitSignal(hosts, .45f, .5f, [&] {
        feed.run();
    });
    auto after = engine.status();
    const auto mapping = static_cast<size_t>(AudioHistoryMissingReason::ClockMapping);
    assert(after.inputs[0].captureReads.missingFrames[mapping] ==
           baseline.inputs[0].captureReads.missingFrames[mapping]);
    assert(after.inputs[0].captureHistoryProjected &&
           after.inputs[0].captureMaximumRawPhaseStepNs >= 200000);
    assert(!after.inputs[0].sourceCounterHardware &&
           after.inputs[0].captureMetadata.sampleHostTimeNs == feed.last.firstNs);
    const auto& budget = after.monitors[0];
    assert(budget.historyBudgetKnown && budget.queuedOutputFrames == 1024 &&
           budget.outputPeriodFrames == 256);
    assert(budget.filterFutureFrames >= 96 && budget.schedulerReserveFrames == 256);
    assert(budget.historyDelayFrames == 1792);
    // A reconnect may preserve nominal48k while changing the actual queue.
    hosts->outputBufferFrames = hosts->outputPresentationLeadFrames = 2048;
    hosts->lost = true;
    engine.poll();
    after = engine.status();
    assert(hosts->opened == 2 && after.monitors[0].queuedOutputFrames == 2048);
    assert(after.monitors[0].historyDelayFrames == 2816 && after.monitors[0].historyBudgetKnown);
    awaitSignal(hosts, .45f, .5f, [&] {
        feed.run();
    });
}
void bootstrapPeers() {
    auto hosts = std::make_shared<Counts>(), inputs = std::make_shared<Counts>();
    hosts->failedUid = "missing-output";
    inputs->failedUid = "missing-input";
    runtime::AudioRuntime engine(
        [hosts] {
            return std::make_unique<Host>(hosts);
        },
        [inputs] {
            return std::make_unique<Input>(inputs);
        });
    auto document = route();
    auto missing = document.devices[0].routing.sources[0];
    missing.id = "missing";
    document.devices[0].routing.sources.push_back(missing);
    document.devices[0].bindings.push_back(
        {"missing", SourceKind::InputDevice, "missing-input", false});
    missing.id = "song";
    document.devices[0].routing.sources.push_back(missing);
    document.devices[0].bindings.push_back(
        {"song", SourceKind::Application, "native:absent", false});
    auto monitor = document.devices[0].monitors[0];
    monitor.id = "unplugged";
    monitor.deviceSelector = "missing-output";
    document.devices[0].monitors.push_back(monitor);
    engine.commit(engine.prepare(document));
    auto status = engine.status();
    assert(status.active && !status.restored && status.inputs.size() == 2 &&
           status.outputs.size() == 2);
    assert(std::count_if(status.inputs.begin(), status.inputs.end(), [](const auto& input) {
               return input.connected;
           }) == 1);
    assert(std::count_if(status.outputs.begin(), status.outputs.end(), [](const auto& output) {
               return output.connected;
           }) == 1);
    CaptureFeed feed(inputs, 2);
    awaitSignal(hosts, .05f, .1f, [&] {
        feed.run();
    });
    status = engine.status();
    assert(status.devices[0].sources[0].status.online &&
           !status.devices[0].sources[2].status.online);
    assert(!status.restored && status.inputs[0].id == "capture" && status.inputs[0].connected);
}
RouteDocument physicalSources(uint32_t sources, uint32_t channels, bool enabled = true) {
    auto document = route();
    auto& device = document.devices[0];
    device.routing.sources.clear();
    device.bindings.clear();
    for (uint32_t index = 0; index < sources; ++index) {
        const auto id = "source-" + std::to_string(index);
        SourceRouting source;
        source.id = id;
        source.channels = channels;
        source.enabled = enabled;
        device.routing.sources.push_back(source);
        device.bindings.push_back(
            {id, SourceKind::InputDevice, "capture-" + std::to_string(index), false});
    }
    return document;
}
void aggregateBudgets() {
    auto hosts = std::make_shared<Counts>(), inputs = std::make_shared<Counts>();
    runtime::AudioRuntime engine(
        [hosts] {
            return std::make_unique<Host>(hosts);
        },
        [inputs] {
            return std::make_unique<Input>(inputs);
        });
    const auto rejectedBeforeOpen = [&](const RouteDocument& document) {
        rejects([&] {
            engine.prepare(document);
        });
        assert(hosts->opened == 0 && inputs->opened == 0);
    };
    rejectedBeforeOpen(physicalSources(9, 2));
    rejectedBeforeOpen(physicalSources(3, 64));
    rejectedBeforeOpen(physicalSources(9, 64, false));
    auto outputs = route();
    outputs.devices[0].monitors.clear();
    for (uint32_t index = 0; index < 9; ++index) {
        auto monitor = route().devices[0].monitors[0];
        monitor.id = monitor.deviceSelector = "output-" + std::to_string(index);
        outputs.devices[0].monitors.push_back(monitor);
    }
    rejectedBeforeOpen(outputs);
    outputs.devices[0].monitors.resize(3);
    for (auto& monitor : outputs.devices[0].monitors) {
        monitor.channels = 64;
        monitor.connections.clear();
    }
    rejectedBeforeOpen(outputs);
    RouteDocument buses;
    for (uint32_t index = 0; index < 3; ++index) {
        VirtualDeviceDefinition bus;
        bus.id = bus.name = "bus-" + std::to_string(index);
        bus.routing.outputChannels = 64;
        buses.devices.push_back(bus);
    }
    rejectedBeforeOpen(buses);
    auto dense = physicalSources(2, 64);
    dense.devices[0].routing.outputChannels = 64;
    dense.devices[0].monitors[0].enabled = false;
    dense.devices[0].monitors[0].connections = {{0, 0, 1}};
    for (auto& source : dense.devices[0].routing.sources)
        for (uint32_t input = 0; input < 64; ++input)
            for (uint32_t output = 0; output < 64; ++output)
                source.connections.push_back({input, output, 1});
    rejectedBeforeOpen(dense); // Disabled monitor wiring still consumes storage.

    // All aggregate boundaries together, using shared physical owners rather
    // than multiplying histories by the number of logical source references.
    RouteDocument maximum;
    for (uint32_t index = 0; index < 8; ++index) {
        VirtualDeviceDefinition bus;
        bus.id = bus.name = "maximum-" + std::to_string(index);
        bus.routing.outputChannels = 16;
        for (uint32_t sourceIndex = 0; sourceIndex < 4; ++sourceIndex) {
            const auto id = "source-" + std::to_string(sourceIndex);
            SourceRouting source;
            source.id = id;
            source.channels = 16;
            for (uint32_t input = 0; input < 16; ++input)
                for (uint32_t output = 0; output < 16; ++output)
                    source.connections.push_back({input, output, 1});
            bus.routing.sources.push_back(source);
            bus.bindings.push_back({id, SourceKind::InputDevice,
                                    "capture-" + std::to_string((index + sourceIndex) % 8), false});
        }
        MonitorDefinition monitor;
        monitor.id = "monitor";
        monitor.deviceSelector = "output-" + std::to_string(index);
        monitor.channels = 16;
        bus.monitors.push_back(monitor);
        maximum.devices.push_back(bus);
    }
    auto prepared = engine.prepare(maximum);
    assert(hosts->opened == 8 && inputs->opened == 8);
    prepared.reset();
    assert(hosts->live == 0 && inputs->live == 0);
    RouteDocument stereo;
    for (uint32_t index = 0; index < 32; ++index) {
        VirtualDeviceDefinition bus;
        bus.id = bus.name = "stereo-" + std::to_string(index);
        stereo.devices.push_back(bus);
    }
    prepared = engine.prepare(stereo);
    prepared.reset();
    assert(hosts->opened == 8 && inputs->opened == 8);
}
void retainedProfilesAndOwners() {
    auto hosts = std::make_shared<Counts>(), inputs = std::make_shared<Counts>();
    runtime::AudioRuntime engine(
        [hosts] {
            return std::make_unique<Host>(hosts);
        },
        [inputs] {
            return std::make_unique<Input>(inputs);
        });
    auto document = physicalSources(1, 64);
    engine.commit(engine.prepare(document));
    document.devices[0].routing.sources[0].channels = 1;
    auto second = document.devices[0].routing.sources[0];
    second.id = "second";
    second.channels = 64;
    document.devices[0].routing.sources.push_back(second);
    document.devices[0].bindings.push_back(
        {"second", SourceKind::InputDevice, "capture-second", false});
    engine.commit(engine.prepare(document));
    document.devices[0].routing.sources[1].channels = 1;
    second.id = "third";
    document.devices[0].routing.sources.push_back(second);
    document.devices[0].bindings.push_back(
        {"third", SourceKind::InputDevice, "capture-third", false});
    const auto opened = inputs->opened;
    rejects([&] {
        engine.prepare(document);
    });
    assert(inputs->opened == opened); // Wider retained profiles count too.
}
void rapidOwnerRetirement() {
    auto hosts = std::make_shared<Counts>(), inputs = std::make_shared<Counts>();
    runtime::AudioRuntime engine(
        [hosts] {
            return std::make_unique<Host>(hosts);
        },
        [inputs] {
            return std::make_unique<Input>(inputs);
        });
    auto current = route();
    engine.commit(engine.prepare(current));
    for (uint32_t index = 0; index < 40; ++index) {
        current.devices[0].bindings[0].selector = "serial-capture-" + std::to_string(index);
        current.devices[0].monitors[0].deviceSelector = "serial-output-" + std::to_string(index);
        engine.commit(engine.prepare(current));
        assert(inputs->live <= 3 && hosts->live <= 2);
    }
    assert(hosts->maximumLive <= 2);
    engine.commit(engine.prepare({}));
    auto cleanup = engine.prepare({});
    cleanup.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    cleanup = engine.prepare({});
    cleanup.reset();
    assert(inputs->live == 0 && hosts->live == 0);
}
void boundedMeterPreview() {
    auto hosts = std::make_shared<Counts>(), inputs = std::make_shared<Counts>();
    runtime::AudioRuntime engine(
        [hosts] {
            return std::make_unique<Host>(hosts);
        },
        [inputs] {
            return std::make_unique<Input>(inputs);
        });
    // Observe one complete snapshot satisfying the asynchronous condition.
    // Renew only while simulating a visible reader waiting for its first meter.
    const auto awaitState = [&](auto condition, bool renewPreview = false) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        runtime::RuntimeStatus observed;
        do {
            if (renewPreview)
                engine.requestTelemetryPreview();
            observed = engine.status();
            if (condition(observed))
                return observed;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::steady_clock::now() < deadline);
        assert(false && "Runtime did not publish the expected preview state before its deadline");
        return observed;
    };
    auto document = route();
    document.devices[0].bindings[0] = {"input", SourceKind::Application, "native:song", false};
    document.devices[0].monitors.clear();
    engine.bindNativeSource("native:song", std::make_shared<Native>());
    engine.commit(engine.prepare(document));
    awaitState([](const auto& state) {
        return state.renderedFrames > 0 && !state.devices[0].active;
    });
    assert(hosts->opened == 0);
    const auto visible = awaitState(
        [](const auto& state) {
            const auto& device = state.devices[0];
            return device.active && device.meter.frames != 0 && device.sources[0].status.online &&
                   state.restored;
        },
        true);
    assert(visible.devices[0].active && visible.devices[0].meter.frames != 0 && hosts->opened == 0);
    // Stop renewing. A newly rendered idle state proves expiry, rather than
    // reading a second snapshot that may have expired after the first one.
    awaitState([&](const auto& state) {
        return state.renderedFrames > visible.renderedFrames && !state.devices[0].active &&
               state.devices[0].meter.frames == 0;
    });
    // Permanent monitor demand must remain active through new render blocks
    // after the same preview TTL has elapsed.
    document.devices[0].monitors = route().devices[0].monitors;
    engine.commit(engine.prepare(document));
    engine.requestTelemetryPreview();
    const auto previewElapsed = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
    awaitSignal(hosts, .125f, .25f);
    awaitState([&](const auto&) {
        return std::chrono::steady_clock::now() >= previewElapsed;
    });
    const auto checkpoint = engine.status().renderedFrames;
    awaitState([&](const auto& state) {
        return state.renderedFrames > checkpoint && state.devices[0].active &&
               state.devices[0].meter.frames != 0 && state.restored;
    });
    // A missing source cannot briefly appear restored before the first render.
    document.devices[0].bindings[0].selector = "native:missing";
    engine.commit(engine.prepare(document));
    assert(!engine.status().restored);
    const auto missingCheckpoint = engine.status().renderedFrames;
    awaitState([&](const auto& state) {
        return state.renderedFrames > missingCheckpoint && state.devices[0].active &&
               !state.devices[0].sources[0].status.online && !state.restored;
    });
}
void routingSamples() {
    auto hosts = std::make_shared<Counts>();
    auto inputs = std::make_shared<Counts>();
    runtime::AudioRuntime engine(
        [hosts] {
            return std::make_unique<Host>(hosts);
        },
        [inputs] {
            return std::make_unique<Input>(inputs);
        });
    auto source = std::make_shared<Native>();
    engine.bindNativeSource("native:song", source);
    auto document = route();
    auto& device = document.devices[0];
    device.bindings[0] = {"input", SourceKind::Application, "native:song", false};
    device.routing.sources[0].connections = {{0, 1, 1}, {1, 0, 1}};
    device.routing.sources[0].gain = 0.5f;
    device.routing.gain = 0.5f;
    device.monitors[0].gain = 0.5f;
    engine.commit(engine.prepare(document));
    engine.poll();
    awaitSignal(hosts, 0.03125f, 0.015625f);
    assert(inputs->opened == 0);
    auto telemetry = engine.status();
    assert(telemetry.devices[0].sources[0].meter.peak[0] == 0.125f);
    assert(telemetry.devices[0].sources[0].meter.peak[1] == 0.25f);
    assert(telemetry.monitors[0].meter.frames != 0);
    device.routing.sources[0].muted = true;
    engine.commit(engine.prepare(document));
    engine.poll();
    awaitSignal(hosts, 0, 0);
    device.routing.sources[0].muted = false;
    device.monitors[0].connections = {{0, 1, 1}, {1, 0, 1}};
    engine.commit(engine.prepare(document));
    engine.poll();
    awaitSignal(hosts, 0.015625f, 0.03125f);
    device.monitors[0].muted = true;
    engine.commit(engine.prepare(document));
    engine.poll();
    awaitSignal(hosts, 0, 0);
    assert(hosts->opened == 1);
    // Unregistering does not destroy sources still retained by published scenes.
    std::weak_ptr<Native> retained = source;
    source.reset();
    assert(engine.unbindNativeSource("native:song"));
    assert(!engine.unbindNativeSource("native:song") && !retained.expired());
    // An unavailable registered song becomes offline without blocking the graph.
    auto offline = engine.prepare(document);
    offline.reset();
    engine.commit(engine.prepare({}));
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    engine.poll();
    assert(retained.expired());
    for (size_t index = 0; index < runtime::kMaximumNativeSources; ++index)
        engine.bindNativeSource("native:" + std::to_string(index), std::make_shared<Native>());
    rejects([&] {
        engine.bindNativeSource("native:overflow", std::make_shared<Native>());
    });
    engine.bindNativeSource("native:0", std::make_shared<Native>());
    assert(engine.unbindNativeSource("native:0"));
    engine.bindNativeSource("native:overflow", std::make_shared<Native>());
}
} // namespace
int main() {
    {
        auto hosts = std::make_shared<Counts>(), inputs = std::make_shared<Counts>();
        runtime::AudioRuntime engine(
            [hosts] {
                return std::make_unique<Host>(hosts);
            },
            [inputs] {
                return std::make_unique<Input>(inputs);
            });
        auto document = route();
        document.devices[0].bindings[0].selector = "default";
        engine.commit(engine.prepare(document));
        assert(inputs->lastInputUid == "physical-input");
    }

    captureSamplesAndBoundaries();
    estimatedClockAndOutputBudget();
    estimatedOutputDoesNotModulateSines();
    bootstrapPeers();
    aggregateBudgets();
    retainedProfilesAndOwners();
    rapidOwnerRetirement();
    boundedMeterPreview();
    routingSamples();
    auto hosts = std::make_shared<Counts>();
    auto inputs = std::make_shared<Counts>();
    runtime::AudioRuntime runtime(
        [hosts] {
            return std::make_unique<Host>(hosts);
        },
        [inputs] {
            return std::make_unique<Input>(inputs);
        });
    assert(hosts->opened == 0 && inputs->opened == 0);
    auto document = route();
    runtime.commit(runtime.prepare(document));
    runtime.poll();
    assert(!runtime.status().restored); // Connected is not yet clock-admitted.

    assert(hosts->opened == 1 && inputs->opened == 1);
    // Capture clock facts and pre-gain native meters come from real callback
    // observations, while unavailable sequence metadata remains explicit.
    std::array<float, 256> samples;
    samples.fill(0.25f);
    const float* planes[] = {samples.data(), samples.data()};
    const auto timestamp =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count());
    singz::AudioInputBusBlock capture{planes,
                                      2,
                                      256,
                                      256,
                                      timestamp,
                                      timestamp,
                                      singz::AudioInputTimestampQuality::CallbackEstimate};
    assert(inputs->push(inputs->context, capture));
    awaitRestored(runtime);
    auto observation = runtime.status().inputs[0];
    assert(observation.captureTelemetry && observation.captureHistory.callbacks == 1);
    assert(observation.timestampQuality == singz::AudioInputTimestampQuality::CallbackEstimate);
    assert(!observation.sourceSequenceAvailable);
    capture.firstNs += 5333333;
    capture.quality = singz::AudioInputTimestampQuality::Hardware;
    assert(inputs->push(inputs->context, capture));
    observation = runtime.status().inputs[0];
    assert(observation.timestampQuality == singz::AudioInputTimestampQuality::Hardware);
    capture.quality = singz::AudioInputTimestampQuality::Unknown;
    assert(inputs->push(inputs->context, capture));
    observation = runtime.status().inputs[0];
    assert(!observation.captureHistory.online && !runtime.status().restored);
    capture.quality = singz::AudioInputTimestampQuality::Hardware;
    capture.firstNs += 5333333;
    assert(inputs->push(inputs->context, capture));
    assert(runtime.status().inputs[0].captureHistory.online);

    const unsigned inputStops = inputs->stopped, hostStops = hosts->stopped;
    document.devices[0].routing.sources[0].gain = 0.5f;
    runtime.commit(runtime.prepare(document));
    runtime.poll();
    assert(hosts->opened == 1 && inputs->opened == 1);
    assert(inputs->stopped == inputStops && hosts->stopped == hostStops);

    // A failed replacement must retain the old route and its physical owners.
    auto failed = document;
    failed.devices[0].monitors[0].deviceSelector = "missing-output";
    hosts->fail = true;
    rejects([&] {
        runtime.prepare(failed);
    });
    hosts->fail = false;
    awaitRestored(runtime);
    assert(runtime.status().outputs[0].id == "output");
    assert(inputs->opened == 1 && inputs->stopped == inputStops);

    // A new failed capture must leave every existing route owner untouched.
    failed = document;
    failed.devices[0].bindings[0].selector = "missing-capture";
    inputs->fail = true;
    rejects([&] {
        runtime.prepare(failed);
    });
    inputs->fail = false;
    awaitRestored(runtime);
    assert(runtime.status().inputs[0].id == "capture");
    const auto openedAfterFailure = inputs->opened;
    const auto stoppedAfterFailure = inputs->stopped;

    // Same-device expansion cannot briefly steal ownership from the live route.
    auto wider = document;
    wider.devices[0].routing.sources[0].channels = 4;
    rejects([&] {
        runtime.prepare(wider);
    });
    awaitRestored(runtime);
    assert(inputs->opened == openedAfterFailure);

    // The native song source has an independent lifetime from the microphone.
    runtime.bindNativeSource("native:song", std::make_shared<Native>());
    auto song = document.devices[0].routing.sources[0];
    song.id = "song";
    document.devices[0].routing.sources.push_back(song);
    document.devices[0].bindings.push_back({"song", SourceKind::Application, "native:song", false});
    runtime.commit(runtime.prepare(document));
    runtime.poll();
    document.devices[0].routing.sources.pop_back();
    document.devices[0].bindings.pop_back();
    runtime.commit(runtime.prepare(document));
    runtime.poll();
    assert(inputs->opened == openedAfterFailure && inputs->stopped == stoppedAfterFailure);
    awaitRestored(runtime);

    // Input-off retires hardware only after the prepared replacement commits.
    document.devices[0].routing.sources[0].enabled = false;
    auto off = runtime.prepare(document);
    assert(inputs->stopped == stoppedAfterFailure);
    runtime.commit(std::move(off));
    runtime.poll();
    assert(runtime.status().inputs.empty());
    // Scene reclamation can lag one render borrower; poll again after a block.
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    runtime.poll();
    assert(inputs->stopped > stoppedAfterFailure);
}
