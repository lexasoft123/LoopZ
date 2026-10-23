#include "virtual_cable.hpp"
#include "pcm16.hpp"
#include "float32.hpp"
#include "driver_transport.hpp"
#include "playback_clock.hpp"
#include "publication_wait.hpp"
#include "publication_cadence.hpp"
#include "ingress_sequence.hpp"
#include "ingress_drain.hpp"
#include <loopz/audio_resampler.hpp>
#include <loopz/sinc_kernel.hpp>
#include <loopz/timeline.hpp>
#include <windows.h>
#include <objbase.h>
#include <avrt.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace loopz::windows {
namespace {
uint64_t clockNs() noexcept {
    LARGE_INTEGER ticks{}, frequency{};
    QueryPerformanceCounter(&ticks);
    QueryPerformanceFrequency(&frequency);
    const auto t = static_cast<uint64_t>(ticks.QuadPart);
    const auto f = static_cast<uint64_t>(frequency.QuadPart);
    return t / f * 1000000000ULL + t % f * 1000000000ULL / f;
}
MonitorDefinition publication(uint32_t channels) {
    MonitorDefinition monitor;
    monitor.id = "__loopz_public_microphone";
    monitor.channels = channels;
    monitor.enabled = true;
    for (uint32_t channel = 0; channel < channels; ++channel)
        monitor.connections.push_back({channel, channel, 1});
    return monitor;
}
} // namespace
struct VirtualCable::Impl {
    const std::string endpoint;
    const loopz_driver::AudioFormat format;
    const uint32_t renderRate;
    std::shared_ptr<AudioHistory> history;
    const std::shared_ptr<const AudioHistory> published;
    std::unique_ptr<MonitorProgram> program;
    std::jthread worker;
    mutable std::mutex mutex;
    std::condition_variable ready;
    bool initialized = false;
    std::atomic<bool> running{false};
    std::atomic<uint64_t> starts{0}, outgoingFrames{0}, outgoingAllZeroFrames{0};
    std::atomic<unsigned> negotiatedVersion{0}, negotiatedBits{0};
    std::atomic<uint32_t> threadId{0}, mmcssError{0};
    std::atomic<bool> mmcssRegistered{false};
    std::atomic<uint64_t> deadlineMisses{0}, lastDeadlineMissNs{0};
    std::atomic<uint64_t> maximumLatenessNs{0}, maximumDrainNs{0}, maximumRenderNs{0},
        maximumReadNs{0}, maximumWriteNs{0};
    std::atomic<bool> driverQueueAvailable{false};
    std::atomic<uint64_t> driverMissingFrames{0}, driverDroppedFrames{0}, driverUnderruns{0},
        driverLastUnderrunTime100ns{0}, driverRecordingFrames{0}, driverLeaseSilentFrames{0};
    std::atomic<uint32_t> driverQueuedFrames{0}, driverIngressPackets{0},
        driverLastUnderrunQueued{0}, driverLastUnderrunRequested{0};
    std::atomic<uint64_t> driverSampleTime100ns{0};
    std::atomic<uint64_t> publicationWaits{0}, publicationWaitTimeouts{0},
        maximumPublicationWaitNs{0};
    std::atomic<bool> publicationWaitEnabled{false};
    std::atomic<bool> streamingChunksEnabled{false};
    std::atomic<uint32_t> batchFrames{0};
    std::atomic<unsigned> publicationFlowVersion{0};
    std::atomic<uint64_t> maximumChunkRenderNs{0};
    std::atomic<uint64_t> ingressPackets{0}, ingressFrames{0}, packetDiscontinuities{0},
        timestampResets{0}, completedIngressZeroRuns{0}, maximumCompletedIngressZeroFrames{0};
    std::atomic<uint64_t> ingressSkippedPackets{0}, ingressSequenceResets{0};
    std::atomic<uint64_t> maximumIngressPublicationLagNs{0}, maximumIngressPublicationGapNs{0};
    static void maximum(std::atomic<uint64_t>& value, uint64_t observation) noexcept {
        // One worker writes each statistic; control readers only load it.
        value.store(std::max(value.load(std::memory_order_relaxed), observation),
                    std::memory_order_relaxed);
    }
    std::string failure;
    Impl(std::string id, const VirtualDeviceDefinition& device,
         std::shared_ptr<const AudioSource> mix, uint32_t rate)
        : endpoint(std::move(id)),
          format{static_cast<unsigned>(device.sampleRate), device.routing.outputChannels},
          renderRate(rate),
          history(std::make_shared<AudioHistory>(format.channels,
                                                 AudioHistory::kDefaultCapacityFrames,
                                                 ResamplingFilter::LinearPhase, true)),
          published(std::dynamic_pointer_cast<const AudioHistory>(mix)),
          program(std::make_unique<MonitorProgram>(
              device, publication(format.channels), std::move(mix),
              MonitorTiming{rate, device.sampleRate, rate * 2048 / 48000})) {}
    void fail(std::string message) {
        std::lock_guard lock(mutex);
        failure = std::move(message);
        running.store(false);
    }
    void run(std::stop_token stop) {
        threadId.store(GetCurrentThreadId());
        const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(apartment)) {
            fail("Private audio transport could not initialize COM");
            {
                std::lock_guard lock(mutex);
                initialized = true;
            }
            ready.notify_all();
            return;
        }
        {
            DriverTransport transport;
            std::string error;
            bool connected = transport.connect(endpoint, error);
            if (connected && (transport.format().sampleRate != format.sampleRate ||
                              transport.format().channels != format.channels)) {
                error = "Microphone format changed. Use Settings → Install or repair microphones.";
                transport.close();
                connected = false;
            }
            if (!connected)
                fail(error);
            running.store(connected);
            negotiatedVersion.store(connected ? transport.version() : 0);
            negotiatedBits.store(connected ? transport.sampleBytes() * 8 : 0);
            DWORD task{};
            HANDLE priority = connected ? AvSetMmThreadCharacteristicsW(L"Audio", &task) : nullptr;
            mmcssRegistered.store(priority != nullptr);
            mmcssError.store(priority ? 0 : connected ? GetLastError() : ERROR_NOT_READY);
            HANDLE timer = CreateWaitableTimerExW(
                nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!timer)
                timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
            wchar_t batchSetting[2]{};
            const bool smallBatches =
                GetEnvironmentVariableW(L"LOOPZ_EXPERIMENT_SMALL_BATCHES", batchSetting, 2) == 1 &&
                batchSetting[0] == L'1';
            const unsigned channels = format.channels,
                           frames = publicationBatchFrames(format.sampleRate, smallBatches);
            batchFrames.store(frames);
            const unsigned wireSampleBytes = transport.sampleBytes(),
                           wireFrameBytes = transport.frameBytes();
            const unsigned wirePacketFrames = transport.packetFrames();
            std::vector<float> incoming(channels * loopz_driver::DuplexBridge::packetFrames),
                outgoing(channels * frames);
            std::array<const float*, loopz_driver::AudioFormat::maxChannels> input{};
            std::array<float*, loopz_driver::AudioFormat::maxChannels> output{};
            for (unsigned c = 0; c < channels; ++c) {
                input[c] = incoming.data() + c * loopz_driver::DuplexBridge::packetFrames;
                output[c] = outgoing.data() + c * frames;
            }
            std::vector<unsigned char> pcm(frames * wireFrameBytes);
            auto packetStorage = std::make_unique<loopz_driver::DuplexBridge::Packet>();
            auto& packet = *packetStorage;
            PlaybackClock playbackClock(format.sampleRate);
            IngressSequence ingressSequence;
            uint64_t next = 0, frame = 0, lastInput = 0;
            uint64_t zeroFrames = 0;
            bool inputSeen = false;
            bool queryDriverQueue = true;
            uint64_t nextDriverQuery = 0;
            const auto flow = publicationFlowPolicy(transport.version(), wireSampleBytes,
                                                    bool(published), smallBatches);
            const bool waitForPublication = flow.paired, streamChunks = flow.paired;
            if (connected && flow.paired) {
                loopz_driver::DuplexBridge::Diagnostics queue{};
                queryDriverQueue = transport.diagnostics(queue);
                if (!flow.acceptsQueueStatus(queryDriverQueue))
                    fail("Microphone driver queue status is unavailable. Install or repair "
                         "microphones.");
            }
            publicationFlowVersion.store(flow.version());
            publicationWaitEnabled.store(waitForPublication);
            streamingChunksEnabled.store(streamChunks);
            {
                std::lock_guard lock(mutex);
                initialized = true;
            }
            ready.notify_all();

            // All supported nominal rates contain whole frames in 10 ms. Use
            // that common grid so same-clock publication reads integral render
            // frames instead of paying for a fractional-delay filter solely
            // because this worker happened to start between two sample times.
            next = clockNs();
            next += (10000000 - next % 10000000) % 10000000;
            const auto publicationOrigin = next;
            uint64_t publicationBatch = 0;
            const auto drainIngress = [&](uint64_t deadline = UINT64_MAX,
                                          uint32_t minimumFrames = 0) {
                const auto drainStart = clockNs();
                uint32_t receivedFrames = 0;
                // Bounded drain: let microphone publication and cancellation run
                // even if a render client keeps the ingress continuously full.
                const auto capacity = wireSampleBytes == 4
                                          ? loopz_driver::FloatDuplexBridge::packetCapacity
                                          : loopz_driver::DuplexBridge::packetCapacity;
                for (unsigned drain = 0; drain < capacity; ++drain) {
                    if (!running.load() || stop.stop_requested() ||
                        !ingressDrainMayContinue(clockNs(), deadline, receivedFrames,
                                                 minimumFrames))
                        break;
                    const auto readStart = clockNs();
                    const bool read = transport.read(packet, error);
                    maximum(maximumReadNs, clockNs() - readStart);
                    if (!read) {
                        fail(error);
                        break;
                    }
                    if (!packet.frames)
                        break;
                    receivedFrames += packet.frames;
                    const auto sequence = ingressSequence.append(packet.sequence);
                    ingressSkippedPackets.fetch_add(sequence.skippedPackets,
                                                    std::memory_order_relaxed);
                    if (sequence.reset)
                        ingressSequenceResets.fetch_add(1, std::memory_order_relaxed);
                    ingressPackets.fetch_add(1, std::memory_order_relaxed);
                    ingressFrames.fetch_add(packet.frames, std::memory_order_relaxed);
                    if (packet.discontinuity)
                        packetDiscontinuities.fetch_add(1, std::memory_order_relaxed);
                    const auto stamp =
                        playbackClock.append(packet.time, packet.frames, packet.discontinuity != 0);
                    if (!stamp.valid) {
                        fail("Invalid playback timestamp");
                        break;
                    }
                    if (stamp.discontinuity) {
                        if (!packet.discontinuity)
                            timestampResets.fetch_add(1, std::memory_order_relaxed);
                        history->stop();
                    }
                    for (unsigned i = 0; i < packet.frames; ++i) {
                        unsigned nonzero = 0;
                        if (wireSampleBytes == 4) {
                            nonzero = decodeFloat32Frame(packet.samples + i * wireFrameBytes,
                                                         channels, incoming.data() + i,
                                                         loopz_driver::DuplexBridge::packetFrames);
                        } else {
                            for (unsigned c = 0; c < channels; ++c) {
                                const auto offset = i * wireFrameBytes + c * 2;
                                const unsigned raw =
                                    static_cast<unsigned>(packet.samples[offset]) |
                                    (static_cast<unsigned>(packet.samples[offset + 1]) << 8);
                                nonzero |= raw;
                                const int sample = raw >= 32768 ? static_cast<int>(raw) - 65536
                                                                : static_cast<int>(raw);
                                incoming[c * loopz_driver::DuplexBridge::packetFrames + i] =
                                    static_cast<float>(sample) / 32768.0f;
                            }
                        }
                        if (!nonzero) {
                            if (inputSeen)
                                ++zeroFrames;
                        } else {
                            // Count only runs bounded by nonzero audio on both
                            // sides, excluding startup and final idle silence.
                            if (zeroFrames) {
                                completedIngressZeroRuns.fetch_add(1, std::memory_order_relaxed);
                                maximum(maximumCompletedIngressZeroFrames, zeroFrames);
                                zeroFrames = 0;
                            }
                            inputSeen = true;
                        }
                    }
                    if (!history->append({{input.data(), channels}, packet.frames}, packet.frames,
                                         stamp.hostNs, format.sampleRate,
                                         static_cast<double>(stamp.sourceFrame), true)) {
                        fail("Could not publish playback history");
                        break;
                    }
                    const auto publishedAt = clockNs();
                    const auto packetEnd = stamp.hostNs + frameNs(packet.frames, format.sampleRate);
                    if (publishedAt > packetEnd)
                        maximum(maximumIngressPublicationLagNs, publishedAt - packetEnd);
                    if (lastInput && publishedAt >= lastInput)
                        maximum(maximumIngressPublicationGapNs, publishedAt - lastInput);
                    lastInput = publishedAt;
                }
                maximum(maximumDrainNs, clockNs() - drainStart);
            };
            while (connected && running.load() && !stop.stop_requested()) {
                const auto queryTime = clockNs();
                if (queryDriverQueue && queryTime >= nextDriverQuery) {
                    loopz_driver::DuplexBridge::Diagnostics queue{};
                    queryDriverQueue = transport.diagnostics(queue);
                    if (queryDriverQueue) {
                        driverMissingFrames.store(queue.missingFrames);
                        driverDroppedFrames.store(queue.droppedFrames);
                        driverUnderruns.store(queue.underruns);
                        driverLastUnderrunTime100ns.store(queue.lastUnderrunTime);
                        driverRecordingFrames.store(queue.recordingFrames);
                        driverLeaseSilentFrames.store(queue.leaseSilentFrames);
                        driverSampleTime100ns.store(queue.sampleTime);
                        driverQueuedFrames.store(queue.queuedFrames);
                        driverIngressPackets.store(queue.ingressPackets);
                        driverLastUnderrunQueued.store(queue.lastUnderrunQueued);
                        driverLastUnderrunRequested.store(queue.lastUnderrunRequested);
                        driverQueueAvailable.store(true);
                    } else {
                        driverQueueAvailable.store(false);
                        if (!flow.acceptsQueueStatus(false))
                            fail("Microphone driver queue status was lost");
                    }
                    nextDriverQuery = queryTime + 500000000;
                }
                if (!running.load() || stop.stop_requested())
                    break;
                // Once output is due, publish it before draining a backlog.
                // Ingress stays queued in the driver for the next slack window
                // and for the existing inter-chunk drains below.
                drainIngress(next);
                if (!running.load())
                    break;
                const auto now = clockNs();
                if (lastInput && now - lastInput > 50000000) {
                    history->stop();
                    playbackClock.reset();
                    lastInput = 0;
                }
                if (now >= next) {
                    maximum(maximumLatenessNs, now - next);
                    if (now - next > privateWorkerLatenessLimitNs) {
                        deadlineMisses.fetch_add(1, std::memory_order_relaxed);
                        lastDeadlineMissNs.store(now - next, std::memory_order_relaxed);
                        fail("Private audio worker missed its deadline by " +
                             std::to_string((now - next) / 1000000) + " ms");
                        break;
                    }
                    // The driver's recording FIFO starts after a 20 ms prefill.
                    // Use the intended presentation time, not worker receipt time.
                    const auto waitForFrames = [&](unsigned throughFrames) {
                        if (waitForPublication && published && queryDriverQueue) {
                            const auto delay = frameNs(renderRate * 2048 / 48000, renderRate);
                            constexpr auto support =
                                static_cast<uint32_t>(SincKernel::kRadius / SincKernel::kCutoff) +
                                2;
                            const auto requiredEnd =
                                next + 20000000 > delay
                                    ? next + 20000000 - delay +
                                          frameNs(throughFrames, format.sampleRate) +
                                          frameNs(support, std::min(renderRate, format.sampleRate))
                                    : 0;
                            const auto pending = [&] {
                                if (!requiredEnd || !published->status().online)
                                    return false;
                                const auto end = published->publishedBlockEndHostNs();
                                return !end || end < requiredEnd;
                            };
                            if (pending()) {
                                loopz_driver::DuplexBridge::Diagnostics queue{};
                                const auto queried = clockNs();
                                if (transport.diagnostics(queue)) {
                                    const auto reserve =
                                        std::max<uint64_t>(
                                            2000000, streamChunks ? maximumChunkRenderNs.load()
                                                                  : maximumRenderNs.load()) +
                                        std::max<uint64_t>(1000000, maximumWriteNs.load()) +
                                        1000000;
                                    const auto deadline =
                                        publicationWaitDeadline(queried, next, queue.queuedFrames,
                                                                format.sampleRate, reserve);
                                    if (clockNs() < deadline) {
                                        publicationWaits.fetch_add(1);
                                        while (clockNs() < deadline && pending() &&
                                               running.load() && !stop.stop_requested()) {
                                            drainIngress(deadline);
                                            if (!running.load() || stop.stop_requested())
                                                break;
                                            const auto before = clockNs();
                                            if (before >= deadline)
                                                break;
                                            if (timer) {
                                                LARGE_INTEGER due{};
                                                due.QuadPart = -static_cast<LONGLONG>(
                                                    std::min<uint64_t>(250000, deadline - before) /
                                                        100 +
                                                    1);
                                                if (SetWaitableTimer(timer, &due, 0, nullptr,
                                                                     nullptr, FALSE))
                                                    (void)WaitForSingleObject(timer, 1);
                                                else
                                                    Sleep(1);
                                            } else
                                                Sleep(1);
                                        }
                                        maximum(maximumPublicationWaitNs, clockNs() - queried);
                                        if (pending())
                                            publicationWaitTimeouts.fetch_add(1);
                                    }
                                } else {
                                    driverQueueAvailable.store(false);
                                    fail("Microphone driver queue status was lost during "
                                         "publication");
                                }
                            }
                        }
                    };
                    if (!streamChunks)
                        waitForFrames(frames);
                    if (!running.load() || stop.stop_requested())
                        break;
                    const auto renderStart = clockNs();
                    // Guarantee intake for each paired output chunk, including
                    // late chunks. Otherwise continuous output catch-up can
                    // starve ingress while the worker remains actively running.
                    // Extra intake remains limited by the chunk's deadline.
                    for (unsigned offset = 0; offset < frames;) {
                        const auto count =
                            std::min(frames - offset,
                                     streamChunks ? std::min(256u, wirePacketFrames) : 256u);
                        if (streamChunks)
                            drainIngress(next + frameNs(offset, format.sampleRate), count);
                        else if (offset)
                            drainIngress();
                        if (!running.load() || stop.stop_requested())
                            break;
                        if (streamChunks)
                            waitForFrames(offset + count);
                        if (!running.load() || stop.stop_requested())
                            break;
                        const auto chunkRenderStart = clockNs();
                        for (unsigned c = 0; c < channels; ++c)
                            output[c] = streamChunks ? outgoing.data() + c * count
                                                     : outgoing.data() + c * frames + offset;
                        const auto rendered = program->render(
                            next + 20000000 + frameNs(offset, format.sampleRate), format.sampleRate,
                            {{output.data(), channels}, count}, count, frame + offset);
                        if (!rendered.validBlock) {
                            fail("Invalid microphone render block");
                            break;
                        }
                        maximum(maximumChunkRenderNs, clockNs() - chunkRenderStart);
                        if (streamChunks) {
                            const auto zeros =
                                wireSampleBytes == 4
                                    ? encodeFloat32(outgoing.data(), channels, count, pcm.data())
                                    : encodePcm16(outgoing.data(), channels, count, pcm.data());
                            const auto writeStart = clockNs();
                            const bool written = transport.write(pcm.data(), count, error);
                            maximum(maximumWriteNs, clockNs() - writeStart);
                            if (!written) {
                                fail(error);
                                break;
                            }
                            outgoingFrames.fetch_add(count, std::memory_order_relaxed);
                            outgoingAllZeroFrames.fetch_add(zeros, std::memory_order_relaxed);
                        }
                        offset += count;
                    }
                    maximum(maximumRenderNs, clockNs() - renderStart);
                    if (!running.load() || stop.stop_requested())
                        break;
                    if (!streamChunks) {
                        const auto allZeroFrames =
                            wireSampleBytes == 4
                                ? encodeFloat32(outgoing.data(), channels, frames, pcm.data())
                                : encodePcm16(outgoing.data(), channels, frames, pcm.data());
                        outgoingFrames.fetch_add(frames, std::memory_order_relaxed);
                        outgoingAllZeroFrames.fetch_add(allZeroFrames, std::memory_order_relaxed);
                        for (unsigned offset = 0; offset < frames;) {
                            const unsigned count = std::min(frames - offset, wirePacketFrames);
                            const auto writeStart = clockNs();
                            const bool written =
                                transport.write(pcm.data() + offset * wireFrameBytes, count, error);
                            maximum(maximumWriteNs, clockNs() - writeStart);
                            if (!written) {
                                fail(error);
                                break;
                            }
                            offset += count;
                        }
                    }
                    if (!running.load())
                        break;
                    frame += frames;
                    next =
                        publicationBatchTime(publicationOrigin, ++publicationBatch, smallBatches);
                }
                const auto beforeWait = clockNs();
                const auto wake = std::min(next, beforeWait + 2000000);
                if (wake > beforeWait && timer) {
                    LARGE_INTEGER due{};
                    due.QuadPart = -static_cast<LONGLONG>((wake - beforeWait) / 100 + 1);
                    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
                        WaitForSingleObject(timer, 10);
                    else
                        Sleep(1);
                } else if (!timer)
                    Sleep(1);
            }
            history->stop();
            running.store(false);
            if (timer)
                CloseHandle(timer);
            if (priority)
                AvRevertMmThreadCharacteristics(priority);
        }
        CoUninitialize();
    }
};
VirtualCable::VirtualCable(std::string endpoint, const VirtualDeviceDefinition& device,
                           std::shared_ptr<const AudioSource> mix, uint32_t renderRate)
    : impl_(std::make_unique<Impl>(std::move(endpoint), device, std::move(mix), renderRate)) {}
VirtualCable::~VirtualCable() {
    stop();
}
bool VirtualCable::start() {
    stop();
    impl_->starts.fetch_add(1, std::memory_order_relaxed);
    impl_->maximumLatenessNs.store(0);
    impl_->maximumDrainNs.store(0);
    impl_->maximumRenderNs.store(0);
    impl_->driverQueueAvailable.store(false);
    impl_->publicationWaitEnabled.store(false);
    impl_->streamingChunksEnabled.store(false);
    impl_->publicationFlowVersion.store(0);
    impl_->maximumReadNs.store(0);
    impl_->maximumWriteNs.store(0);
    impl_->threadId.store(0);
    impl_->mmcssRegistered.store(false);
    impl_->mmcssError.store(0);
    {
        std::lock_guard lock(impl_->mutex);
        impl_->initialized = false;
        impl_->failure.clear();
    }
    impl_->worker = std::jthread([this](std::stop_token stop) {
        impl_->run(stop);
    });
    std::unique_lock lock(impl_->mutex);
    const bool initialized = impl_->ready.wait_for(lock, std::chrono::seconds(10), [&] {
        return impl_->initialized;
    });
    if (!initialized) {
        lock.unlock();
        stop();
        impl_->fail("Private audio transport connection timed out");
        return false;
    }
    return impl_->running.load();
}
void VirtualCable::stop() {
    if (impl_->worker.joinable()) {
        impl_->worker.request_stop();
        impl_->worker.join();
    }
}
bool VirtualCable::matches(const VirtualDeviceDefinition& device,
                           uint32_t renderRate) const noexcept {
    return impl_->format.sampleRate == device.sampleRate &&
           impl_->format.channels == device.routing.outputChannels &&
           impl_->renderRate == renderRate;
}
bool VirtualCable::healthy() const noexcept {
    return impl_->running.load();
}
std::string VirtualCable::error() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->failure;
}
VirtualCableDiagnostics VirtualCable::diagnostics() const noexcept {
    // Called by the serialized runtime control owner, which also owns start/stop.
    // Read the worker's lifetime CPU time here, never inside an audio callback or
    // its render loop. Wall-clock maxima alone cannot identify CPU saturation.
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool cpuAvailable =
        impl_->worker.joinable() &&
        GetThreadTimes(impl_->worker.native_handle(), &created, &exited, &kernel, &user);
    const auto ticks = [](FILETIME value) {
        return (uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    };
    const auto cpuNs = cpuAvailable ? (ticks(kernel) + ticks(user)) * 100 : 0;
    return {impl_->maximumLatenessNs.load(),
            impl_->maximumDrainNs.load(),
            impl_->maximumRenderNs.load(),
            impl_->maximumReadNs.load(),
            impl_->maximumWriteNs.load(),
            impl_->deadlineMisses.load(),
            impl_->lastDeadlineMissNs.load(),
            impl_->ingressPackets.load(),
            impl_->ingressFrames.load(),
            impl_->packetDiscontinuities.load(),
            impl_->timestampResets.load(),
            impl_->completedIngressZeroRuns.load(),
            impl_->maximumCompletedIngressZeroFrames.load(),
            impl_->starts.load() ? impl_->starts.load() - 1 : 0,
            impl_->outgoingFrames.load(),
            impl_->outgoingAllZeroFrames.load(),
            impl_->driverQueueAvailable.load(),
            impl_->driverMissingFrames.load(),
            impl_->driverDroppedFrames.load(),
            impl_->driverUnderruns.load(),
            impl_->driverLastUnderrunTime100ns.load(),
            impl_->driverRecordingFrames.load(),
            impl_->driverLeaseSilentFrames.load(),
            impl_->driverSampleTime100ns.load(),
            impl_->driverQueuedFrames.load(),
            impl_->driverIngressPackets.load(),
            impl_->driverLastUnderrunQueued.load(),
            impl_->driverLastUnderrunRequested.load(),
            impl_->publicationWaits.load(),
            impl_->publicationWaitTimeouts.load(),
            impl_->maximumPublicationWaitNs.load(),
            impl_->publicationWaitEnabled.load(),
            impl_->streamingChunksEnabled.load(),
            impl_->maximumChunkRenderNs.load(),
            impl_->negotiatedVersion.load(),
            impl_->negotiatedBits.load(),
            cpuAvailable,
            impl_->mmcssRegistered.load(),
            cpuNs,
            impl_->threadId.load(),
            impl_->mmcssError.load(),
            impl_->batchFrames.load(),
            impl_->publicationFlowVersion.load(),
            impl_->ingressSkippedPackets.load(),
            impl_->ingressSequenceResets.load(),
            impl_->maximumIngressPublicationLagNs.load(),
            impl_->maximumIngressPublicationGapNs.load()};
}
const std::string& VirtualCable::endpoint() const noexcept {
    return impl_->endpoint;
}
std::shared_ptr<AudioHistory> VirtualCable::playback() const {
    return impl_->history;
}
MonitorProgram* VirtualCable::program() const noexcept {
    return impl_->program.get();
}
} // namespace loopz::windows
