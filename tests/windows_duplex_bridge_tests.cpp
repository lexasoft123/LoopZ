#include "../native/platform/windows/driver/duplex_bridge.hpp"
#include "../native/platform/windows/driver/transport_protocol.hpp"
#include "../native/platform/windows/publication_wait.hpp"
#include "../native/platform/windows/publication_cadence.hpp"
#include "../native/platform/windows/ingress_sequence.hpp"
#include "../native/platform/windows/ingress_drain.hpp"
#include <array>
#include <cassert>
#include <iostream>
#include <memory>
#include <vector>
#include <algorithm>

using loopz_driver::DuplexBridge;
void configuredFormats() {
    using loopz_driver::AudioFormat;
    for (unsigned rate : {44100u, 48000u, 88200u, 96000u, 176400u, 192000u}) {
        for (unsigned channels = 2; channels <= 64; channels += 2) {
            const AudioFormat format{rate, channels};
            assert(format.valid());
            auto bridge = std::make_unique<DuplexBridge>();
            constexpr unsigned guard = 32;
            std::vector<unsigned char> ingress(DuplexBridge::ingressBytes(format) + 2 * guard, 211),
                recording(format.ringBytes() + 2 * guard, 177),
                input(DuplexBridge::packetFrames * format.frameBytes()), output(input.size(), 0);
            assert(!bridge->configure(format, nullptr, 0, recording.data(), format.ringBytes()));
            assert(!bridge->configure(format, ingress.data(), DuplexBridge::ingressBytes(format),
                                      recording.data(), format.ringBytes() - 1));
            assert(bridge->configure(format, ingress.data() + guard,
                                     DuplexBridge::ingressBytes(format), recording.data() + guard,
                                     format.ringBytes()));
            for (unsigned i = 0; i < input.size(); ++i)
                input[i] = static_cast<unsigned char>(i * 17 + channels);
            bridge->playback(true);
            bridge->capture(true);
            const auto session = bridge->connect(100);
            assert(session);
            assert(!bridge->configure(format, ingress.data(), DuplexBridge::ingressBytes(format),
                                      recording.data(), format.ringBytes()));
            assert(bridge->pushPlayback(input.data(), DuplexBridge::packetFrames, 1234, 100));
            auto packet = std::make_unique<DuplexBridge::Packet>();
            assert(bridge->takePlayback(session, 100, *packet));
            assert(packet->frames == DuplexBridge::packetFrames && packet->time == 1234);
            assert(std::equal(input.begin(), input.end(), packet->samples));
            assert(std::all_of(packet->samples + input.size(),
                               packet->samples + sizeof(packet->samples), [](unsigned char value) {
                                   return value == 0;
                               }));
            const unsigned blocks = (format.prefillFrames() + DuplexBridge::packetFrames - 1) /
                                    DuplexBridge::packetFrames;
            for (unsigned b = 0; b < blocks; ++b)
                assert(
                    bridge->pushProcessed(session, 100, input.data(), DuplexBridge::packetFrames));
            bridge->readRecording(output.data(), DuplexBridge::packetFrames, 100);
            assert(output == input);
            bridge->readRecording(output.data(), DuplexBridge::packetFrames,
                                  101 + DuplexBridge::leaseDuration);
            assert(std::all_of(output.begin(), output.end(), [](unsigned char b) {
                return b == 0;
            }));
            for (unsigned i = 0; i < guard; ++i) {
                assert(ingress[i] == 211 && ingress[ingress.size() - 1 - i] == 211);
                assert(recording[i] == 177 && recording[recording.size() - 1 - i] == 177);
            }
        }
    }
    for (auto format : {AudioFormat{0, 2}, AudioFormat{48000, 0}, AudioFormat{48000, 3},
                        AudioFormat{48000, 66}, AudioFormat{8000, 2}, AudioFormat{384000, 64}})
        assert(!format.valid());
    loopz_driver::AudioFormatRecord record;
    assert(record.valid());
    record.reserved = 1;
    assert(!record.valid());
    record.reserved = 0;
    record.version = 2;
    assert(!record.valid());
}
void queueDiagnostics() {
    auto bridge = std::make_unique<DuplexBridge>();
    bridge->capture(true);
    const auto session = bridge->connect(100);
    std::array<unsigned char, 1440 * 4> bytes{};
    bridge->readRecording(bytes.data(), 10, 101); // Startup, not a primed underrun.
    auto state = bridge->diagnostics();
    assert(state.missingFrames == 10 && state.underruns == 0 && state.recordingFrames == 10);
    for (unsigned i = 0; i < 3; ++i)
        assert(bridge->pushProcessed(session, 102, bytes.data(), 480));
    assert(bridge->diagnostics().queuedFrames == 1440);
    bridge->readRecording(bytes.data(), 480, 103);
    assert(bridge->diagnostics().queuedFrames == 960);
    bridge->readRecording(bytes.data(), 1440, 104);
    state = bridge->diagnostics();
    assert(state.underruns == 1 && state.missingFrames == 490);
    assert(state.lastUnderrunTime == 104 && state.lastUnderrunQueued == 960 &&
           state.lastUnderrunRequested == 1440);
    bridge->readRecording(bytes.data(), 10, 105); // Re-prime silence is separate.
    state = bridge->diagnostics();
    assert(state.underruns == 1 && state.missingFrames == 500 && state.recordingFrames == 1940);
    assert(state.lastUnderrunTime == 104);
    for (unsigned i = 0; i < 21; ++i)
        assert(bridge->pushProcessed(session, 106, bytes.data(), 480));
    state = bridge->diagnostics();
    assert(state.queuedFrames == 9600 && state.droppedFrames == 480);
    bridge->readRecording(bytes.data(), 5, 107 + DuplexBridge::leaseDuration);
    state = bridge->diagnostics();
    assert(state.leaseSilentFrames == 5 && state.recordingFrames == 1945);
    assert(state.underruns == 1 && state.missingFrames == 500 && state.queuedFrames == 0);
    assert(bridge->diagnostics().leaseSilentFrames == state.leaseSilentFrames);
}
void lateDuplexService() {
    using Bridge = loopz_driver::FloatDuplexBridge;
    for (unsigned rate : {48000u, 192000u})
        for (bool minimumService : {false, true}) {
            const loopz_driver::AudioFormat format{rate, 64};
            auto bridge = std::make_unique<Bridge>();
            std::vector<unsigned char> ingress(Bridge::ingressBytes(format)),
                recording(Bridge::recordingBytes(format)),
                samples(Bridge::packetFrames * format.channels * 4);
            assert(bridge->configure(format, ingress.data(), static_cast<unsigned>(ingress.size()),
                                     recording.data(), static_cast<unsigned>(recording.size())));
            bridge->playback(true);
            const auto session = bridge->connect(100);
            auto packet = std::make_unique<Bridge::Packet>();
            uint64_t produced = 0, received = 0;
            const auto batchFrames = rate / 100;
            // Keep every output chunk overdue. Optional deadline-only intake
            // then starves indefinitely even though the worker keeps running.
            for (unsigned batch = 0; batch < 100; ++batch) {
                for (unsigned offset = 0; offset < batchFrames;) {
                    const auto count = std::min(Bridge::packetFrames, batchFrames - offset);
                    assert(bridge->pushPlayback(
                        samples.data(), count,
                        1000 + loopz_driver::FrameClock::timeForFrames(produced, rate), 100));
                    produced += count;
                    offset += count;
                }
                for (unsigned offset = 0; offset < batchFrames;) {
                    const auto count = std::min(Bridge::packetFrames, batchFrames - offset);
                    uint32_t drained = 0;
                    for (unsigned reads = 0; reads < Bridge::packetCapacity &&
                                             loopz::windows::ingressDrainMayContinue(
                                                 9000000, 0, drained, minimumService ? count : 0);
                         ++reads) {
                        if (!bridge->takePlayback(session, 100, *packet))
                            break;
                        assert(packet->sequence == received / Bridge::packetFrames);
                        drained += packet->frames;
                        received += packet->frames;
                    }
                    offset += count;
                }
            }
            if (minimumService) {
                assert(received == produced && bridge->diagnostics().ingressPackets == 0);
            } else {
                assert(received == 0 &&
                       bridge->diagnostics().ingressPackets == Bridge::packetCapacity);
                assert(bridge->takePlayback(session, 100, *packet));
                assert(packet->sequence > 0); // Earlier packets really were discarded.
            }
        }
}
int main() {
    lateDuplexService();
    {
        loopz::windows::IngressSequence sequence;
        auto value = sequence.append(50);
        assert(!value.skippedPackets && !value.reset);
        value = sequence.append(51);
        assert(!value.skippedPackets && !value.reset);
        value = sequence.append(56);
        assert(value.skippedPackets == 4 && !value.reset);
        value = sequence.append(56);
        assert(!value.skippedPackets && value.reset);
        value = sequence.append(0);
        assert(!value.skippedPackets && value.reset);
        value = sequence.append(1);
        assert(!value.skippedPackets && !value.reset);
        loopz::windows::IngressSequence wrapping;
        (void)wrapping.append(UINT64_MAX - 1);
        value = wrapping.append(1);
        assert(value.skippedPackets == 2 && !value.reset);
        value = wrapping.append(2);
        assert(!value.skippedPackets && !value.reset);
    }
    using loopz::windows::publicationBatchFrames;
    using loopz::windows::publicationBatchTime;
    for (const auto rate : {44100u, 48000u, 88200u, 96000u, 176400u, 192000u}) {
        assert(publicationBatchFrames(rate, false) * 100 == rate);
        assert(publicationBatchFrames(rate, true) * 300 == rate);
        // Three small batches preserve the old 10 ms sample count exactly.
        assert(publicationBatchFrames(rate, true) * 3 == publicationBatchFrames(rate, false));
    }
    assert(!publicationBatchFrames(0, true) && !publicationBatchFrames(44101, true));
    constexpr uint64_t origin = 123456789;
    for (uint64_t batch = 0; batch < 1080000; ++batch) {
        const auto small = publicationBatchTime(origin, batch, true);
        const auto next = publicationBatchTime(origin, batch + 1, true);
        assert(next - small == 3333333 || next - small == 3333334);
        if (batch % 3 == 0)
            assert(small == publicationBatchTime(origin, batch / 3, false));
    }
    assert(publicationBatchTime(origin, 1080000, true) == origin + 3600000000000ULL);
    assert(publicationBatchTime(UINT64_MAX - 1, 1, true) == UINT64_MAX);
    assert(publicationBatchTime(1, UINT64_MAX, true) == UINT64_MAX);
    using loopz::windows::publicationFlowPolicy;
    using loopz::windows::publicationWaitDeadline;
    for (unsigned version : {0u, 1u, 2u, 3u, 4u})
        for (unsigned sampleBytes : {0u, 2u, 4u, 8u})
            for (bool published : {false, true})
                for (bool smallBatch : {false, true}) {
                    const auto flow =
                        publicationFlowPolicy(version, sampleBytes, published, smallBatch);
                    if (flow.paired) {
                        assert(version == 3 && sampleBytes == 4 && published && !smallBatch);
                        assert(flow.version() == 1);
                        assert(!flow.acceptsQueueStatus(false));
                    } else {
                        assert(version != 3 || sampleBytes != 4 || !published || smallBatch);
                        assert(flow.version() == 0);
                        assert(flow.acceptsQueueStatus(false));
                    }
                    assert(flow.acceptsQueueStatus(true));
                }
    assert(publicationWaitDeadline(0, 0, 960, 48000, 8000000) == 12000000);
    assert(publicationWaitDeadline(2000000, 0, 384, 48000, 3000000) == 7000000);
    assert(publicationWaitDeadline(4000000, 0, 480, 48000, 3000000) == 11000000);
    assert(publicationWaitDeadline(4000000, 0, 48, 48000, 3000000) == 4000000);
    assert(publicationWaitDeadline(10000000, 0, 960, 48000, 0) == 30000000);
    assert(publicationWaitDeadline(1, 2, 960, 48000, 0) == 1);
    assert(publicationWaitDeadline(1, 0, 960, 0, 0) == 1);
    assert(publicationWaitDeadline(UINT64_MAX - 1, UINT64_MAX - 2, 960, 48000, 0) ==
           UINT64_MAX - 1);
    // A producer finishing 200 us after a late chunk becomes due remains
    // reachable without consuming the FIFO's 3 ms delivery reserve. The old
    // nominal-batch cap denied this wait and rendered unpublished samples.
    const auto lateChunk = uint64_t{11000000};
    const auto producerReady = lateChunk + 200000;
    const auto deadline = publicationWaitDeadline(lateChunk, 0, 882, 88200, 3000000);
    assert(producerReady < deadline);
    assert(deadline - lateChunk + 3000000 <= uint64_t{882} * 1000000000 / 88200);
    assert(publicationWaitDeadline(29000000, 0, 882, 88200, 3000000) == 30000000);
    assert(publicationWaitDeadline(30000000, 0, 882, 88200, 3000000) == 30000000);
    assert(publicationWaitDeadline(11000000, 0, 240, 88200, 3000000) == 11000000);
    // Across supported rates and queue depths, waiting must leave the reserve
    // intact and must not push work beyond the worker's existing failure limit.
    for (const auto rate : {44100u, 48000u, 88200u, 96000u, 176400u, 192000u})
        for (unsigned queued = 0; queued <= 9600; queued += 137)
            for (uint64_t elapsed = 0; elapsed <= 31000000; elapsed += 500000) {
                constexpr uint64_t due = 100000000;
                constexpr uint64_t reserve = 4000000;
                const auto queried = due + elapsed;
                const auto result = publicationWaitDeadline(queried, due, queued, rate, reserve);
                assert(result >= queried);
                if (result > queried) {
                    assert(result <= due + loopz::windows::privateWorkerLatenessLimitNs);
                    assert(result - queried + reserve <= uint64_t(queued) * 1000000000 / rate);
                }
            }
    queueDiagnostics();
    configuredFormats();
    // The object lives in adapter-owned nonpaged memory, never on a kernel stack.
    auto bridge = std::make_unique<DuplexBridge>();
    std::array<unsigned char, DuplexBridge::packetFrames * 4> input{}, processed{}, output{};
    input.fill(19);
    processed.fill(73);
    const auto silence = [&] {
        for (const auto sample : output)
            assert(sample == 0);
    };
    DuplexBridge::Packet packet;
    constexpr unsigned frames = DuplexBridge::packetFrames;
    bridge->playback(true);
    bridge->capture(true);
    assert(!bridge->pushPlayback(input.data(), frames, 0, 0));
    const auto first = bridge->connect(0);
    assert(first != 0);
    assert(bridge->connect(0) == 0);
    // Public playback is readable privately, but cannot leak into recording.
    assert(bridge->pushPlayback(input.data(), frames, 123, 1));
    bridge->readRecording(output.data(), frames, 1);
    silence();
    assert(bridge->takePlayback(first, 1, packet));
    assert(packet.frames == frames && packet.time == 123 && packet.discontinuity == 1);
    for (unsigned i = 0; i < sizeof(packet.samples); ++i)
        assert(packet.samples[i] == (i < input.size() ? 19 : 0));
    assert(!bridge->takePlayback(first, 2, packet));
    assert(packet.frames == 0 && packet.time == 0); // no stale response bytes
    // Two processed packets satisfy the recording prefill, independently of ingress.
    for (unsigned i = 0; i < 2; ++i)
        assert(bridge->pushProcessed(first, 3, processed.data(), frames));
    bridge->readRecording(output.data(), frames, 3);
    for (const auto sample : output)
        assert(sample == 73);
    bridge->playback(false);
    bridge->readRecording(output.data(), frames, 4);
    for (const auto sample : output)
        assert(sample == 73); // an app-only mix needs no public playback client
    bridge->readRecording(output.data(), frames, 4);
    silence();
    // Capture close clears pending processed audio, but not monitor ingress.
    bridge->playback(true);
    assert(bridge->pushPlayback(input.data(), frames, 456, 5));
    bridge->capture(false);
    assert(bridge->takePlayback(first, 5, packet));
    assert(packet.time == 456);
    assert(bridge->pushProcessed(first, 5, processed.data(), frames));
    bridge->capture(true);
    bridge->readRecording(output.data(), frames, 5);
    silence();
    // Overflow preserves the most recent packets and signals discontinuity once.
    for (unsigned i = 0; i < DuplexBridge::packetCapacity + 3; ++i)
        assert(bridge->pushPlayback(input.data(), 1, 1000 + i, 6));
    for (unsigned i = 0; i < DuplexBridge::packetCapacity; ++i) {
        assert(bridge->takePlayback(first, 6, packet));
        assert(packet.time == 1003 + i);
        assert(packet.frames == 1 && packet.discontinuity == (i == 0 ? 1u : 0u));
        for (unsigned j = 4; j < sizeof(packet.samples); ++j)
            assert(packet.samples[j] == 0);
    }
    assert(!bridge->pushPlayback(nullptr, 1, 0, 7));
    assert(!bridge->pushPlayback(input.data(), frames + 1, 0, 7));
    assert(!bridge->pushProcessed(first, 7, nullptr, 1));
    assert(!bridge->pushProcessed(first, 7, processed.data(), frames + 1));
    // A dead engine cannot keep recorded audio queued or renew an expired lease.
    for (unsigned i = 0; i < 2; ++i)
        assert(bridge->pushProcessed(first, 8, processed.data(), frames));
    assert(bridge->pushPlayback(input.data(), frames, 2000, 8));
    constexpr auto expired = 9 + DuplexBridge::leaseDuration;
    bridge->readRecording(output.data(), frames, expired);
    silence();
    assert(!bridge->pushProcessed(first, expired, processed.data(), frames));
    assert(!bridge->takePlayback(first, expired, packet));
    const auto second = bridge->connect(expired);
    assert(first != second);
    assert(!bridge->disconnect(first));
    assert(!bridge->pushProcessed(first, expired, processed.data(), frames));
    assert(!bridge->takePlayback(second, expired, packet));
    bridge->readRecording(output.data(), frames, expired);
    silence();
    // A stale caller cannot prolong a newer session's lease.
    assert(!bridge->takePlayback(first, expired + DuplexBridge::leaseDuration, packet));
    assert(!bridge->pushProcessed(second, expired + DuplexBridge::leaseDuration + 1,
                                  processed.data(), frames));
    const auto third = bridge->connect(expired + DuplexBridge::leaseDuration + 2);
    assert(bridge->disconnect(third));
    assert(
        !bridge->pushPlayback(input.data(), frames, 0, expired + DuplexBridge::leaseDuration + 2));
    // Clock regression fails closed instead of unsigned-wrap lease extension.
    const auto fourth = bridge->connect(100);
    assert(!bridge->pushProcessed(fourth, 99, processed.data(), frames));
    bridge->readRecording(output.data(), frames, 100);
    silence();
    // Lease boundaries are inclusive and polling without playback keeps an
    // application-only mix live. Independent adapters cannot exchange packets.
    const auto fifth = bridge->connect(200);
    assert(fifth != 0);
    assert(!bridge->takePlayback(fifth, 200 + DuplexBridge::leaseDuration, packet));
    assert(bridge->pushProcessed(fifth, 200 + 2 * DuplexBridge::leaseDuration, processed.data(),
                                 frames));
    auto other = std::make_unique<DuplexBridge>();
    other->playback(true);
    other->capture(true);
    const auto otherSession = other->connect(0);
    assert(otherSession != 0);
    assert(other->pushPlayback(input.data(), frames, 42, 0));
    other->readRecording(output.data(), frames, 0);
    silence();
    assert(bridge->pushPlayback(input.data(), frames, 84, 200 + 2 * DuplexBridge::leaseDuration));
    bridge->playback(false);
    bridge->playback(true);
    assert(!bridge->takePlayback(fifth, 200 + 2 * DuplexBridge::leaseDuration, packet));
    assert(other->takePlayback(otherSession, 0, packet) && packet.time == 42);
    std::cout << "Windows duplex isolation, lifecycle, overflow and engine lease checks passed\n";
}
