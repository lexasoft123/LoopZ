#include <loopz/capture.hpp>
#include <loopz/graph.hpp>
#include <cassert>
#include <cstdio>
#include <thread>
#include <memory>
#include <array>
#include <cmath>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

int main() {
    using namespace loopz;
    auto ring = std::make_unique<Timeline>();
    ring->write(42, 0.25f, -0.5f);
    float a, b, c, d;
    assert(ring->read(42, a, b) && ring->read(42, c, d) && a == c && b == d);
    ring->write(42 + kCapacity, 0.8f, 0.9f);
    assert(!ring->read(42, a, b) && a == 0 && b == 0);
    ring->write(43, NAN, INFINITY);
    assert(ring->read(43, a, b) && a == 0 && b == 0);
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (uint64_t frame = 0; frame < 200000; ++frame)
            ring->write(frame, static_cast<float>(frame), -static_cast<float>(frame));
        done.store(true);
    });
    uint64_t reads = 0;
    while (!done.load()) {
        for (uint64_t frame = 0; frame < 200000; frame += 977)
            if (ring->read(frame, a, b)) {
                assert(a == static_cast<float>(frame) && b == -static_cast<float>(frame));
                ++reads;
            }
    }
    writer.join();
    struct Exchange {
        Timeline ring;
        std::atomic<uint64_t> published{0}, acknowledged[2]{};
    };
    // One writer and two live reader processes exchange 128 batches, including
    // wraparound. The writer cannot finish until both independently validate.
    auto* memory =
        mmap(nullptr, sizeof(Exchange), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    assert(memory != MAP_FAILED);
    auto* shared = new (memory) Exchange{};
    pid_t children[2]{};
    for (uint32_t reader = 0; reader < 2; ++reader) {
        children[reader] = fork();
        assert(children[reader] >= 0);
        if (children[reader] == 0) {
            uint64_t observations = 0;
            for (uint64_t batch = 1; batch <= 128; ++batch) {
                while (shared->published.load() < batch)
                    std::this_thread::yield();
                for (uint64_t i = (batch - 1) * 512; i < batch * 512; ++i) {
                    float left, right;
                    if (!shared->ring.read(i, left, right) || left != static_cast<float>(i) ||
                        right != -static_cast<float>(i))
                        _exit(1);
                    ++observations;
                }
                shared->acknowledged[reader].store(batch);
            }
            _exit(observations == 65536 ? 0 : 1);
        }
    }
    for (uint64_t batch = 1; batch <= 128; ++batch) {
        for (uint64_t i = (batch - 1) * 512; i < batch * 512; ++i)
            shared->ring.write(i, static_cast<float>(i), -static_cast<float>(i));
        shared->published.store(batch);
        while (shared->acknowledged[0].load() < batch || shared->acknowledged[1].load() < batch)
            std::this_thread::yield();
    }
    for (auto child : children) {
        int status = 0;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
               WEXITSTATUS(status) == 0);
    }
    munmap(memory, sizeof(Exchange));
    // Smaller actual renders must preserve output across callback partitions,
    // including limiter state/nonfinite containment, without writing the tail.
    for (double rate : {44100.0, 48000.0}) {
        constexpr uint32_t frames = 1024;
        std::array<float, frames> al{}, ar{}, ml{}, mr{}, pl{}, pr{}, expectedL{}, expectedR{};
        for (uint32_t i = 0; i < frames; ++i) {
            al[i] = 1.2f * std::sin(2 * M_PI * 1100 * i / rate);
            ar[i] = 1.1f * std::sin(2 * M_PI * 2200 * i / rate);
            ml[i] = 0.2f;
            mr[i] = -0.1f;
            pl[i] = 0.05f;
        }
        al[123] = NAN;
        ar[124] = INFINITY;
        MixingGraph reference(rate);
        for (uint32_t i = 0; i < frames; i += kBlock)
            assert(reference.render(al.data() + i, ar.data() + i, ml.data() + i, mr.data() + i,
                                    pl.data() + i, pr.data() + i, expectedL.data() + i,
                                    expectedR.data() + i, kBlock, i));
        for (uint32_t quantum : {64u, 128u}) {
            MixingGraph candidate(rate);
            std::array<float, frames + 2> actualL{}, actualR{};
            actualL.fill(99);
            actualR.fill(-99);
            for (uint32_t i = 0; i < frames; i += quantum) {
                assert(candidate.render(al.data() + i, ar.data() + i, ml.data() + i, mr.data() + i,
                                        pl.data() + i, pr.data() + i, actualL.data() + i + 1,
                                        actualR.data() + i + 1, quantum, i));
                assert(actualL[0] == 99 && actualR[0] == -99);
                assert(actualL[i + quantum + 1] == 99 && actualR[i + quantum + 1] == -99);
            }
            for (uint32_t i = 0; i < frames; ++i) {
                assert(std::isfinite(actualL[i + 1]) && std::isfinite(actualR[i + 1]));
                assert(std::fabs(actualL[i + 1] - expectedL[i]) < 1e-6);
                assert(std::fabs(actualR[i + 1] - expectedR[i]) < 1e-6);
                assert(std::fabs(actualL[i + 1]) <= 0.98001f);
                assert(std::fabs(actualR[i + 1]) <= 0.98001f);
            }
        }
        auto largeCapture = std::make_unique<CaptureBuffer>();
        std::array<float, 4096> capturedL{}, capturedR{};
        capturedL.fill(0.5f);
        capturedR.fill(-0.5f);
        largeCapture->append(capturedL.data(), capturedR.data(), 4096, 1000000000, rate);
        std::array<float, 64> smallL{}, smallR{};
        assert(largeCapture->callbacks.load() == 1);
        assert(largeCapture->sample(1000000000 + frameNs(3840, rate), rate, smallL.data(),
                                    smallR.data(), 64) == 0);
        for (uint32_t i = 0; i < 64; ++i)
            assert(smallL[i] == 0.5f && smallR[i] == -0.5f);
    }
    for (double rate : {44100.0, 48000.0}) {
        MixingGraph graph(rate);
        std::array<float, kBlock> al{}, ar{}, ml{}, mr{}, pl{}, pr{}, left{}, right{};
        al.fill(0.1f);
        ar.fill(0.2f);
        ml.fill(0.03f);
        mr.fill(-0.04f);
        pl.fill(0.02f);
        pr.fill(0.01f);
        for (uint32_t block = 0; block < 8; ++block)
            assert(graph.render(al.data(), ar.data(), ml.data(), mr.data(), pl.data(), pr.data(),
                                left.data(), right.data(), kBlock, block * kBlock));
        for (uint32_t i = 0; i < kBlock; ++i) {
            assert(std::fabs(left[i] - 0.15f) < 1e-5);
            assert(std::fabs(right[i] - 0.17f) < 1e-5);
        }
        al.fill(5);
        ar.fill(-5);
        assert(graph.render(al.data(), ar.data(), ml.data(), mr.data(), pl.data(), pr.data(),
                            left.data(), right.data(), kBlock, 2048));
        for (uint32_t i = 0; i < kBlock; ++i)
            assert(std::isfinite(left[i]) && std::fabs(left[i]) <= 0.98001f &&
                   std::fabs(right[i]) <= 0.98001f);
        assert(!graph.render(al.data(), ar.data(), ml.data(), mr.data(), pl.data(), pr.data(),
                             left.data(), right.data(), kBlock + 1, 0));
    }
    // Simulated +100 ppm capture clock mismatch. The host anchor conversion
    // must keep a constant time offset rather than accumulate rate error.
    auto capture = std::make_unique<CaptureBuffer>();
    std::array<float, kBlock> input{}, inputR{}, output{}, outputR{};
    constexpr double inputRate = 48000 * 1.0001;
    constexpr uint64_t startNs = 1000000000;
    uint64_t misses = 0, checked = 0;
    double maximumError = 0;
    // About 7.5 seconds of synthetic samples; this is neither a physical nor
    // a two-hour device test.
    for (uint64_t block = 0; block < 1400; ++block) {
        const auto first = block * kBlock;
        for (uint32_t i = 0; i < kBlock; ++i) {
            input[i] = static_cast<float>(std::sin(2 * M_PI * 220 * (first + i) / inputRate));
            inputR[i] = -input[i];
        }
        capture->append(input.data(), inputR.data(), kBlock, startNs + frameNs(first, inputRate),
                        48000);
        if (block > 300) {
            const auto target = startNs + frameNs(first - 1024, inputRate);
            misses += capture->sample(target, 48000, output.data(), outputR.data(), kBlock);
            for (uint32_t i = 0; i < kBlock; ++i) {
                const double expected =
                    std::sin(2 * M_PI * 220 * ((target - startNs) * 1e-9 + i / 48000.0));
                maximumError = std::max(maximumError, std::fabs(output[i] - expected));
                assert(std::fabs(output[i] + outputR[i]) < 1e-6);
                ++checked;
            }
        }
    }
    assert(misses == 0 && maximumError < 0.002);
    // Even a single missing 64-frame block must invalidate the source timeline;
    // the old history cannot be replayed at the new host-time anchor.
    auto discontinuous = std::make_unique<CaptureBuffer>();
    input.fill(0.5f);
    inputR.fill(-0.5f);
    for (uint64_t block = 0; block < 32; ++block)
        discontinuous->append(input.data(), inputR.data(), kBlock,
                              startNs + frameNs(block * kBlock, 48000), 48000, block * kBlock);
    CaptureBuffer::ReaderState cachedClock;
    assert(discontinuous->sample(startNs + frameNs(28 * kBlock, 48000), 48000, output.data(),
                                 outputR.data(), kBlock, &cachedClock) == 0);
    const auto afterGap = startNs + frameNs(32 * kBlock + 64, 48000);
    discontinuous->append(input.data(), inputR.data(), kBlock, afterGap, 48000, 32 * kBlock + 64);
    assert(discontinuous->discontinuities.load() == 1);
    assert(discontinuous->sample(afterGap - frameNs(1024, 48000), 48000, output.data(),
                                 outputR.data(), kBlock, &cachedClock) == kBlock);
    discontinuous->append(input.data(), inputR.data(), kBlock, afterGap + frameNs(kBlock, 48000),
                          48000, 0);
    assert(discontinuous->discontinuities.load() == 2);
    discontinuous->append(input.data(), inputR.data(), kBlock, afterGap + 1000000000, 44100, 0);
    assert(discontinuous->discontinuities.load() == 3);
    // Exercise a realtime reader overlapping a small-buffer capture producer.
    // The producer cannot outrun the retained history while this block is read.
    auto concurrentCapture = std::make_unique<CaptureBuffer>();
    for (uint64_t block = 0; block < 64; ++block)
        concurrentCapture->append(input.data(), inputR.data(), 128,
                                  startNs + frameNs(block * 128, 48000), 48000, block * 128);
    CaptureBuffer::ReaderState concurrentClock;
    assert(concurrentCapture->sample(startNs + frameNs(6144, 48000), 48000, output.data(),
                                     outputR.data(), kBlock, &concurrentClock) == 0);
    std::atomic<uint64_t> ready{0}, captureDone{0}, acknowledged{0};
    std::thread captureWriter([&] {
        for (uint64_t round = 1; round <= 1000; ++round) {
            while (ready.load() < round)
                std::this_thread::yield();
            const auto first = (63 + round) * 128;
            concurrentCapture->append(input.data(), inputR.data(), 128,
                                      startNs + frameNs(first, 48000), 48000, first);
            captureDone.store(round);
            while (acknowledged.load() < round)
                std::this_thread::yield();
        }
    });
    uint64_t overlappingReads = 0;
    for (uint64_t round = 1; round <= 1000; ++round) {
        const auto target = startNs + frameNs((63 + round) * 128 - 2048, 48000);
        ready.store(round);
        do {
            assert(concurrentCapture->sample(target, 48000, output.data(), outputR.data(), kBlock,
                                             &concurrentClock) == 0);
            for (uint32_t i = 0; i < kBlock; ++i)
                assert(output[i] == 0.5f && outputR[i] == -0.5f);
            ++overlappingReads;
        } while (captureDone.load() < round);
        acknowledged.store(round);
    }
    captureWriter.join();
    assert(overlappingReads >= 1000);
    std::printf("PASS: two readers, overwrite silence, nonfinite containment, concurrent snapshots "
                "(%llu reads), cross-process atomics, real zdsp mixing/limiting at both rates, "
                "+100 ppm timestamp conversion (%llu samples, max error %.6f)\n",
                reads, checked, maximumError);
}
