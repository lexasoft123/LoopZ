#include <loopz/runtime/audio_runtime.hpp>
#include <loopz/audio_history.hpp>
#include <loopz/audio_resampler.hpp>
#include <loopz/render_schedule.hpp>
#include <loopz/sinc_kernel.hpp>
#include <loopz/timeline.hpp>
#include <loopz/monitor.hpp>
#include <loopz/realtime_publication.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <thread>
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace loopz::runtime {
namespace {
constexpr uint32_t rate = 48000, blockFrames = 256;
#if defined(__linux__)
// zCore's ALSA I/O workers use SCHED_FIFO 20 and consume renderer-published
// history about one period later; SingZ's song producer uses 18.
constexpr int kRendererRealtimePriority = 19;
#endif
// A provider that stops calling back without reporting failure is treated as
// failed after this long without any delivered block (measured from open).
constexpr uint64_t kCaptureStallNs = 2000000000;
uint64_t nowNs() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}
// Called once by the renderer before its loop. Failure (typically EPERM
// without RLIMIT_RTPRIO) keeps inherited scheduling; the caller records it.
bool requestRendererRealtime() noexcept {
#if defined(__linux__)
    (void)pthread_setname_np(pthread_self(), "loopz-render");
    sched_param parameters{};
    parameters.sched_priority = kRendererRealtimePriority;
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &parameters) == 0;
#else
    return false;
#endif
}
template <typename RetainedChannels>
void validateRuntimeBudget(const RouteDocument& document, RetainedChannels retainedChannels,
                           const std::map<std::string, uint32_t>& liveCaptures = {}) {
    uint64_t busChannels = 0, sourceChannels = 0, connections = 0;
    std::map<std::string_view, uint32_t> captures, outputs;
    const auto reject = [] {
        throw std::invalid_argument("Standalone Engine aggregate resource budget exceeded");
    };
    for (const auto& device : document.devices) {
        busChannels += device.routing.outputChannels;
        for (const auto& source : device.routing.sources) {
            sourceChannels += source.channels;
            connections += source.connections.size();
        }
        for (const auto& monitor : device.monitors) {
            connections += monitor.connections.size();
            if (device.routing.enabled && monitor.enabled) {
                if (!outputs.emplace(monitor.deviceSelector, monitor.channels).second)
                    throw std::invalid_argument(
                        "A hardware output may be assigned to only one monitor bus");
            }
        }
        if (device.routing.enabled)
            for (const auto& binding : device.bindings) {
                if (binding.kind != SourceKind::InputDevice)
                    continue;
                const auto source =
                    std::find_if(device.routing.sources.begin(), device.routing.sources.end(),
                                 [&](const auto& route) {
                                     return route.id == binding.id;
                                 });
                if (source != device.routing.sources.end() && source->enabled)
                    captures[binding.selector] =
                        std::max(captures[binding.selector], source->channels);
            }
        if (busChannels > RuntimeLimits::maximumBusChannels ||
            sourceChannels > RuntimeLimits::maximumSourceChannels ||
            connections > RuntimeLimits::maximumConnections ||
            captures.size() > RuntimeLimits::maximumPhysicalCaptures ||
            outputs.size() > RuntimeLimits::maximumPhysicalOutputs)
            reject();
    }
    // Private leases and retired/prepared scenes share the same physical
    // budget. Count each live owner once, retaining its negotiated width.
    for (const auto& [uid, channels] : liveCaptures)
        captures[uid] = std::max(captures[uid], channels);
    if (captures.size() > RuntimeLimits::maximumPhysicalCaptures)
        reject();
    uint64_t captureChannels = 0, outputChannels = 0;
    for (const auto& [uid, channels] : captures) {
        (void)uid;
        captureChannels += retainedChannels(uid, channels);
    }
    for (const auto& [uid, channels] : outputs) {
        (void)uid;
        outputChannels += channels;
    }
    if (captureChannels > RuntimeLimits::maximumCaptureChannels ||
        outputChannels > RuntimeLimits::maximumOutputChannels)
        reject();
}

// Estimated ALSA availability stamps can move by sub-millisecond USB packet
// phases. Received-frame continuity permits a bounded estimated timeline,
// without declaring those stamps or counters hardware-accurate. There is no
// PCM queue: phase corrections stay below the core clock's local-rate bound.
struct EstimatedCaptureTimeline {
    struct Result {
        uint64_t ns, rawStepNs, errorNs;
        bool reset;
    };
    long double projectedNs{0};
    uint64_t previousFrame{0}, previousRawNs{0};
    uint32_t previousFrames{0};
    void clear() noexcept {
        *this = {};
    }
    Result observe(uint64_t frame, uint64_t rawNs, uint32_t frames, double hz) noexcept {
        if (!projectedNs) {
            projectedNs = rawNs;
            previousFrame = frame;
            previousRawNs = rawNs;
            previousFrames = frames;
            return {rawNs, 0, 0, false};
        }
        if (frame < previousFrame) {
            clear();
            const auto fresh = observe(frame, rawNs, frames, hz);
            return {fresh.ns, 0, 0, true};
        }
        const auto duration = static_cast<long double>(frame - previousFrame) * 1e9L / hz;
        const auto predicted = projectedNs + duration;
        const auto error = static_cast<long double>(rawNs) - predicted;
        const auto step = std::abs(static_cast<long double>(rawNs) - previousRawNs - duration);
        const bool reset = frame != previousFrame + previousFrames || rawNs <= previousRawNs ||
                           std::abs(error) > 2000000.0L;
        if (reset)
            projectedNs = rawNs;
        else {
            // A 32-block phase average rejects packet jitter. The explicit
            // 0.2% slew bound is below SampleClockHistory's 0.5% local limit.
            const auto maximum = duration * 0.002L;
            projectedNs = predicted + std::clamp(error / 32.0L, -maximum, maximum);
        }
        previousFrame = frame;
        previousRawNs = rawNs;
        previousFrames = frames;
        return {static_cast<uint64_t>(projectedNs), static_cast<uint64_t>(step),
                static_cast<uint64_t>(std::abs(error)), reset};
    }
};

// One producer; payload atomics avoid a data race even when a bounded snapshot
// cannot obtain a coherent publication. All writes remain callback-safe.
struct CaptureMetadata {
    std::atomic<uint64_t> version{0}, clock{0}, generation{0}, sequence{0}, frame{0}, sampleNs{0},
        callbackNs{0}, xruns{0}, boundaries{0};
    std::atomic<uint32_t> flags{0}, reason{0}, quality{0};
    std::atomic<bool> sourceValid{false};
    void publish(const singz::AudioInputCaptureMetadata& value, uint64_t overruns, uint64_t resets,
                 bool validSource = false) noexcept {
        const auto next = version.load(std::memory_order_relaxed) + 1;
        version.store(next, std::memory_order_seq_cst);
        sourceValid.store(validSource);
        clock.store(value.clockDomainId);
        generation.store(value.streamGeneration);
        sequence.store(value.sequence);
        frame.store(value.sourceFrame);
        sampleNs.store(value.sampleHostTimeNs);
        callbackNs.store(value.callbackHostTimeNs);
        flags.store(value.flags);
        reason.store(static_cast<uint32_t>(value.discontinuity));
        quality.store(static_cast<uint32_t>(value.timestampQuality));
        xruns.store(overruns);
        boundaries.store(resets);
        version.store(next + 1, std::memory_order_seq_cst);
    }
    void read(EndpointStatus& output) const noexcept {
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            const auto before = version.load();
            if (!before || (before & 1))
                continue;
            singz::AudioInputCaptureMetadata value;
            value.clockDomainId = clock.load();
            value.streamGeneration = generation.load();
            value.sequence = sequence.load();
            value.sourceFrame = frame.load();
            value.sampleHostTimeNs = sampleNs.load();
            value.callbackHostTimeNs = callbackNs.load();
            value.flags = flags.load();
            value.discontinuity = static_cast<singz::AudioInputDiscontinuityReason>(reason.load());
            value.timestampQuality = static_cast<singz::AudioInputTimestampQuality>(quality.load());
            const auto overruns = xruns.load(), resets = boundaries.load();
            const auto validSource = sourceValid.load();
            if (before == version.load()) {
                output.captureMetadata = value;
                output.captureMetadataValid = value.streamGeneration != 0;
                output.sourceSequenceAvailable = output.captureMetadataValid && validSource;
                output.observedXruns = overruns;
                output.captureBoundaries = resets;
                return;
            }
        }
    }
};
struct Capture final : AudioSource {
    std::string uid, error;
    uint32_t count;
    AudioRuntime::InputFactory factory;
    AudioRuntime::HostFactory hostFactory;
    std::unique_ptr<singz::AudioInputBackend> backend;
    AudioHistory history;
    AudioMeter meter;
    std::atomic<singz::AudioInputTimestampQuality> quality{
        singz::AudioInputTimestampQuality::Unknown};
    singz::AudioInputTimestampQuality previousQuality{singz::AudioInputTimestampQuality::Unknown};
    CaptureMetadata provenance;
    singz::AudioInputCaptureMetadata previousMetadata{};
    uint64_t previousXruns{0}, boundaries{0};
    uint32_t previousFrames{0};
    bool previousSourceValid{false};
    EstimatedCaptureTimeline estimatedTimeline;
    std::atomic<bool> historyProjected{false};
    std::atomic<uint64_t> maximumRawStepNs{0}, maximumProjectionErrorNs{0};
    // Every provider block, including timestamp-unknown ones, proves liveness.
    std::atomic<uint64_t> deliveries{0};
    // Control-only liveness baseline: open time or the latest observed block.
    uint64_t observedDeliveries{0}, progressNs{0};
    unsigned consecutiveStalls{0};
    double sampleRate{0};
    bool connected{false};
    Capture(std::string id, uint32_t channels, AudioRuntime::InputFactory make,
            AudioRuntime::HostFactory hosts)
        : uid(std::move(id)), count(channels), factory(std::move(make)),
          hostFactory(std::move(hosts)), history(channels, AudioHistory::kDefaultCapacityFrames,
                                                 ResamplingFilter::LinearPhase, true),
          meter(channels) {}
    ~Capture() override {
        close();
    }
    void close() noexcept {
        if (backend)
            backend->stop();
        backend.reset();
        connected = false;
        history.stop();
        meter.clear();
        previousQuality = singz::AudioInputTimestampQuality::Unknown;
        quality.store(previousQuality);
        previousMetadata = {};
        previousXruns = 0;
        previousFrames = 0;
        previousSourceValid = false;
        provenance.publish({}, 0, boundaries);
        estimatedTimeline.clear();
        historyProjected.store(false);
    }
    void open() {
        close();
        backend = factory();
        if (!backend)
            throw std::runtime_error("Capture backend is unavailable");
        std::string physical = uid;
        if (physical == "default") {
            singz::AudioHost inventory(hostFactory());
            physical = inventory.enumerate().defaultInputUid;
            if (physical.empty())
                throw std::runtime_error("No default input device is available");
        }
        singz::AudioInputBusConfig config{physical, std::vector<uint32_t>(count)};
        std::iota(config.channels.begin(), config.channels.end(), 0);
        auto opened = backend->openBus(config, push, this);
        if (opened.status != singz::AudioInputBusOpenStatus::Ready)
            throw std::runtime_error(opened.result.error.empty()
                                         ? "This provider cannot open a native capture bus"
                                         : opened.result.error);
        // Use the history's own bound so an accepted capture can go online.
        if (!std::isfinite(opened.result.sampleRate) ||
            opened.result.sampleRate < AudioHistory::kMinimumSampleRate ||
            opened.result.sampleRate > AudioHistory::kMaximumSampleRate)
            throw std::runtime_error("Capture provider returned an unsupported sample rate "
                                     "(8000-192000 Hz)");
        sampleRate = opened.result.sampleRate;
        // Blocks delivered while start() runs still count as progress, but the
        // grace period begins only once the provider has actually started.
        observedDeliveries = deliveries.load(std::memory_order_relaxed);
        auto started = backend->start();
        if (!started.ok)
            throw std::runtime_error(started.error);
        progressNs = nowNs();
        connected = true;
        error.clear();
    }
    // Control only. True when an open provider has delivered no block for the
    // stall interval since start or its previous observed delivery. A provider
    // that keeps stalling (for example, waiting on an absent external clock)
    // is given 2, 4, 8, 16, then 32 s per reopen; any delivery resets that.
    bool stalled(uint64_t now) noexcept {
        if (!connected || !backend)
            return false;
        const auto delivered = deliveries.load(std::memory_order_relaxed);
        if (delivered != observedDeliveries) {
            observedDeliveries = delivered;
            progressNs = now;
            consecutiveStalls = 0;
            return false;
        }
        const auto limit = kCaptureStallNs << std::min(consecutiveStalls, 4u);
        if (now <= progressNs || now - progressNs < limit)
            return false;
        ++consecutiveStalls;
        return true;
    }
    static bool push(void* context, const singz::AudioInputBusBlock& block) noexcept {
        auto& self = *static_cast<Capture*>(context);
        self.deliveries.fetch_add(1, std::memory_order_relaxed);
        const auto& metadata = block.capture;
        const bool available = metadata.streamGeneration != 0;
        const bool sourceValid = available &&
                                 (metadata.flags & singz::AudioInputSourceFrameValid) != 0 &&
                                 metadata.sourceFrame <= UINT64_MAX - block.frames;
        const bool previousAvailable = self.previousMetadata.streamGeneration != 0;
        const bool boundary =
            available != previousAvailable ||
            (available &&
             (metadata.streamGeneration != self.previousMetadata.streamGeneration ||
              (previousAvailable && (metadata.sequence != self.previousMetadata.sequence + 1 ||
                                     (sourceValid && self.previousSourceValid &&
                                      metadata.sourceFrame != self.previousMetadata.sourceFrame +
                                                                  self.previousFrames))) ||
              metadata.discontinuity != singz::AudioInputDiscontinuityReason::None ||
              (metadata.flags & singz::AudioInputDiscontinuous) != 0 ||
              block.xruns != self.previousXruns || sourceValid != self.previousSourceValid));
        if (boundary) {
            self.estimatedTimeline.clear();
            self.history.stop();
            ++self.boundaries;
        }
        self.previousMetadata = metadata;
        self.previousFrames = block.frames;
        self.previousSourceValid = sourceValid;
        self.previousXruns = block.xruns;
        self.provenance.publish(metadata, block.xruns, self.boundaries, sourceValid);
        if (block.quality != self.previousQuality) {
            self.history.stop();
            self.estimatedTimeline.clear();
            self.previousQuality = block.quality;
            self.quality.store(block.quality, std::memory_order_relaxed);
        }
        // Unknown timestamps cannot place PCM on the shared routing clock.
        // Keep the provider alive but retire stale history until it reports a
        // usable timestamp; a quiet source is not a provider failure.
        if (block.quality == singz::AudioInputTimestampQuality::Unknown || !block.firstNs) {
            self.history.stop();
            self.meter.clear();
            self.estimatedTimeline.clear();
            self.historyProjected.store(false);
            return true;
        }
        uint64_t historyNs = block.firstNs;
        const bool project =
            sourceValid && block.quality == singz::AudioInputTimestampQuality::CallbackEstimate;
        self.historyProjected.store(project, std::memory_order_relaxed);
        if (project) {
            const auto projected = self.estimatedTimeline.observe(
                metadata.sourceFrame, block.firstNs, block.frames, self.sampleRate);
            historyNs = projected.ns;
            if (projected.rawStepNs > self.maximumRawStepNs.load(std::memory_order_relaxed))
                self.maximumRawStepNs.store(projected.rawStepNs, std::memory_order_relaxed);
            if (projected.errorNs > self.maximumProjectionErrorNs.load(std::memory_order_relaxed))
                self.maximumProjectionErrorNs.store(projected.errorNs, std::memory_order_relaxed);
            if (projected.reset) {
                self.history.stop();
                ++self.boundaries;
                self.provenance.publish(metadata, block.xruns, self.boundaries, sourceValid);
            }
        } else
            self.estimatedTimeline.clear();
        self.meter.publish({{block.planes, block.channelCount}, block.capacityFrames},
                           block.frames);
        return self.history.append({{block.planes, block.channelCount}, block.capacityFrames},
                                   block.frames, historyNs, self.sampleRate,
                                   static_cast<double>(metadata.sourceFrame), sourceValid);
    }
    uint32_t channels() const noexcept override {
        return count;
    }
    bool readMeter(AudioMeterSnapshot& output) const noexcept override {
        return meter.read(output);
    }
    AudioReadResult read(uint64_t ns, double hz, RoutingOutput out, uint32_t frames,
                         AudioReadCursor& cursor) const noexcept override {
        return history.read(ns, hz, out, frames, cursor);
    }
    std::optional<AudioReadResult>
    readChannels(uint64_t ns, double hz, RoutingOutput out, uint32_t frames,
                 AudioReadCursor& cursor, std::span<const uint32_t> channels,
                 std::optional<ResamplingFilter> filter) const noexcept override {
        return history.readChannels(ns, hz, out, frames, cursor, channels, filter);
    }
};
EndpointStatus captureStatus(const std::string& uid, const Capture& capture) {
    EndpointStatus endpoint;
    endpoint.id = uid;
    endpoint.error = capture.error;
    endpoint.connected = capture.connected;
    endpoint.captureTelemetry = true;
    endpoint.captureHistory = capture.history.status();
    endpoint.captureReads = capture.history.readDiagnostics();
    endpoint.captureHistoryProjected = capture.historyProjected.load(std::memory_order_relaxed);
    endpoint.captureMaximumRawPhaseStepNs =
        capture.maximumRawStepNs.load(std::memory_order_relaxed);
    endpoint.captureMaximumProjectionErrorNs =
        capture.maximumProjectionErrorNs.load(std::memory_order_relaxed);
    endpoint.timestampQuality = capture.quality.load(std::memory_order_relaxed);
    capture.provenance.read(endpoint);
    endpoint.audio.format.sampleRate = capture.sampleRate;
    endpoint.audio.format.inputChannels = capture.count;
    endpoint.audio.callbacks = endpoint.captureHistory.callbacks;
    endpoint.audio.discontinuities = endpoint.captureHistory.discontinuities;
    return endpoint;
}
class BusOnly final : public AudioSink {
    const uint32_t count_;

  public:
    explicit BusOnly(uint32_t count) : count_(count) {}
    uint32_t channels() const noexcept override {
        return count_;
    }
    bool requested() const noexcept override {
        return false;
    }
    bool publish(uint64_t, uint32_t, RoutingInput, uint32_t) noexcept override {
        return true;
    }
    void idle() noexcept override {}
};
struct MonitorBudget {
    bool known{false};
    uint32_t total{0}, queued{0}, period{0}, future{0}, reserve{blockFrames};
};
MonitorBudget monitorBudget(const singz::AudioHostStatus& status) {
    if (status.format.sampleRate != 0 &&
        (!std::isfinite(status.format.sampleRate) || status.format.sampleRate != rate))
        throw std::runtime_error("Output profile has an unsupported monitor clock");
    const auto period = status.diagnostics.outputPeriodFrames
                            ? status.diagnostics.outputPeriodFrames
                            : status.format.nominalBufferFrames;
    const auto queue = status.diagnostics.outputBufferFrames ? status.diagnostics.outputBufferFrames
                                                             : status.latency.bufferFrames;
    if (!period || !queue || status.format.sampleRate <= 0) {
        if (status.state == singz::AudioHostState::Running ||
            status.state == singz::AudioHostState::Open)
            throw std::runtime_error(
                "Output provider has not reported its negotiated queue budget");
        return {};
    }
    MonitorBudget budget;
    budget.known = true;
    const auto ratio = double(rate) / status.format.sampleRate;
    budget.queued = static_cast<uint32_t>(std::ceil(queue * ratio));
    budget.period = static_cast<uint32_t>(std::ceil(period * ratio));
    const auto cutoff = SincKernel::kCutoff * std::min(1.0, status.format.sampleRate / rate);
    budget.future = static_cast<uint32_t>(std::ceil(SincKernel::kRadius / cutoff));
    const uint64_t required =
        uint64_t(budget.queued) + budget.period + budget.future + budget.reserve;
    if (required > AudioHistory::kDefaultCapacityFrames / 2)
        throw std::runtime_error("Negotiated output queue exceeds the monitor history budget");
    budget.total = static_cast<uint32_t>((required + blockFrames - 1) / blockFrames * blockFrames);
    return budget;
}
struct OutputProgram {
    const VirtualDeviceDefinition device;
    const MonitorDefinition monitor;
    const std::shared_ptr<const AudioSource> source;
    const MonitorBudget budget;
    MonitorProgram program;
    OutputProgram(VirtualDeviceDefinition d, MonitorDefinition m,
                  std::shared_ptr<const AudioSource> s, const singz::AudioHostStatus& status)
        : device(std::move(d)), monitor(std::move(m)), source(std::move(s)),
          budget(monitorBudget(status)),
          program(device, monitor, source,
                  MonitorTiming{rate,
                                status.format.sampleRate > 0 ? status.format.sampleRate : rate,
                                budget.total}) {}
};
// Estimated output timestamps are observations of a continuous device frame
// counter, not commands to jump the source position every callback. Integrate
// each block with its chosen rate; steer only the next block's spacing toward
// the observed clock. Hardware-certified timestamps bypass this estimator.
struct EstimatedOutputTimeline {
    struct Result {
        uint64_t ns;
        double hz;
    };
    long double projectedNs{0};
    double previousHz{0}, nominalHz{0};
    long double basePeriodNs{0};
    uint64_t acquisitionFrames{0};
    uint64_t previousFrame{0}, previousRawNs{0};
    uint32_t previousFrames{0};
    void clear() noexcept {
        *this = {};
    }
    Result observe(uint64_t frame, uint64_t rawNs, uint32_t frames, double hz,
                   bool discontinuity) noexcept {
        if (!rawNs || rawNs >= UINT64_MAX - 10000000000ULL || !frames ||
            frames > singz::kAudioHostMaxFrames || frame > UINT64_MAX - frames ||
            !std::isfinite(hz) || hz < AudioHistory::kMinimumSampleRate ||
            hz > AudioHistory::kMaximumSampleRate) {
            clear();
            return {rawNs, 0};
        }
        const bool continuous = projectedNs && !discontinuity && nominalHz == hz &&
                                previousFrame <= UINT64_MAX - previousFrames &&
                                frame == previousFrame + previousFrames && rawNs > previousRawNs;
        long double start = rawNs;
        double selectedHz = hz;
        bool reset = !continuous;
        if (continuous) {
            start = projectedNs + static_cast<long double>(previousFrames) * 1e9L / previousHz;
            const auto error = static_cast<long double>(rawNs) - start;
            reset = !std::isfinite(start) || start <= 0 ||
                    start >= static_cast<long double>(UINT64_MAX - 10000000000ULL) ||
                    std::abs(error) > 2000000.0L;
            if (!reset) {
                // Acquire device drift quickly after an explicit boundary,
                // then reject timestamp-observer jitter with a slow alpha-beta
                // clock servo. Only sample spacing changes, never block phase.
                const auto coefficient =
                    acquisitionFrames < static_cast<uint64_t>(hz) ? 1.0L / 16 : 1.0L / 8192;
                const auto alpha = std::min(.25L, coefficient * frames / 256);
                const auto beta = alpha * alpha / 4;
                const auto minimum = 1e9L / (hz * 1.005);
                const auto maximum = 1e9L / (hz * .995);
                basePeriodNs = std::clamp(basePeriodNs + beta * error / frames, minimum, maximum);
                const auto period =
                    std::clamp(basePeriodNs + alpha * error / frames, minimum, maximum);
                selectedHz = std::clamp(static_cast<double>(1e9L / period), hz * .995, hz * 1.005);
            }
        }
        if (reset) {
            start = rawNs;
            basePeriodNs = 1e9L / hz;
            acquisitionFrames = 0;
        }
        if (acquisitionFrames < static_cast<uint64_t>(hz))
            acquisitionFrames += frames;
        projectedNs = start;
        previousHz = selectedHz;
        nominalHz = hz;
        previousFrame = frame;
        previousRawNs = rawNs;
        previousFrames = frames;
        return {static_cast<uint64_t>(start), selectedHz};
    }
};
struct Output {
    std::string uid, error;
    singz::AudioHost host;
    singz::AudioHostConfig config;
    RealtimePublication<OutputProgram> programs;
    EstimatedOutputTimeline estimatedTimeline;
    uint64_t previousClock{0}, previousStream{0}, previousRoute{0};
    explicit Output(std::string id, std::unique_ptr<singz::AudioHostBackend> backend)
        : uid(std::move(id)), host(std::move(backend)) {}
    ~Output() {
        host.stop();
    }
    static bool render(void* context, const singz::AudioHostRenderBlock& block) noexcept {
        auto& self = *static_cast<Output*>(context);
        auto program = self.programs.borrow();
        if (!program) {
            for (uint32_t c = 0; c < block.outputChannels; ++c)
                std::fill_n(block.output[c], block.frames, 0.f);
            return true;
        }
        uint64_t ns =
            block.outputTimestampValid ? block.outputHostTimeNs : block.callbackHostTimeNs;
        double measuredRate = 0;
        if (block.outputTimestampValid && !block.outputTimestampHardware && ns && block.frames &&
            block.frames <= block.maximumFrames && block.outputChannels > 0 &&
            block.outputChannels <= kMaximumRoutingChannels &&
            block.sampleRate == program->program.timing().outputRate) {
            const bool boundary = block.discontinuity != singz::AudioHostDiscontinuityNone ||
                                  block.clockDomain != self.previousClock ||
                                  block.streamGeneration != self.previousStream ||
                                  block.routeGeneration != self.previousRoute;
            const auto projected = self.estimatedTimeline.observe(
                block.outputFrame, ns, block.frames, block.sampleRate, boundary);
            ns = projected.ns;
            measuredRate = projected.hz;
        } else {
            self.estimatedTimeline.clear();
        }
        self.previousClock = block.clockDomain;
        self.previousStream = block.streamGeneration;
        self.previousRoute = block.routeGeneration;
        return program->program
            .render(ns, block.sampleRate,
                    {{block.output, block.outputChannels}, block.maximumFrames}, block.frames,
                    block.outputFrame, measuredRate)
            .validBlock;
    }
    void open() {
        auto result = host.open(config, render, this);
        if (!result.ok)
            throw std::runtime_error(result.message);
        if (result.format.sampleRate != rate) {
            host.stop();
            throw std::runtime_error(
                "Output provider did not preserve the requested 48000 Hz profile");
        }
        // open() has drained the previous worker; reset callback-owned clock
        // state only now, before start() admits the new callback owner.
        estimatedTimeline.clear();
        previousClock = previousStream = previousRoute = 0;
        // Reopening may negotiate a different queue even when the nominal
        // rate/map is unchanged. Rebuild on control before callbacks resume.
        auto previous = programs.borrow();
        if (previous) {
            auto replacement = std::make_unique<OutputProgram>(previous->device, previous->monitor,
                                                               previous->source, host.status());
            auto prepared = programs.stage(std::move(replacement));
            prepared->commit();
        }
        result = host.start();
        if (!result.ok)
            throw std::runtime_error(result.message);
        error.clear();
    }
};
} // namespace

struct AudioRuntime::Prepared {
    RouteDocument document;
    std::map<std::string, std::shared_ptr<Capture>> inputs;
    std::map<std::string, std::shared_ptr<Output>> outputs;
    std::map<std::string, std::pair<std::string, std::string>> outputLabels;
    bool committed{false};
    const void* owner{nullptr};
    std::vector<std::unique_ptr<RealtimePublication<OutputProgram>::Update>> monitorUpdates;
    std::unique_ptr<RealtimePublication<AudioScene>::Update> sceneUpdate;
    std::shared_ptr<Prepared> retired;
};
struct AudioRuntime::Impl {
    HostFactory hostFactory;
    InputFactory inputFactory;
    RealtimePublication<AudioScene> scenes;
    MonitorBuses buses{rate};
    std::shared_ptr<SceneHistoryStore> histories = std::make_shared<SceneHistoryStore>();
    std::map<std::string, std::shared_ptr<const AudioSource>> native;
    std::shared_ptr<Prepared> active;
    std::map<std::string, std::weak_ptr<Capture>> captures;
    std::map<uint64_t, std::shared_ptr<Capture>> subscriptions;
    uint64_t nextSubscription{1};
    void collectCaptures() {
        std::erase_if(captures, [](const auto& entry) {
            return entry.second.expired();
        });
    }
    std::shared_ptr<Capture> findCapture(const std::string& uid) const {
        const auto found = captures.find(uid);
        return found == captures.end() ? nullptr : found->second.lock();
    }
    std::map<std::string, uint32_t> liveCaptureChannels() const {
        std::map<std::string, uint32_t> result;
        for (const auto& [uid, owner] : captures)
            if (auto capture = owner.lock())
                result.emplace(uid, capture->count);
        return result;
    }
    std::atomic<bool> stopping{false};
    std::atomic<uint64_t> rendered{0}, failures{0}, deadlines{0}, previewUntilNs{0}, lateFrames{0},
        skippedFrames{0}, reanchors{0};
    std::atomic<bool> rendererRealtime{false};
    std::thread renderer;
    uint64_t lastRetry{0};
    Impl(HostFactory host, InputFactory input)
        : hostFactory(std::move(host)), inputFactory(std::move(input)), renderer([this] {
              run();
          }) {}
    ~Impl() {
        stopping.store(true);
        renderer.join();
        active.reset();
    }
    void run() noexcept {
        rendererRealtime.store(requestRendererRealtime(), std::memory_order_relaxed);
        RenderSchedule schedule({rate, blockFrames, blockFrames, 4096});
        while (!stopping.load()) {
            const auto scheduled = schedule.step(nowNs());
            {
                // Release the borrow before sleeping so control can reclaim
                // a retired scene without waiting for the next wake.
                auto scene = scenes.borrow();
                if (scene && scheduled.valid) {
                    lateFrames.fetch_add(scheduled.lateFrames, std::memory_order_relaxed);
                    skippedFrames.fetch_add(scheduled.skippedFrames, std::memory_order_relaxed);
                    reanchors.fetch_add(scheduled.reanchored, std::memory_order_relaxed);
                    deadlines.fetch_add((scheduled.skippedFrames + blockFrames - 1) / blockFrames,
                                        std::memory_order_relaxed);
                    for (uint32_t index = 0; index < scheduled.blocks; ++index) {
                        const auto frame = scheduled.firstFrame + uint64_t(index) * blockFrames;
                        const auto preview =
                            nowNs() < previewUntilNs.load(std::memory_order_relaxed) ? UINT32_MAX
                                                                                     : 0u;
                        const auto result = scene->render(frame, blockFrames, preview);
                        rendered.fetch_add(blockFrames, std::memory_order_relaxed);
                        failures.fetch_add(result.failedGraphs + result.failedSinks,
                                           std::memory_order_relaxed);
                        // Publication deadline is the tagged interval's end. A
                        // late wake still catches up contiguously; an actually
                        // late publication remains counted, never hidden.
                        if (nowNs() > frameNs(frame + blockFrames, rate))
                            deadlines.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            if (scheduled.valid && scheduled.nextWakeNs > nowNs())
                std::this_thread::sleep_until(std::chrono::steady_clock::time_point(
                    std::chrono::nanoseconds(scheduled.nextWakeNs)));
        }
    }
};
AudioRuntime::AudioRuntime(HostFactory host, InputFactory input)
    : impl_(std::make_unique<Impl>(std::move(host), std::move(input))) {}
AudioRuntime::~AudioRuntime() = default;
singz::AudioHostInventory AudioRuntime::inventory() const {
    singz::AudioHost host(impl_->hostFactory());
    auto result = host.enumerate();
    // An exclusive owner can make ALSA's transient capability probe report
    // zero channels (EBUSY). Preserve only an already-present explicit UID's
    // connected, successfully opened prefix; never resurrect missing devices.
    for (auto& device : result.devices) {
        const auto capture = impl_->findCapture(device.uid);
        if (capture && capture->uid != "default" && capture->connected)
            device.inputChannels = std::max(device.inputChannels, capture->count);
    }
    return result;
}
InputSubscription AudioRuntime::acquireInput(std::string deviceUid, uint32_t minimumChannels) {
    if (deviceUid.empty() || deviceUid == "default" || deviceUid.size() > 1024 ||
        deviceUid.find('\0') != std::string::npos || minimumChannels == 0 ||
        minimumChannels > singz::kAudioInputBusMaxChannels)
        throw std::invalid_argument(
            "Private input requires an explicit UID and valid channel capacity");
    impl_->collectCaptures();
    if (impl_->findCapture("default"))
        throw std::invalid_argument("Disable the public default input before private subscription");
    if (impl_->subscriptions.size() >= RuntimeLimits::maximumInputSubscriptions ||
        impl_->nextSubscription == std::numeric_limits<uint64_t>::max())
        throw std::invalid_argument("Private input subscription budget exceeded");
    auto capture = impl_->findCapture(deviceUid);
    if (capture && capture->count < minimumChannels)
        throw std::runtime_error(
            "Input channel expansion requires all public/private input owners off");
    auto live = impl_->liveCaptureChannels();
    live[deviceUid] = capture ? capture->count : minimumChannels;
    validateRuntimeBudget(
        RouteDocument{},
        [](std::string_view, uint32_t channels) {
            return channels;
        },
        live);
    if (!capture) {
        capture = std::make_shared<Capture>(deviceUid, minimumChannels, impl_->inputFactory,
                                            impl_->hostFactory);
        try {
            capture->open();
        } catch (const std::exception& failure) {
            capture->close();
            capture->error = failure.what();
        }
        impl_->captures[deviceUid] = capture;
    }
    InputSubscription result{impl_->nextSubscription, std::move(deviceUid), capture->count,
                             capture.get()};
    impl_->subscriptions.emplace(result.id, std::move(capture));
    ++impl_->nextSubscription;
    return result;
}
bool AudioRuntime::releaseInput(uint64_t id) {
    const auto found = impl_->subscriptions.find(id);
    if (found == impl_->subscriptions.end())
        return false;
    // Caller's readers are already joined. Public/retired scenes may retain
    // the capture; otherwise destruction closes/drains here on control.
    impl_->subscriptions.erase(found);
    impl_->collectCaptures();
    return true;
}
std::optional<EndpointStatus> AudioRuntime::inputStatus(uint64_t id) const {
    const auto found = impl_->subscriptions.find(id);
    if (found == impl_->subscriptions.end())
        return std::nullopt;
    return captureStatus(found->second->uid, *found->second);
}
std::shared_ptr<AudioRuntime::Prepared> AudioRuntime::prepare(const RouteDocument& document) {
    validateRouteDocument(document);
    impl_->collectCaptures();
    validateRuntimeBudget(
        document,
        [&](std::string_view uid, uint32_t channels) {
            const auto retained = impl_->findCapture(std::string(uid));
            return retained ? std::max(channels, retained->count) : channels;
        },
        impl_->liveCaptureChannels());
    // A committed predecessor is needed for at most one control turn. Old
    // scenes pin their captures until render borrowers leave; retired output
    // destructors drain callbacks here, never inside the no-fail commit.
    impl_->scenes.collect();
    if (impl_->active)
        impl_->active->retired.reset();
    impl_->buses.collect();
    impl_->scenes.checkStageCapacity();
    auto next = std::make_shared<Prepared>();
    next->document = document;
    next->owner = impl_.get();
    std::map<std::string, uint32_t> required;
    for (const auto& device : document.devices) {
        if (device.sampleRate != rate)
            throw std::invalid_argument("Linux Engine currently uses a 48000 Hz mix clock");
        for (const auto& binding : device.bindings) {
            auto route = std::find_if(device.routing.sources.begin(), device.routing.sources.end(),
                                      [&](const auto& source) {
                                          return source.id == binding.id;
                                      });
            if (route == device.routing.sources.end() || !route->enabled || !device.routing.enabled)
                continue;
            if (binding.kind == SourceKind::InputDevice) {
                if (route->channels > singz::kAudioInputBusMaxChannels)
                    throw std::invalid_argument("Native capture bus channel limit exceeded");
                required[binding.selector] = std::max(required[binding.selector], route->channels);
            } else if (binding.kind == SourceKind::Application) {
                auto found = impl_->native.find(binding.selector);
                if (!binding.selector.starts_with("native:") ||
                    (found != impl_->native.end() && found->second->channels() < route->channels))
                    throw std::invalid_argument(
                        "Application capture requires a matching native: source layout");
            } else if (binding.kind == SourceKind::PassThrough) {
                throw std::invalid_argument("Linux pass-through capture is not implemented");
            }
        }
    }
    if (!impl_->subscriptions.empty() && required.contains("default"))
        throw std::invalid_argument(
            "Private input subscriptions require explicit public input UIDs");
    for (const auto& [uid, channels] : required) {
        auto capture = impl_->findCapture(uid);
        if (capture) {
            if (capture->count < channels)
                throw std::runtime_error(
                    "Input channel expansion requires all public/private input owners off");
        } else {
            capture =
                std::make_shared<Capture>(uid, channels, impl_->inputFactory, impl_->hostFactory);
            try {
                capture->open();
            } catch (const std::exception& failure) {
                if (impl_->active || !impl_->subscriptions.empty())
                    throw;
                capture->close();
                capture->error = failure.what();
            }
            impl_->captures[uid] = capture;
        }
        next->inputs.emplace(uid, std::move(capture));
    }
    auto scene = std::make_unique<AudioScene>(
        document, SceneTiming{rate, blockFrames, 1024, 128},
        [&](const VirtualDeviceDefinition&, const SourceBinding& binding, const SourceRouting&) {
            std::shared_ptr<const AudioSource> source;
            if (binding.kind == SourceKind::InputDevice) {
                auto found = next->inputs.find(binding.selector);
                if (found != next->inputs.end())
                    source = found->second;
            } else if (binding.kind == SourceKind::Application) {
                auto found = impl_->native.find(binding.selector);
                if (found != impl_->native.end())
                    source = found->second;
            }
            return ResolvedAudioSource{std::move(source), std::make_shared<AudioReadCursor>()};
        },
        [&](const VirtualDeviceDefinition& device) {
            return impl_->buses.decorate(device,
                                         std::make_shared<BusOnly>(device.routing.outputChannels));
        },
        impl_->histories);
    for (size_t index = 0; index < document.devices.size(); ++index) {
        const auto& device = document.devices[index];
        const auto* sink = dynamic_cast<const MonitoringSink*>(scene->sink(index));
        for (const auto& monitor : device.monitors) {
            if (!monitor.enabled || !device.routing.enabled)
                continue;
            if (next->outputs.contains(monitor.deviceSelector))
                throw std::invalid_argument(
                    "A hardware output may be assigned to only one monitor bus");
            std::shared_ptr<Output> output;
            auto old = impl_->active ? impl_->active->outputs.find(monitor.deviceSelector)
                                     : next->outputs.end();
            if (impl_->active && old != impl_->active->outputs.end()) {
                output = old->second;
                if (output->config.outputChannels.size() != monitor.channels)
                    throw std::runtime_error("Output channel expansion requires explicit "
                                             "monitor-off before reconfiguration");
            } else {
                output = std::make_shared<Output>(monitor.deviceSelector, impl_->hostFactory());
                output->config.outputDeviceUid = monitor.deviceSelector;
                output->config.outputChannels.resize(monitor.channels);
                std::iota(output->config.outputChannels.begin(),
                          output->config.outputChannels.end(), 0);
                output->config.requestedSampleRate = rate;
                output->config.requestedBufferFrames = blockFrames;
                output->config.allowSharedDeviceBufferChange = true;
                output->config.maximumFrames = singz::kAudioHostMaxFrames;
                try {
                    output->open();
                } catch (const std::exception& failure) {
                    if (impl_->active)
                        throw;
                    output->host.stop();
                    output->error = failure.what();
                }
            }
            auto program = std::make_unique<OutputProgram>(device, monitor, sink->bus()->source(),
                                                           output->host.status());
            next->monitorUpdates.push_back(output->programs.stage(std::move(program)));
            next->outputLabels.emplace(monitor.deviceSelector,
                                       std::make_pair(device.id, monitor.id));
            next->outputs.emplace(monitor.deviceSelector, std::move(output));
        }
    }
    scene->activate();
    next->sceneUpdate = impl_->scenes.stage(std::move(scene));
    return next;
}
void AudioRuntime::commit(std::shared_ptr<Prepared> prepared) noexcept {
    if (!prepared || prepared->owner != impl_.get() || prepared->committed)
        return;
    prepared->committed = true;
    for (auto& update : prepared->monitorUpdates)
        update->commit();
    prepared->sceneUpdate->commit();
    prepared->retired = std::move(impl_->active);
    impl_->active = std::move(prepared);
}
void AudioRuntime::poll() {
    impl_->scenes.collect();
    impl_->buses.collect();
    if (impl_->active)
        impl_->active->retired.reset();
    impl_->collectCaptures();
    const auto now = nowNs();
    const bool retry = now - impl_->lastRetry >= 2000000000;
    if (retry)
        impl_->lastRetry = now;
    for (auto& [uid, weak] : impl_->captures) {
        (void)uid;
        auto input = weak.lock();
        if (!input)
            continue;
        std::string error;
        if (input->backend && input->backend->takeFailure(error)) {
            input->close();
            input->error = error;
        } else if (input->stalled(now)) {
            // Same recovery as a reported failure: close here, reopen on retry.
            input->close();
            input->error = "Capture provider stopped delivering audio";
        }
        if (!input->connected && retry) {
            try {
                input->open();
            } catch (const std::exception& failure) {
                input->close();
                input->error = failure.what();
            }
        }
    }
    if (!impl_->active)
        return;
    for (auto& [uid, output] : impl_->active->outputs) {
        (void)uid;
        output->programs.collect();
        const auto state = output->host.status().state;
        if (state != singz::AudioHostState::Running && retry) {
            try {
                output->host.stop();
                output->open();
            } catch (const std::exception& failure) {
                output->host.stop();
                output->error = failure.what();
            }
        }
    }
}
RuntimeStatus AudioRuntime::status() const {
    RuntimeStatus result;
    result.active = bool(impl_->active);
    result.restored = result.active;
    result.renderedFrames = impl_->rendered.load();
    result.graphFailures = impl_->failures.load();
    result.missedDeadlines = impl_->deadlines.load();
    result.renderLateFrames = impl_->lateFrames.load();
    result.renderSkippedFrames = impl_->skippedFrames.load();
    result.renderReanchors = impl_->reanchors.load();
    result.rendererRealtime = impl_->rendererRealtime.load(std::memory_order_relaxed);
    if (!impl_->active)
        return result;
    for (const auto& [uid, input] : impl_->active->inputs) {
        result.inputs.push_back(captureStatus(uid, *input));
        result.restored &= input->connected && input->history.status().online &&
                           input->quality.load(std::memory_order_relaxed) !=
                               singz::AudioInputTimestampQuality::Unknown;
    }
    for (const auto& [uid, output] : impl_->active->outputs) {
        auto audio = output->host.status();
        const bool connected = audio.state == singz::AudioHostState::Running;
        EndpointStatus endpoint;
        endpoint.id = uid;
        endpoint.error = output->error;
        endpoint.connected = connected;
        endpoint.audio = audio;
        result.outputs.push_back(std::move(endpoint));
        auto program = output->programs.borrow();
        if (program) {
            const auto& labels = impl_->active->outputLabels.at(uid);
            MonitorTelemetry monitor;
            monitor.device = labels.first;
            monitor.monitor = labels.second;
            monitor.status = program->program.status();
            program->program.meters(monitor.meter);
            monitor.historyBudgetKnown = program->budget.known;
            monitor.historyDelayFrames = program->budget.total;
            monitor.queuedOutputFrames = program->budget.queued;
            monitor.outputPeriodFrames = program->budget.period;
            monitor.filterFutureFrames = program->budget.future;
            monitor.schedulerReserveFrames = program->budget.reserve;
            result.monitors.push_back(std::move(monitor));
        }
        result.restored &= connected;
    }
    auto scene = impl_->scenes.borrow();
    if (scene) {
        // A newly published graph has not yet reported demand/source health.
        result.restored &= scene->hasRendered();
        result.buses.resize(impl_->active->document.devices.size());
        for (size_t index = 0; index < result.buses.size(); ++index) {
            scene->deviceMeters(index, result.buses[index]);
            BusTelemetry bus;
            const auto& device = impl_->active->document.devices[index];
            bus.id = device.id;
            bus.active = scene->deviceStatus(index).active;
            bus.meter = result.buses[index];
            for (size_t sourceIndex = 0; sourceIndex < device.routing.sources.size();
                 ++sourceIndex) {
                SourceTelemetry source;
                source.id = device.routing.sources[sourceIndex].id;
                source.status = scene->sourceStatus(index, sourceIndex);
                const auto& routing = device.routing.sources[sourceIndex];
                const auto binding = std::find_if(device.bindings.begin(), device.bindings.end(),
                                                  [&](const auto& value) {
                                                      return value.id == routing.id;
                                                  });
                if (bus.active && routing.enabled && binding->kind == SourceKind::Application)
                    result.restored &= source.status.online;
                scene->sourceMeters(index, sourceIndex, source.meter);
                bus.sources.push_back(std::move(source));
            }
            result.devices.push_back(std::move(bus));
        }
    }
    return result;
}
void AudioRuntime::requestTelemetryPreview() noexcept {
    impl_->previewUntilNs.store(nowNs() + 500000000, std::memory_order_relaxed);
}
void AudioRuntime::bindNativeSource(std::string selector,
                                    std::shared_ptr<const AudioSource> source) {
    if (!selector.starts_with("native:") || selector.size() > 4096 || !source ||
        !source->channels() || source->channels() > kMaximumRoutingChannels)
        throw std::invalid_argument(
            "A native source requires a native: selector and a valid channel layout");
    if (!impl_->native.contains(selector) && impl_->native.size() >= kMaximumNativeSources)
        throw std::length_error("Native source registration limit reached");
    impl_->native.insert_or_assign(std::move(selector), std::move(source));
}
bool AudioRuntime::unbindNativeSource(std::string_view selector) {
    auto found = impl_->native.find(std::string(selector));
    if (found == impl_->native.end())
        return false;
    impl_->native.erase(found);
    return true;
}
} // namespace loopz::runtime
