#include "driver_bridge.h"
#include <loopz/bridge.hpp>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <algorithm>
#include <cmath>

namespace {
std::atomic<loopz::SharedAudio*> active{nullptr};
std::atomic<uint32_t> readers{0};
std::atomic<uint64_t> anchor{0}, generation{1};
std::atomic<uint32_t> clockRate{48000};
std::atomic<uint32_t> periodFrames{128}, queueFrames{1024}, sourceFrames{2048},
    maximumReadFrames{0};
std::atomic<int64_t> minimumReadFirstNs{INT64_MAX}, maximumReadLastNs{INT64_MIN};
mach_timebase_info_data_t timebase{};
std::unique_ptr<loopz::BridgeClient> client;
uint64_t toNs(uint64_t ticks) noexcept {
    return ticks / timebase.denom * timebase.numer +
           ticks % timebase.denom * timebase.numer / timebase.denom;
}
uint64_t toTicks(uint64_t ns) noexcept {
    return ns / timebase.numer * timebase.denom +
           ns % timebase.numer * timebase.denom / timebase.numer;
}
uint64_t position(double sample) noexcept {
    const long double ns =
        toNs(anchor.load()) + static_cast<long double>(sample) * 1e9L / clockRate.load();
    return loopz::hostFrame(ns > 0 ? static_cast<uint64_t>(ns) : 0, clockRate.load());
}
} // namespace
extern "C" void LoopZBridgeInitialize() {
    mach_timebase_info(&timebase);
    anchor.store(mach_absolute_time());
    auto queue = dispatch_queue_create("com.lexasoft.loopz.driver.bridge", DISPATCH_QUEUE_SERIAL);
    auto timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
    dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC, 100 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(timer, ^{
        if (client && client->connected())
            return;
        active.exchange(nullptr);
        // Pin the old mapping until every callback that could have borrowed it has
        // exited. New callbacks see null; no callback waits for the control queue.
        if (readers.load() != 0)
            return;
        client.reset();
        auto replacement = std::make_unique<loopz::BridgeClient>();
        std::string error;
        if (replacement->connect("driver", error)) {
            replacement->audio()->rate.store(clockRate.load());
            active.store(replacement->audio());
            client = std::move(replacement);
        }
    });
    dispatch_resume(timer);
}
extern "C" void LoopZClockStart(double rate) {
    clockRate.store(static_cast<uint32_t>(rate));
    anchor.store(mach_absolute_time());
    generation.fetch_add(1);
    readers.fetch_add(1);
    if (auto* audio = active.load())
        audio->rate.store(static_cast<uint32_t>(rate));
    readers.fetch_sub(1);
}
extern "C" void LoopZClockStamp(double* sample, uint64_t* host, uint64_t* seed) {
    const auto start = anchor.load();
    const auto now = mach_absolute_time();
    const auto rate = clockRate.load();
    const auto frame = loopz::hostFrame(toNs(now >= start ? now - start : 0), rate);
    const auto period = periodFrames.load();
    const auto zero = frame / period * period;
    *sample = static_cast<double>(zero);
    *host = start + toTicks(loopz::frameNs(zero, rate));
    *seed = generation.load();
}
extern "C" void LoopZRead(float* buffer, uint32_t frames, double sampleTime) {
    if (!buffer || frames > 32768)
        return;
    std::fill_n(buffer, frames * 2, 0);
    readers.fetch_add(1);
    auto* audio = active.load();
    const auto now = toNs(mach_absolute_time());
    uint64_t missing = frames;
    if (audio && audio->helperOnline.load() && now >= audio->helperHeartbeat.load() &&
        now - audio->helperHeartbeat.load() < 250000000 && audio->rate.load() == clockRate.load()) {
        const auto first = position(sampleTime);
        const auto queue = queueFrames.load();
        const int64_t firstDelta = static_cast<int64_t>(loopz::frameNs(
                                       first > queue ? first - queue : 0, clockRate.load())) -
                                   static_cast<int64_t>(now);
        const int64_t lastDelta =
            firstDelta + static_cast<int64_t>(loopz::frameNs(frames, clockRate.load()));
        // Approximate extrema for diagnostics; one bounded CAS never delays audio.
        auto low = minimumReadFirstNs.load();
        if (firstDelta < low)
            minimumReadFirstNs.compare_exchange_strong(low, firstDelta);
        auto high = maximumReadLastNs.load();
        if (lastDelta > high)
            maximumReadLastNs.compare_exchange_strong(high, lastDelta);
        auto maximum = maximumReadFrames.load();
        if (frames > maximum)
            maximumReadFrames.compare_exchange_strong(maximum, frames);
        missing = 0;
        for (uint32_t i = 0; i < frames; ++i) {
            // One fixed extra queue margin; source alignment delay is in the helper.
            const auto frame = first + i;
            if (frame < queue ||
                !audio->capture.read(frame - queue, buffer[2 * i], buffer[2 * i + 1]))
                ++missing;
        }
    }
    if (audio) {
        audio->reads.fetch_add(frames);
        audio->missing.fetch_add(missing);
        audio->driverHeartbeat.store(now);
        audio->rate.store(clockRate.load());
    }
    readers.fetch_sub(1);
}
extern "C" void LoopZWrite(const float* buffer, uint32_t frames, double sampleTime) {
    if (!buffer || frames > 32768)
        return;
    readers.fetch_add(1);
    if (auto* audio = active.load()) {
        const auto first = position(sampleTime);
        for (uint32_t i = 0; i < frames; ++i)
            audio->playback.write(first + i, buffer[2 * i], buffer[2 * i + 1]);
        audio->writes.fetch_add(frames);
        audio->driverHeartbeat.store(toNs(mach_absolute_time()));
        audio->rate.store(clockRate.load());
    }
    readers.fetch_sub(1);
}

extern "C" uint32_t LoopZClockPeriod() {
    return periodFrames.load();
}
extern "C" uint32_t LoopZInputLatency() {
    return sourceFrames.load() + queueFrames.load();
}
extern "C" int LoopZValidTiming(uint32_t period, uint32_t queue, uint32_t source) {
    return (period == 64 || period == 128 || period == 256 || period == 512 || period == 1024) &&
           queue <= 2048 && queue % 256 == 0 && source >= 64 && source <= 4096 && source % 64 == 0;
}
extern "C" void LoopZSetTiming(uint32_t period, uint32_t queue, uint32_t source) {
    periodFrames.store(period);
    queueFrames.store(queue);
    sourceFrames.store(source);
    minimumReadFirstNs.store(INT64_MAX);
    maximumReadLastNs.store(INT64_MIN);
    maximumReadFrames.store(0);
}
extern "C" CFDictionaryRef LoopZCopyTiming() {
    const void* keys[] = {CFSTR("build"),
                          CFSTR("periodFrames"),
                          CFSTR("queueFrames"),
                          CFSTR("sourceFrames"),
                          CFSTR("maximumReadFrames"),
                          CFSTR("minimumReadFirstNs"),
                          CFSTR("maximumReadLastNs")};
    int64_t values[] = {periodFrames.load(),       queueFrames.load(),
                        sourceFrames.load(),       maximumReadFrames.load(),
                        minimumReadFirstNs.load(), maximumReadLastNs.load()};
    const void* objects[7] = {CFSTR("loopz-timing-3")};
    for (unsigned i = 0; i < 6; ++i)
        objects[i + 1] = CFNumberCreate(nullptr, kCFNumberSInt64Type, &values[i]);
    auto result = CFDictionaryCreate(nullptr, keys, objects, 7, &kCFTypeDictionaryKeyCallBacks,
                                     &kCFTypeDictionaryValueCallBacks);
    for (unsigned i = 1; i < 7; ++i)
        CFRelease(objects[i]);
    return result;
}
