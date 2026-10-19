#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#include <loopz/bridge.hpp>
#include <loopz/capture.hpp>
#include <loopz/graph.hpp>
#include <zcore/device/audio_host.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <AudioToolbox/AudioWorkInterval.h>
#include <pthread.h>
#include <thread>
#include <chrono>
#include <array>
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cmath>
#include <map>
#include <string>

namespace {
volatile sig_atomic_t stopping = 0;
volatile sig_atomic_t checkpointRequested = 0;
void stop(int) {
    stopping = 1;
}
void requestCheckpoint(int) {
    checkpointRequested = 1;
}
struct Monitor {
    loopz::SharedAudio* audio;
    double rate;
    bool muted;
    std::atomic<uint64_t> missing{0}, rendered{0}, timestampErrors{0};
    std::atomic<int64_t> driftPpb{0};
    uint64_t originNs{0}, originFrame{0}, expectedFrame{0}, streamGeneration{0};
    bool havePrevious{false};
    static bool render(void* context, const singz::AudioHostRenderBlock& block) noexcept {
        auto& self = *static_cast<Monitor*>(context);
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
            const double measured = (block.outputFrame - self.originFrame) * 1e9 /
                                    (block.outputHostTimeNs - self.originNs);
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
};
} // namespace
int main(int argc, char** argv) {
    @autoreleasepool {
        std::map<std::string, std::string> options;
        for (int i = 1; i < argc; ++i) {
            std::string key = argv[i];
            if (key == "--list" || key == "--bridge-status" || key == "--tone" ||
                key == "--mute-app" || key == "--no-pass-through" || key == "--app-mixdown")
                options[key] = "1";
            else if (i + 1 < argc && key.starts_with("--"))
                options[key] = argv[++i];
            else {
                std::fprintf(stderr, "Invalid argument: %s\n", key.c_str());
                return 2;
            }
        }
        if (options.contains("--bridge-status")) {
            std::string message;
            const auto status = loopz::inspectBridge(message);
            if (!message.empty()) {
                std::fprintf(stderr, "%s\n", message.c_str());
                return 1;
            }
            std::printf("{\"protocol\":%u,\"driverAttached\":%s,\"helperAttached\":%s}\n",
                        status.protocol, status.driverAttached ? "true" : "false",
                        status.helperAttached ? "true" : "false");
            return 0;
        }
        if (options.contains("--list")) {
            std::puts(loopz::inventoryJSON().c_str());
            return 0;
        }
        if (!options.contains("--app") && !options.contains("--mic") &&
            !options.contains("--tone")) {
            std::fprintf(stderr, "usage: LoopZ Engine --list | [--app bundle] [--mic uid|default] "
                                 "[--tone] [--seconds n] [--monitor-muted uid] [--mute-app]\n");
            return 2;
        }
        const double seconds =
            options.contains("--seconds") ? std::strtod(options["--seconds"].c_str(), nullptr) : 0;
        if (!std::isfinite(seconds) || seconds < 0 || seconds > 86400)
            return 2;
        const double micGain = options.contains("--mic-gain")
                                   ? std::strtod(options["--mic-gain"].c_str(), nullptr)
                                   : 1;
        if (!std::isfinite(micGain) || micGain < 0 || micGain > 1)
            return 2;
        const double blockOption = options.contains("--block-frames")
                                       ? std::strtod(options["--block-frames"].c_str(), nullptr)
                                       : loopz::kBlock;
        if (blockOption != 64 && blockOption != 128 && blockOption != loopz::kBlock) {
            std::fprintf(stderr, "Render blocks must be 64, 128, or 256 frames\n");
            return 2;
        }
        const auto blockFrames = static_cast<uint32_t>(blockOption);
        const bool smallBlocks = blockFrames != loopz::kBlock;
        // Advance computation while preserving source and output timestamps.
        const uint32_t renderLeadFrames = smallBlocks ? 32 : 64;
        if (smallBlocks &&
            (!options.contains("--app") || options.contains("--mic") ||
             options.contains("--monitor-muted") || options.contains("--tone") ||
             options.contains("--app-mixdown") || !options.contains("--no-pass-through"))) {
            std::fprintf(stderr, "Small render blocks require app-only device-stream capture\n");
            return 2;
        }
        const double latencyOption = options.contains("--latency-frames")
                                         ? std::strtod(options["--latency-frames"].c_str(), nullptr)
                                         : 2048;
        if (!std::isfinite(latencyOption) || latencyOption < blockFrames ||
            latencyOption > loopz::kLatency || std::fmod(latencyOption, blockFrames)) {
            std::fprintf(stderr, "Source latency must be %u..4096 frames in multiples of %u\n",
                         blockFrames, blockFrames);
            return 2;
        }
        const auto latency = static_cast<uint32_t>(latencyOption);
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        std::signal(SIGUSR1, requestCheckpoint);
        loopz::BridgeClient bridge;
        std::string error;
        if (!bridge.connect("helper", error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        auto* audio = bridge.audio();
        loopz::Capture capture;
        if (options.contains("--mic")) {
            const auto status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
            if (status == AVAuthorizationStatusNotDetermined) {
                auto semaphore = dispatch_semaphore_create(0);
                [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                         completionHandler:^(BOOL) {
                                             dispatch_semaphore_signal(semaphore);
                                         }];
                if (dispatch_semaphore_wait(semaphore,
                                            dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC))) {
                    std::fprintf(stderr, "Microphone permission request timed out\n");
                    return 1;
                }
            }
            if ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio] !=
                AVAuthorizationStatusAuthorized) {
                std::fprintf(stderr, "Microphone access is not authorized\n");
                return 1;
            }
            const auto uid = options["--mic"] == "default" ? "" : options["--mic"];
            if (!capture.startMicrophone(uid, error)) {
                std::fprintf(stderr, "%s\n", error.c_str());
                return 1;
            }
        }
        loopz::ApplicationCaptureConfig captureConfig;
        captureConfig.preferDeviceStream = !options.contains("--app-mixdown");
        captureConfig.requireDeviceStream = smallBlocks;
        captureConfig.requestedBufferFrames = smallBlocks ? 64 : 128;
        if (options.contains("--app") &&
            !capture.startApplication(options["--app"], options.contains("--mute-app"), error,
                                      captureConfig)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        double rate = audio->rate.load();
        std::unique_ptr<loopz::MixingGraph> graph;
        try {
            graph = std::make_unique<loopz::MixingGraph>(rate);
        } catch (const std::exception& failure) {
            std::fprintf(stderr, "%s\n", failure.what());
            return 1;
        }
        singz::AudioHost monitorHost;
        Monitor monitor{audio, rate, true};
        if (options.contains("--monitor-muted")) {
            const auto uid = options["--monitor-muted"];
            if (uid == loopz::kDeviceUID) {
                std::fprintf(stderr, "Monitoring to LoopZ would create a routing cycle\n");
                return 2;
            }
            auto device = monitorHost.describeOutputDevice(uid);
            if (!device || device->outputChannels < 2) {
                std::fprintf(stderr, "Stereo monitor device not found\n");
                return 1;
            }
            monitor.rate = device->nominalSampleRate;
            singz::AudioHostConfig config;
            config.outputDeviceUid = uid;
            config.outputChannels = {0, 1};
            config.requestedBufferFrames = loopz::kBlock;
            auto opened = monitorHost.open(config, Monitor::render, &monitor);
            if (!opened.ok || !monitorHost.start().ok) {
                std::fprintf(stderr, "Monitor output did not start: %s\n", opened.message.c_str());
                return 1;
            }
        }
        std::array<float, loopz::kBlock> al{}, ar{}, ml{}, mr{}, pl{}, pr{}, left{}, right{};
        const auto started = loopz::hostNowNs();
        std::atomic<uint64_t> rendered{0}, missed{0}, appMissing{0}, micMissing{0}, appNonzero{0},
            micNonzero{0};
        std::atomic<bool> renderStop{false};
        std::atomic<int> renderError{0};
        std::atomic<bool> renderReady{false};
        std::atomic<uint64_t> maxExecutionNs{0}, maxWakeNs{0};
        std::atomic<int64_t> minimumPublishSlackNs{INT64_MAX};
        std::atomic<uint64_t> missedPublishDeadlines{0};
        std::atomic<int64_t> appAheadNs{INT64_MAX}, micAheadNs{INT64_MAX};
        uint64_t nextHealthCheck = 0, nextTelemetry = started + 2000000000ULL;
        bool warmed = false;
        bool failed = false;
        auto telemetry = [&](const char* event) {
            const auto now = loopz::hostNowNs();
            const auto appClock = capture.app().clock(), micClock = capture.mic().clock();
            const double appAge =
                appClock.anchorNs && now >= appClock.anchorNs ? (now - appClock.anchorNs) / 1e6 : 0;
            const double micAge =
                micClock.anchorNs && now >= micClock.anchorNs ? (now - micClock.anchorNs) / 1e6 : 0;
            std::printf(
                "{\"event\":\"%s\",\"elapsedSeconds\":%.3f,\"renderedFrames\":%llu,\"lateFrames\":%"
                "llu,\"appCallbacks\":%llu,\"micCallbacks\":%llu,\"appMissingFrames\":%llu,"
                "\"micMissingFrames\":%llu,\"appNonzeroFrames\":%llu,\"micNonzeroFrames\":%llu,"
                "\"driverReadFrames\":%llu,\"driverMissingFrames\":%llu,\"monitorFrames\":%llu,"
                "\"monitorMissingFrames\":%llu,\"monitorTimestampErrors\":%llu,"
                "\"maxRenderMicroseconds\":%.3f,\"maxWakeLatenessMicroseconds\":%.3f,"
                "\"minimumPublishSlackMicroseconds\":%.3f,"
                "\"missedPublishDeadlines\":%llu,"
                "\"appAvailableMarginMs\":%.3f,\"micAvailableMarginMs\":%.3f,"
                "\"appDiscontinuities\":%llu,\"micDiscontinuities\":%llu,\"appMeasuredRate\":%.6f,"
                "\"micMeasuredRate\":%.6f,\"appAnchorAgeMs\":%.3f,\"micAnchorAgeMs\":%.3f,"
                "\"monitorNominalRate\":%.0f,\"monitorDriftPpm\":%.6f,\"failed\":%s}\n",
                event, (now - started) / 1e9, rendered.load(), missed.load(),
                capture.app().callbacks.load(), capture.mic().callbacks.load(), appMissing.load(),
                micMissing.load(), appNonzero.load(), micNonzero.load(), audio->reads.load(),
                audio->missing.load(), monitor.rendered.load(), monitor.missing.load(),
                monitor.timestampErrors.load(), maxExecutionNs.load() / 1000.0,
                maxWakeNs.load() / 1000.0,
                minimumPublishSlackNs.load() == INT64_MAX ? 0
                                                          : minimumPublishSlackNs.load() / 1000.0,
                missedPublishDeadlines.load(),
                appAheadNs.load() == INT64_MAX ? 0 : appAheadNs.load() / 1e6,
                micAheadNs.load() == INT64_MAX ? 0 : micAheadNs.load() / 1e6,
                capture.app().discontinuities.load(), capture.mic().discontinuities.load(),
                appClock.measuredRate, micClock.measuredRate, appAge, micAge, monitor.rate,
                monitor.driftPpb.load() / 1000.0, failed ? "true" : "false");
            std::fflush(stdout);
        };
        const bool haveApp = options.contains("--app"), haveMic = options.contains("--mic"),
                   tone = options.contains("--tone"),
                   passThrough = !options.contains("--no-pass-through");
        // This clock is independent of the physical monitor. Give its render
        // thread its own audio interval, and keep HAL queries and logging off it.
        auto workgroup =
            AudioWorkIntervalCreate("LoopZ virtual render", OS_CLOCK_MACH_ABSOLUTE_TIME, nullptr);
        if (!workgroup) {
            std::fprintf(stderr, "Audio workgroup creation failed\n");
            return 1;
        }
        std::thread renderer([&] {
            pthread_setname_np("LoopZ virtual audio");
            const auto period = loopz::nsToTicks(loopz::frameNs(blockFrames, rate));
            thread_time_constraint_policy_data_t policy{
                static_cast<uint32_t>(period),
                static_cast<uint32_t>(loopz::nsToTicks(smallBlocks ? 200000 : 500000)),
                static_cast<uint32_t>(loopz::nsToTicks(smallBlocks ? 500000 : 1000000)), true};
            if (thread_policy_set(pthread_mach_thread_np(pthread_self()),
                                  THREAD_TIME_CONSTRAINT_POLICY,
                                  reinterpret_cast<thread_policy_t>(&policy),
                                  THREAD_TIME_CONSTRAINT_POLICY_COUNT) != KERN_SUCCESS) {
                renderError.store(1);
                return;
            }
            os_workgroup_join_token_s token{};
            if (os_workgroup_join(workgroup, &token)) {
                renderError.store(2);
                return;
            }
            loopz::CaptureBuffer::ReaderState appReader, micReader;
            auto frame = loopz::hostFrame(loopz::hostNowNs(), rate) / blockFrames * blockFrames;
            renderReady.store(true);
            audio->helperOnline.store(1);
            while (!renderStop.load() && bridge.connected()) {
                const auto now = loopz::hostNowNs();
                const auto current = loopz::hostFrame(now, rate);
                const auto wakeFrame = frame > renderLeadFrames ? frame - renderLeadFrames : 0;
                const auto wakeNs = loopz::frameNs(wakeFrame, rate);
                const auto targetNs = loopz::frameNs(frame, rate);
                if (audio->rate.load() != rate) {
                    renderError.store(3);
                    break;
                }
                if (current > frame + blockFrames * 4) {
                    missed.fetch_add(current - frame);
                    renderError.store(4);
                    break;
                }
                if (now > started + 2000000000ULL)
                    maxWakeNs.store(std::max(maxWakeNs.load(), now > wakeNs ? now - wakeNs : 0));
                const auto begin = loopz::nsToTicks(std::min(now, wakeNs));
                const auto nextWake =
                    loopz::nsToTicks(loopz::frameNs(frame + blockFrames - renderLeadFrames, rate));
                // Publication is due at this block's first sample, not the next
                // wake. Bootstrap/late blocks retain a valid workgroup deadline;
                // measured publication slack still exposes any late completion.
                const auto deadline = loopz::nsToTicks(
                    targetNs > now ? targetNs : now + (smallBlocks ? 500000 : 1000000));
                if (os_workgroup_interval_start(workgroup, begin, deadline, nullptr)) {
                    renderError.store(5);
                    break;
                }
                const auto sourceFrame = frame > latency ? frame - latency : 0;
                const auto sourceTime = loopz::frameNs(sourceFrame, rate);
                if (haveApp)
                    appMissing.fetch_add(capture.app().sample(sourceTime, rate, al.data(),
                                                              ar.data(), blockFrames, &appReader));
                if (haveMic)
                    micMissing.fetch_add(capture.mic().sample(sourceTime, rate, ml.data(),
                                                              mr.data(), blockFrames, &micReader));
                if (now > started + 2000000000ULL) {
                    auto margin = [&](const loopz::CaptureBuffer::ReaderState& reader,
                                      std::atomic<int64_t>& minimum) {
                        if (!reader.ns || !reader.rate)
                            return;
                        const int64_t ahead =
                            static_cast<int64_t>(reader.ns +
                                                 loopz::frameNs(reader.frames - 1, reader.rate)) -
                            static_cast<int64_t>(sourceTime + loopz::frameNs(blockFrames, rate));
                        minimum.store(std::min(minimum.load(), ahead));
                    };
                    if (haveApp)
                        margin(appReader, appAheadNs);
                    if (haveMic)
                        margin(micReader, micAheadNs);
                }
                uint64_t nonzeroApp = 0, nonzeroMic = 0;
                for (uint32_t i = 0; i < blockFrames; ++i) {
                    if (tone) {
                        al[i] = 0.1f * std::sin(2 * M_PI * 440 * (sourceFrame + i) / rate);
                        ar[i] = 0.1f * std::sin(2 * M_PI * 880 * (sourceFrame + i) / rate);
                    }
                    ml[i] *= micGain;
                    mr[i] *= micGain;
                    if (passThrough)
                        audio->playback.read(sourceFrame + i, pl[i], pr[i]);
                    if (std::fabs(al[i]) + std::fabs(ar[i]) > 1e-6f)
                        ++nonzeroApp;
                    if (std::fabs(ml[i]) + std::fabs(mr[i]) > 1e-6f)
                        ++nonzeroMic;
                }
                appNonzero.fetch_add(nonzeroApp);
                micNonzero.fetch_add(nonzeroMic);
                const bool ok =
                    graph->render(al.data(), ar.data(), ml.data(), mr.data(), pl.data(), pr.data(),
                                  left.data(), right.data(), blockFrames, rendered.load());
                if (ok) {
                    for (uint32_t i = 0; i < blockFrames; ++i)
                        audio->capture.write(frame + i, left[i], right[i]);
                    audio->capture.newest.store(frame + blockFrames);
                    const auto publishedAt = loopz::hostNowNs();
                    audio->helperHeartbeat.store(publishedAt);
                    if (now > started + 2000000000ULL) {
                        const auto slack =
                            static_cast<int64_t>(targetNs) - static_cast<int64_t>(publishedAt);
                        minimumPublishSlackNs.store(std::min(minimumPublishSlackNs.load(), slack));
                        if (slack <= 0)
                            missedPublishDeadlines.fetch_add(1);
                    }
                    rendered.fetch_add(blockFrames);
                    frame += blockFrames;
                }
                const int finished = os_workgroup_interval_finish(workgroup, nullptr);
                if (now > started + 2000000000ULL)
                    maxExecutionNs.store(std::max(maxExecutionNs.load(), loopz::hostNowNs() - now));
                if (!ok || finished) {
                    renderError.store(6);
                    break;
                }
                // Signals can interrupt the wait. Never let an early return
                // consume source history before the planned wake time.
                while (!renderStop.load() && mach_absolute_time() < nextWake) {
                    const auto status = mach_wait_until(nextWake);
                    if (status != KERN_SUCCESS && status != KERN_ABORTED) {
                        renderError.store(7);
                        break;
                    }
                }
                if (renderError.load())
                    break;
            }
            audio->helperOnline.store(0);
            os_workgroup_leave(workgroup, &token);
        });
        const auto admissionDeadline = loopz::hostNowNs() + 1000000000ULL;
        while (!renderReady.load() && !renderError.load() && loopz::hostNowNs() < admissionDeadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!renderReady.load() || renderError.load()) {
            renderStop.store(true);
            renderer.join();
            std::fprintf(stderr, "Realtime admission failed (%d)\n", renderError.load());
            return 1;
        }
        std::printf("{\"event\":\"started\",\"rate\":%.0f,\"protocol\":%u,\"tone\":%s,"
                    "\"sourceLatencyFrames\":%u,\"renderLeadFrames\":%u,\"blockFrames\":%u,"
                    "\"realtimeRender\":true}\n",
                    rate, loopz::kProtocol, tone ? "true" : "false", latency, renderLeadFrames,
                    blockFrames);
        std::fflush(stdout);
        while (!stopping && bridge.connected()) {
            @autoreleasepool {
                const auto now = loopz::hostNowNs();
                if (checkpointRequested) {
                    checkpointRequested = 0;
                    telemetry("checkpoint");
                }
                if (now >= nextHealthCheck) {
                    if (!capture.healthy(error)) {
                        std::fprintf(stderr, "%s\n", error.c_str());
                        failed = true;
                        break;
                    }
                    if (options.contains("--monitor-muted") &&
                        monitorHost.status().state != singz::AudioHostState::Running) {
                        std::fprintf(stderr,
                                     "Monitor route stopped; restart this feasibility run\n");
                        failed = true;
                        break;
                    }
                    nextHealthCheck = now + 250000000;
                }
                if (seconds && now - started >= seconds * 1e9)
                    break;
                // Structural format changes restart this feasibility run explicitly.
                if (audio->rate.load() != rate) {
                    std::fprintf(
                        stderr, "Virtual device rate changed; restart helper for the new format\n");
                    failed = true;
                    break;
                }
                if (now >= nextTelemetry) {
                    telemetry(warmed ? "progress" : "warmup");
                    warmed = true;
                    nextTelemetry = now + 60000000000ULL;
                }
                if (renderError.load()) {
                    std::fprintf(stderr, "Realtime render failed (%d)\n", renderError.load());
                    failed = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        renderStop.store(true);
        renderer.join();
        if (renderError.load())
            failed = true;
        audio->helperOnline.store(0);
        monitorHost.stop();
        capture.stop();
        telemetry("stopped");
        return !failed && bridge.connected() && !missed.load() ? 0 : 1;
    }
}
