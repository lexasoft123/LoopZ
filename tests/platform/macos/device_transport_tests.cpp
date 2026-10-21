#include <loopz/macos/transport_pool.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <thread>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
thread_local bool trackHeap = false;
thread_local uint64_t heapEvents = 0;
} // namespace

void* operator new(std::size_t bytes) {
    if (trackHeap)
        ++heapEvents;
    if (void* memory = std::malloc(bytes ? bytes : 1))
        return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) {
    return ::operator new(bytes);
}
void operator delete(void* memory) noexcept {
    if (trackHeap && memory)
        ++heapEvents;
    std::free(memory);
}
void operator delete[](void* memory) noexcept {
    ::operator delete(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
    ::operator delete(memory);
}
void operator delete[](void* memory, std::size_t) noexcept {
    ::operator delete(memory);
}
void* operator new(std::size_t bytes, std::align_val_t alignment) {
    if (trackHeap)
        ++heapEvents;
    void* memory = nullptr;
    if (::posix_memalign(&memory, static_cast<size_t>(alignment), bytes ? bytes : 1))
        throw std::bad_alloc();
    return memory;
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    return ::operator new(bytes, alignment);
}
void operator delete(void* memory, std::align_val_t) noexcept {
    ::operator delete(memory);
}
void operator delete[](void* memory, std::align_val_t) noexcept {
    ::operator delete(memory);
}
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
    ::operator delete(memory);
}
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
    ::operator delete(memory);
}

namespace {
using namespace loopz;
using namespace loopz::macos;
constexpr uint64_t now = 1000000000;

template <typename Operation> void invalid(Operation operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}

void activate(DeviceAudioView view) {
    view.header().helperHeartbeatNs.store(now);
    view.header().driverHeartbeatNs.store(now);
    view.header().helperOnline.store(1);
    view.header().driverOnline.store(1);
}

void samplesAndBounds(uint32_t channels, uint32_t rate) {
    auto region = DeviceAudioRegion::create(channels, rate, 7);
    auto view = region->view();
    auto reader = DeviceAudioView::attach(region->data(), region->bytes());
    assert(view.channels() == channels && view.sampleRate() == rate && view.generation() == 7);
    constexpr uint32_t frames = 256;
    const uint64_t first = kDeviceAudioCapacity - 128;
    std::vector<float> capture(frames * channels), playback(capture.size()),
        guarded(capture.size() + 2, 99);
    auto output = std::span(guarded).subspan(1, capture.size());
    for (size_t index = 0; index < capture.size(); ++index) {
        capture[index] = static_cast<float>((index % 1024) + 1) / 1024;
        playback[index] = -capture[index];
    }
    capture[0] = NAN;
    capture[1] = INFINITY;
    heapEvents = 0;
    trackHeap = true;
    assert(view.writeCapture(first, capture, frames));
    assert(view.readCapture(first, output, frames, now).missingFrames == frames);
    activate(view);
    assert(view.readCapture(first, output, frames, now).missingFrames == 0);
    assert(output[0] == 0 && output[1] == 0);
    for (size_t index = 2; index < output.size(); ++index)
        assert(output[index] == capture[index]);
    assert(reader.readCapture(first, output, frames, now).missingFrames == 0);
    assert(view.writePlayback(first, playback, frames));
    assert(reader.readPlayback(first, output, frames, now).missingFrames == 0);
    assert(std::equal(output.begin(), output.end(), playback.begin()));
    assert(reader.readCapture(first, output, frames, now).missingFrames == 0);
    for (size_t index = 2; index < output.size(); ++index)
        assert(output[index] == capture[index]);
    assert(view.readCapture(first, output, frames, now + kDeviceAudioHeartbeatTimeoutNs - 1)
               .missingFrames == 0);
    assert(view.readCapture(first, output, frames, now + kDeviceAudioHeartbeatTimeoutNs)
               .missingFrames == frames);
    assert(view.readPlayback(first, output, frames, now - 1).missingFrames == frames);
    assert(view.readPlayback(first, output, frames, now - 1, kDeviceAudioConcurrentHeartbeatSkewNs)
               .missingFrames == 0);
    assert(view.readPlayback(first, output, frames, now - kDeviceAudioConcurrentHeartbeatSkewNs - 1,
                             UINT64_MAX)
               .missingFrames == frames);
    view.header().helperOnline.store(0);
    assert(view.readCapture(first, output, frames, now).missingFrames == frames);
    activate(view);
    assert(!view.writeCapture(UINT64_MAX, capture, 1));
    assert(!view.readCapture(UINT64_MAX, output, 1, now).validBlock);
    assert(std::all_of(output.begin(), output.end(), [](float value) {
        return value == 0;
    }));
    assert(!view.writePlayback(first, std::span(playback).first(1), frames));
    assert(!view.readPlayback(first, output.first(1), frames, now).validBlock);
    assert(view.writeCapture(first + kDeviceAudioCapacity, capture, frames));
    assert(view.readCapture(first, output, frames, now).missingFrames == frames);
    trackHeap = false;
    assert(heapEvents == 0);
    assert(guarded.front() == 99 && guarded.back() == 99);
}

void schemaChecks() {
    invalid([] {
        (void)DeviceAudioRegion::create(3, 48000, 1);
    });
    invalid([] {
        (void)DeviceAudioRegion::create(66, 48000, 1);
    });
    invalid([] {
        (void)DeviceAudioRegion::create(2, 96001, 1);
    });
    invalid([] {
        (void)DeviceAudioRegion::create(2, 48000, 0);
    });
    auto region = DeviceAudioRegion::create(2, 48000, 1);
    invalid([&] {
        (void)DeviceAudioView::attach(nullptr, region->bytes());
    });
    invalid([&] {
        (void)DeviceAudioView::attach(region->data(), region->bytes() - 1);
    });
    invalid([&] {
        (void)DeviceAudioView::attach(static_cast<std::byte*>(region->data()) + 1, region->bytes());
    });
    auto& header = region->header();
    ++header.protocol;
    invalid([&] {
        (void)region->view();
    });
    --header.protocol;
    header.channels = 64;
    invalid([&] {
        (void)region->view();
    });
    header.channels = 2;
    header.reserved = 1;
    invalid([&] {
        (void)region->view();
    });
    header.reserved = 0;
    assert(region->view().channels() == 2);
}

VirtualDeviceDefinition device(std::string id, uint32_t channels = 2) {
    VirtualDeviceDefinition result;
    result.id = std::move(id);
    result.name = result.id;
    result.routing.outputChannels = channels;
    result.routing.sources = {
        {"pass", channels, 1, true, false, defaultChannelConnections(channels, channels)}};
    result.bindings = {{"pass", SourceKind::PassThrough, "", false}};
    return result;
}

void latencyTransactions() {
    DeviceTransportPool pool;
    RouteDocument document{kRouteDocumentVersion, {device("a"), device("b")}};
    std::array<uint32_t, 2> latencies{1025, 129};
    pool.apply(document, latencies);
    const auto original = pool.devices();
    invalid([&] {
        (void)pool.prepare(document, std::span(latencies).first(1));
    });
    latencies[0] = kMaximumInputLatencyFrames + 1;
    invalid([&] {
        (void)pool.prepare(document, latencies);
    });
    latencies[0] = 1;
    {
        auto abandoned = pool.prepare(document, latencies);
        assert(abandoned.devices()[0].region != original[0].region);
        assert(abandoned.devices()[1].region == original[1].region);
        assert(pool.devices()[0].inputLatencyFrames == 1025);
    }
    assert(!original[0].region->header().retired.load());
    auto prepared = pool.prepare(document, latencies);
    assert(prepared.devices()[0].inputLatencyFrames == 1);
    pool.commit(std::move(prepared));
    assert(original[0].region->header().retired.load());
    assert(!original[1].region->header().retired.load());
    const auto retained = pool.devices()[0].region;
    document.devices[0].name = "Same timing";
    pool.apply(document, latencies);
    assert(pool.devices()[0].region == retained);
    document.devices[0].routing.enabled = false;
    pool.apply(document, latencies);
    assert(pool.devices().size() == 1 && pool.devices()[0].id == "b" &&
           pool.devices()[0].inputLatencyFrames == 129);
}

void deviceTransactions() {
    DeviceTransportPool pool;
    RouteDocument document{kRouteDocumentVersion, {device("a", 8), device("b", 64)}};
    pool.apply(document);
    auto a = pool.devices()[0].region;
    auto b = pool.devices()[1].region;
    const auto uid = pool.devices()[0].uid;
    activate(a->view());
    activate(b->view());
    std::array<float, 8> priorAudio{};
    priorAudio.fill(0.25f);
    assert(a->view().writeCapture(100, priorAudio, 1));
    document.devices[0].name = "Renamed";
    document.devices[0].routing.sources[0].muted = true;
    pool.apply(document);
    assert(pool.devices()[0].name == "LoopZ: Renamed" && pool.devices()[0].uid == uid);
    assert(document.devices[0].name == "Renamed");
    assert(pool.devices()[0].region == a && pool.devices()[1].region == b);
    assert(a->header().helperOnline.load() && b->header().driverOnline.load());
    document.devices[0].routing.sources[0].enabled = false;
    pool.apply(document);
    assert(!pool.devices()[0].hasOutput && pool.devices()[0].region == a);

    auto unsupported = document;
    unsupported.devices[0].sampleRate = 44100; // would replace a
    unsupported.devices[1].sampleRate = 96001; // reject after preparation of a
    invalid([&] {
        pool.apply(unsupported);
    });
    assert(pool.devices()[0].region == a && pool.devices()[1].region == b);
    assert(a->header().helperOnline.load() && b->header().driverOnline.load());

    resizeDeviceOutputs(document, "a", 16);
    pool.apply(document);
    auto replacement = pool.devices()[0].region;
    assert(replacement != a && replacement->view().channels() == 16);
    assert(replacement->view().generation() > a->view().generation());
    assert(pool.devices()[0].uid == uid && pool.devices()[1].region == b);
    assert(!a->header().helperOnline.load() && !a->header().driverOnline.load());
    activate(a->view()); // Simulate an old control task arriving after retirement.
    assert(!a->view().writeCapture(101, priorAudio, 1));
    assert(a->view().readCapture(100, priorAudio, 1, now).missingFrames == 1);
    for (float sample : priorAudio)
        assert(sample == 0);
    assert(b->header().helperOnline.load());

    document.devices[0].routing.enabled = false;
    pool.apply(document);
    assert(pool.devices().size() == 1 && pool.devices()[0].region == b);
    document.devices[0].routing.enabled = true;
    pool.apply(document);
    assert(pool.devices()[0].uid == uid && pool.devices()[0].region != replacement);
    assert(!pool.devices()[0].region->header().helperOnline.load());
    assert(pool.devices()[1].region == b);
    pool.apply({});
    assert(pool.devices().empty() && !b->header().helperOnline.load());

    // Pinning an old mapping remains safe after its owning pool is gone.
    std::shared_ptr<DeviceAudioRegion> survivor;
    {
        DeviceTransportPool temporary;
        temporary.apply({kRouteDocumentVersion, {device("survivor")}});
        survivor = temporary.devices()[0].region;
        activate(survivor->view());
    }
    assert(!survivor->header().helperOnline.load());
    assert(survivor->view().channels() == 2);

    RouteDocument maximum;
    for (uint32_t index = 0; index < kMaximumVirtualDevices; ++index)
        maximum.devices.push_back(device(std::to_string(index)));
    pool.apply(maximum);
    assert(pool.devices().size() == kMaximumVirtualDevices);
    for (size_t index = 1; index < pool.devices().size(); ++index)
        assert(pool.devices()[index].region->view().generation() >
               pool.devices()[index - 1].region->view().generation());
    auto pinned = pool.devices()[0].region;
    activate(pinned->view());
    maximum.devices.push_back(device("excess"));
    invalid([&] {
        pool.apply(maximum);
    });
    assert(pool.devices().size() == kMaximumVirtualDevices && pool.devices()[0].region == pinned &&
           pinned->header().helperOnline.load());
}

void concurrentPublication() {
    auto region = DeviceAudioRegion::create(64, 48000, 1);
    const auto view = region->view();
    activate(view);
    std::array<float, 64> input{}, output{};
    std::atomic<bool> finished{false};
    std::jthread writer([&] {
        for (uint32_t iteration = 0; iteration < 10000; ++iteration) {
            input.fill(iteration % 2 ? 0.25f : 0.5f);
            assert(view.writeCapture(0, input, 1));
        }
        finished.store(true);
    });
    do {
        const auto result = view.readCapture(0, output, 1, now);
        if (!result.missingFrames) {
            assert(output[0] == 0.25f || output[0] == 0.5f);
            for (float sample : output)
                assert(sample == output[0]);
        } else {
            for (float sample : output)
                assert(sample == 0);
        }
    } while (!finished.load());
}

void preparedTransactions() {
    DeviceTransportPool pool, other;
    RouteDocument document{kRouteDocumentVersion, {device("prepared", 8)}};
    pool.apply(document);
    auto original = pool.devices()[0].region;
    activate(original->view());
    auto changed = document;
    resizeDeviceOutputs(changed, "prepared", 16);
    uint64_t abandonedGeneration = 0;
    {
        auto abandoned = pool.prepare(changed);
        abandonedGeneration = abandoned.devices()[0].region->view().generation();
        assert(abandoned.devices()[0].region != original);
        assert(pool.devices()[0].region == original && original->header().helperOnline.load());
        assert(!original->header().retired.load());
    }
    assert(pool.devices()[0].region == original && !original->header().retired.load());
    auto accepted = pool.prepare(changed);
    assert(accepted.devices()[0].region->view().generation() > abandonedGeneration);
    auto stale = pool.prepare(document);
    invalid([&] {
        other.commit(std::move(accepted));
    });
    assert(other.devices().empty() && pool.devices()[0].region == original);
    auto replacement = accepted.devices()[0].region;
    pool.commit(std::move(accepted));
    assert(pool.devices()[0].region == replacement && original->header().retired.load());
    invalid([&] {
        pool.commit(std::move(accepted));
    });
    invalid([&] {
        pool.commit(std::move(stale));
    });
    assert(pool.devices()[0].region == replacement);
    auto removal = pool.prepare({});
    assert(!replacement->header().retired.load());
    pool.commit(std::move(removal));
    assert(pool.devices().empty() && replacement->header().retired.load());
}

template <typename Condition> bool waitFor(Condition condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

void independentProcesses() {
    auto a = DeviceAudioRegion::create(8, 48000, 1);
    auto b = DeviceAudioRegion::create(64, 44100, 2);
    activate(a->view());
    activate(b->view());
    struct Exchange {
        std::atomic<uint32_t> published{0}, acknowledged[2]{};
    };
    auto* memory =
        ::mmap(nullptr, sizeof(Exchange), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    assert(memory != MAP_FAILED);
    auto* exchange = std::construct_at(static_cast<Exchange*>(memory));
    constexpr uint32_t batches = 160, frames = 256;
    std::array<pid_t, 2> children{};
    for (uint32_t index = 0; index < 2; ++index) {
        children[index] = ::fork();
        assert(children[index] >= 0);
        if (!children[index]) {
            std::array<float, frames * 64> output{};
            const std::array views{a->view(), b->view()};
            for (uint32_t batch = 1; batch <= batches; ++batch) {
                if (!waitFor([&] {
                        return exchange->published.load() >= batch;
                    }))
                    ::_exit(2);
                for (const auto& view : views) {
                    auto buffer = std::span(output).first(frames * view.channels());
                    const auto result = view.readCapture((batch - 1) * frames, buffer, frames, now);
                    if (!result.validBlock || result.missingFrames)
                        ::_exit(3);
                    for (uint32_t frame = 0; frame < frames; ++frame)
                        for (uint32_t channel = 0; channel < view.channels(); ++channel)
                            if (buffer[frame * view.channels() + channel] !=
                                static_cast<float>(batch * 128 + channel + view.channels() * 4096))
                                ::_exit(4);
                }
                // Each playback direction has exactly one writer process.
                const auto& own = views[index];
                auto buffer = std::span(output).first(frames * own.channels());
                std::fill(buffer.begin(), buffer.end(), -static_cast<float>(batch + index));
                if (!own.writePlayback((batch - 1) * frames, buffer, frames))
                    ::_exit(5);
                exchange->acknowledged[index].store(batch);
            }
            ::_exit(0);
        }
    }
    bool passed = true;
    std::array<float, frames * 64> input{}, output{};
    const std::array views{a->view(), b->view()};
    for (uint32_t batch = 1; batch <= batches && passed; ++batch) {
        for (const auto& view : views) {
            for (uint32_t frame = 0; frame < frames; ++frame)
                for (uint32_t channel = 0; channel < view.channels(); ++channel)
                    input[frame * view.channels() + channel] =
                        static_cast<float>(batch * 128 + channel + view.channels() * 4096);
            assert(view.writeCapture((batch - 1) * frames,
                                     std::span(input).first(frames * view.channels()), frames));
        }
        exchange->published.store(batch);
        passed = waitFor([&] {
            return exchange->acknowledged[0].load() == batch &&
                   exchange->acknowledged[1].load() == batch;
        });
        if (!passed)
            break;
        for (uint32_t index = 0; index < 2; ++index) {
            const auto& view = views[index];
            auto buffer = std::span(output).first(frames * view.channels());
            const auto result = view.readPlayback((batch - 1) * frames, buffer, frames, now);
            assert(result.validBlock && !result.missingFrames);
            for (float sample : buffer)
                assert(sample == -static_cast<float>(batch + index));
        }
    }
    for (auto child : children) {
        int status = 0;
        assert(::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
               WEXITSTATUS(status) == 0);
    }
    ::munmap(memory, sizeof(Exchange));
    assert(passed);
}
} // namespace

int main() {
    schemaChecks();
    for (auto channels : {2u, 8u, 64u})
        for (auto rate : kVirtualSampleRates)
            samplesAndBounds(channels, rate);
    deviceTransactions();
    latencyTransactions();
    preparedTransactions();
    concurrentPublication();
    independentProcesses();
    std::puts(
        "device transport: multichannel IO, bounded silence, heartbeat, generation retirement, "
        "atomic pool edits and two independent reader processes passed (not installed HAL "
        "evidence)");
}
