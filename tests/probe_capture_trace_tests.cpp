#include <loopz/probe_capture_trace.hpp>

#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>

namespace {
bool trackHeap = false;
unsigned heapEvents = 0;
} // namespace
void* operator new(std::size_t size) {
    if (trackHeap)
        ++heapEvents;
    if (auto* value = std::malloc(size ? size : 1))
        return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void* value) noexcept {
    if (trackHeap && value)
        ++heapEvents;
    std::free(value);
}
void operator delete[](void* value) noexcept {
    ::operator delete(value);
}
void operator delete(void* value, std::size_t) noexcept {
    ::operator delete(value);
}
void operator delete[](void* value, std::size_t) noexcept {
    ::operator delete(value);
}

int main() {
    auto trace = std::make_unique<loopz::probe::CaptureTrace>();
    std::array<float, 64> left, right;
    left.fill(.1f);
    right.fill(.2f);
    left[3] = left[4] = right[4] = right[5] = 0;
    constexpr uint64_t base = 100000000000ULL;
    heapEvents = 0;
    trackHeap = true;
    trace->observe(left, right, 0, base, base + 1000, 48000, true, 16);
    trackHeap = false;
    assert(!heapEvents && trace->zeroSamples == 4 && trace->capturedFrames == 64);
    assert(trace->zeros().size() == 3 && trace->gaps().empty());
    for (unsigned index = 0; index < 3; ++index) {
        const auto& span = trace->zeros()[index];
        assert(span.capturedFrame == index + 3 && span.frames == 1);
        assert(span.channels == (index == 0 ? 1 : index == 1 ? 3 : 2));
        assert(span.hostNs == base + uint64_t((index + 3) * 1e9 / 48000));
        assert(span.receivedNs == base + 1000 && span.callback == 0 && span.renderFlags == 16);
    }
    left.fill(.1f);
    right.fill(.2f);
    trace->observe(left, right, 64, base + 1333333, base + 1334333, 48000, true, 0);
    assert(trace->gaps().empty());
    trace->observe(left, right, 160, base + 3333333, base + 3334333, 48000, true, 0);
    assert(trace->gaps().size() == 1);
    auto gap = trace->gaps()[0];
    assert(gap.sampleFrameJump == 32 && gap.hostTimeJumpNs == 666667 && gap.timestampValid);
    assert(gap.callback == 2 && gap.capturedFrame == 128 && gap.receivedNs == base + 3334333);
    trace->observe(left, right, 192, base + 4000000, base + 4001000, 48000, true, 0);
    assert(trace->gaps().size() == 2 && trace->gaps()[1].sampleFrameJump == -32);
    trace->observe(left, right, 256, base + 10000000, base + 10001000, 48000, true, 0);
    assert(trace->gaps().size() == 3 && trace->gaps()[2].sampleFrameJump == 0 &&
           trace->gaps()[2].hostTimeJumpNs > 4000000);
    trace->observe(left, right, NAN, 0, base + 11000000, 48000, false, 0);
    assert(trace->gaps().size() == 4 && !trace->gaps()[3].timestampValid);
    trace->observe(left, right, 400, base + 12000000, base + 12001000, 48000, true, 0);
    assert(trace->gaps().size() == 4); // First valid callback re-establishes the sequence.
    assert(trace->callbacks == 7 && trace->capturedFrames == 448);
    trace->observe({}, right, 0, 1, 1, 48000, true, 0);
    trace->observe(left, right, 0, 1, 1, 0, true, 0);
    assert(trace->invalidBlocks == 2 && trace->callbacks == 7);

    auto bounded = std::make_unique<loopz::probe::CaptureTrace>();
    for (unsigned index = 0; index < 64; ++index)
        left[index] = index % 2 ? .1f : 0;
    heapEvents = 0;
    trackHeap = true;
    for (unsigned block = 0; block < 20; ++block)
        bounded->observe(left, right, block * 64, base + uint64_t(block * 64e9 / 48000),
                         base + uint64_t(block * 64e9 / 48000), 48000, true, 0);
    trackHeap = false;
    assert(!heapEvents && bounded->zeros().size() == loopz::probe::CaptureTrace::capacity);
    assert(bounded->overflows == 128 && bounded->zeroSamples == 640);

    auto limits = std::make_unique<loopz::probe::CaptureTrace>();
    limits->observe(left, right, 0, UINT64_MAX - 1000000000ULL, 1, 48000, true, 0);
    limits->observe(left, right, 64, 1, 2, 48000, true, 0);
    assert(limits->gaps().size() == 1 && limits->gaps()[0].hostTimeJumpNs == -INT64_MAX);
    std::puts("PASS: exact zero-span masks/times/flags, forward/backward/host-only gaps, invalid "
              "timestamps, bounded overflow and callback heap freedom");
}
