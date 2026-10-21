#include <loopz/sample_clock.hpp>
#include <loopz/timeline.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <thread>

namespace {
thread_local bool trackHeap = false;
thread_local uint32_t heapEvents = 0;
} // namespace
void* operator new(size_t bytes) {
    if (trackHeap)
        ++heapEvents;
    if (auto* result = std::malloc(bytes ? bytes : 1))
        return result;
    throw std::bad_alloc();
}
void* operator new[](size_t bytes) {
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
void operator delete(void* memory, size_t) noexcept {
    ::operator delete(memory);
}
void operator delete[](void* memory, size_t) noexcept {
    ::operator delete(memory);
}

namespace {
using namespace loopz;
template <typename Operation> auto realtime(Operation operation) {
    trackHeap = true;
    heapEvents = 0;
    auto value = operation();
    trackHeap = false;
    assert(heapEvents == 0);
    return value;
}
void intervalsResetAndWrap() {
    SampleClockHistory clock(4);
    constexpr uint64_t origin = 1000000000;
    assert(!clock.locate(clock.snapshot(), origin, 0, 48000, 48000).valid);
    assert(!clock.append(0, 0));
    assert(realtime([&] {
        return clock.append(0, origin);
    }));
    const auto first = clock.snapshot();
    assert(!clock.append(0, origin + 1) && !clock.append(64, origin));
    assert(clock.snapshot().end == first.end);
    const auto tail = realtime([&] {
        return clock.locate(first, origin + frameNs(16, 48000), 0, 48000, 48000);
    });
    assert(tail.valid && tail.frame == 0 && std::abs(tail.offset - 16) < 0.00005 &&
           tail.endNs == UINT64_MAX);
    assert(!clock.locate(first, origin - 1, 0, 48000, 48000).valid);
    assert(!clock.locate(first, origin, 1, 48000, 48000).valid);
    assert(!clock.locate(first, origin, NAN, 48000, 48000).valid);
    assert(!clock.locate(first, origin, 0, 0, 48000).valid);
    assert(!clock.locate(first, origin, 0, 48000, 49000).valid);
    assert(clock.append(64, origin + frameNs(64, 48000)));
    const auto closed = realtime([&] {
        return clock.locate(clock.snapshot(), origin + frameNs(16, 48000), 0, 48000, 0);
    });
    assert(closed.valid && closed.rate == 48000 && closed.endNs == origin + frameNs(64, 48000));
    const auto boundary =
        clock.locate(clock.snapshot(), origin + frameNs(64, 48000), 0, 48000, 48000);
    assert(boundary.valid && boundary.frame == 64 && boundary.offset == 0);
    for (uint64_t block = 2; block < 6; ++block)
        assert(clock.append(block * 64, origin + frameNs(block * 64, 48000)));
    assert(!clock.locate(first, origin, 0, 48000, 48000).valid);
    const auto snapshot = clock.snapshot();
    assert(snapshot.end - snapshot.begin == 4);
    // An overwritten oldest record must not invalidate a retained interval in
    // this reader's snapshot when the producer advances once more.
    assert(clock.append(384, origin + frameNs(384, 48000)));
    const auto retained = clock.locate(snapshot, origin + frameNs(272, 48000), 0, 48000, 48000);
    assert(retained.valid && std::abs(retained.frame + retained.offset - 272) < 0.0001);
    realtime([&] {
        clock.reset();
        return true;
    });
    assert(!clock.locate(snapshot, origin + frameNs(272, 48000), 0, 48000, 48000).valid);
    assert(clock.append(0, origin));
    assert(clock.locate(clock.snapshot(), origin, 0, 48000, 48000).valid);
    assert(!clock.locate({1, UINT64_MAX - 1}, origin, 0, 48000, 48000).valid);
}
void coalescingPreservesClockBoundaries() {
    constexpr uint64_t origin = 1000000000ULL;
    for (double ppm : {0.0, 200.0}) {
        SampleClockHistory clock(16);
        const auto actual = 48000 * (1 + ppm * 1e-6);
        for (uint64_t block = 0; block < 6; ++block)
            assert(clock.append(block * 64, origin + frameNs(block * 64, actual)));
        const auto point = realtime([&] {
            return clock.locate(clock.snapshot(), origin + frameNs(16, actual), 0, 48000, 48000,
                                origin + frameNs(272, actual));
        });
        assert(point.valid && point.endNs == origin + frameNs(320, actual));
        assert(std::abs(point.rate / actual - 1) < 0.2e-6);
    }
    SampleClockHistory changed(16);
    for (uint64_t block = 0; block < 6; ++block)
        assert(changed.append(block * 64, origin + frameNs(block * 64, 48000)));
    assert(changed.append(384, origin + frameNs(384, 48000) - 500));
    const auto boundary = changed.locate(changed.snapshot(), origin + frameNs(272, 48000), 0, 48000,
                                         48000, origin + frameNs(400, 48000));
    assert(boundary.valid && boundary.endNs == origin + frameNs(320, 48000));
    SampleClockHistory noisy(16);
    for (uint64_t block = 0; block < 6; ++block)
        assert(
            noisy.append(block * 64, origin + frameNs(block * 64, 48000) + (block == 1 ? 10 : 0)));
    const auto retained = noisy.locate(noisy.snapshot(), origin + frameNs(16, 48000), 0, 48000,
                                       48000, origin + frameNs(272, 48000));
    assert(retained.valid && retained.endNs == origin + frameNs(64, 48000) + 10);
}
void precisionAndInvalidIntervals() {
    SampleClockHistory clock(8);
    constexpr uint64_t origin = 4000000000000000000ULL, firstFrame = (uint64_t{1} << 55) + 1234;
    assert(clock.append(firstFrame, origin));
    assert(clock.append(firstFrame + 1920, origin + 10000000));
    const auto point = clock.locate(clock.snapshot(), origin + 5000000, 0.5L, 192000, 192000);
    assert(point.valid && point.frame == firstFrame &&
           std::abs(point.offset - 960.000096L) < 1e-8L);
    clock.reset();
    assert(clock.append(0, origin));
    assert(clock.append(480, origin + 9000000)); // Outside the admitted clock drift.
    assert(!clock.locate(clock.snapshot(), origin + 1000000, 0, 48000, 48000).valid);
    assert(!clock.locate(clock.snapshot(), origin + 10000000, 0, 48000, 48000).valid);
}
void verifiedPhaseCorrections() {
    constexpr uint64_t origin = 1000000000;
    for (double nominal : {8000.0, 48000.0, 192000.0}) {
        for (double samples : {1.1, 2.21}) {
            for (int sign : {-1, 1}) {
                const auto interval = frameNs(32, nominal);
                const auto correction = static_cast<int64_t>(samples * 1e9 / nominal) * sign;
                const auto corrected = static_cast<uint64_t>(interval + correction);
                for (bool trusted : {false, true}) {
                    SampleClockHistory clock(8);
                    assert(clock.append(0, origin));
                    assert(clock.append(32, origin + corrected, trusted));
                    const auto middle = realtime([&] {
                        return clock.locate(clock.snapshot(), origin + corrected / 2, 0, nominal,
                                            nominal);
                    });
                    assert(middle.valid == trusted);
                    if (trusted) {
                        assert(middle.localClockCorrection);
                        assert(std::abs(middle.offset - 16) < 0.001);
                    }
                    const auto tail = clock.locate(
                        clock.snapshot(), origin + corrected + interval / 2, 0, nominal, nominal);
                    assert(tail.valid == trusted);
                    if (trusted)
                        assert(!tail.localClockCorrection && tail.rate == nominal);
                }
            }
        }
        SampleClockHistory rejected(4);
        assert(rejected.append(0, origin));
        assert(rejected.append(32, origin + frameNs(32, nominal) + frameNs(4, nominal), true));
        assert(!rejected
                    .locate(rejected.snapshot(), origin + frameNs(16, nominal), 0, nominal, nominal)
                    .valid);
        SampleClockHistory jumped(4);
        assert(jumped.append(0, origin));
        assert(jumped.append(32, origin + frameNs(64, nominal), true));
        assert(!jumped.locate(jumped.snapshot(), origin + frameNs(16, nominal), 0, nominal, nominal)
                    .valid);
    }
}
void overlappingPublication() {
    SampleClockHistory clock(64);
    constexpr uint64_t origin = 9000000000ULL;
    for (uint64_t block = 0; block < 64; ++block)
        assert(clock.append(block * 64, origin + frameNs(block * 64, 48000)));
    std::atomic<uint64_t> ready{0}, done{0}, acknowledged{0};
    std::thread producer([&] {
        for (uint64_t round = 1; round <= 1000; ++round) {
            while (ready.load() < round)
                std::this_thread::yield();
            const auto frame = (63 + round) * 64;
            assert(clock.append(frame, origin + frameNs(frame, 48000)));
            done.store(round);
            while (acknowledged.load() < round)
                std::this_thread::yield();
        }
    });
    uint64_t reads = 0;
    for (uint64_t round = 1; round <= 1000; ++round) {
        const auto snapshot = clock.snapshot();
        const auto frame = (snapshot.end - 16) * 64 + 16;
        ready.store(round);
        do {
            const auto point = realtime([&] {
                return clock.locate(snapshot, origin + frameNs(frame, 48000), 0.37L, 48000, 48000,
                                    origin + frameNs((snapshot.end - 2) * 64, 48000));
            });
            assert(point.valid && std::abs(point.frame + point.offset - frame) < 0.0001);
            ++reads;
        } while (done.load() < round);
        acknowledged.store(round);
    }
    producer.join();
    assert(reads >= 1000);
}
} // namespace
int main() {
    intervalsResetAndWrap();
    coalescingPreservesClockBoundaries();
    precisionAndInvalidIntervals();
    verifiedPhaseCorrections();
    overlappingPublication();
    std::puts("Sample clock history: intervals, precision, reset, wrap, concurrent readers and no "
              "callback heap activity passed");
}
