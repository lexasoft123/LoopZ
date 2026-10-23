#include "../native/platform/windows/driver/transport_protocol.hpp"
#include "../native/platform/windows/driver/sample_bits.hpp"
#include "../native/platform/windows/driver/frame_clock.hpp"
#include "../native/platform/windows/playback_clock.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

using namespace loopz_driver;
void fragmentedIngress() {
    for (unsigned rate : {44100u, 48000u, 88200u, 96000u, 176400u, 192000u}) {
        for (unsigned fragment : {1u, 37u, 239u, 240u}) {
            const AudioFormat format{rate, 2};
            auto bridge = std::make_unique<FloatDuplexBridge>();
            std::vector<unsigned char> ingress(FloatDuplexBridge::ingressBytes(format)),
                recording(FloatDuplexBridge::recordingBytes(format));
            assert(bridge->configure(format, ingress.data(), static_cast<unsigned>(ingress.size()),
                                     recording.data(), static_cast<unsigned>(recording.size())));
            bridge->playback(true);
            const auto session = bridge->connect(100);
            constexpr unsigned total = 9600, stride = 8;
            std::vector<unsigned char> samples(total * stride);
            for (unsigned i = 0; i < samples.size(); ++i)
                samples[i] = static_cast<unsigned char>(i * 37 + i / 251);
            // The same frame budget must survive even when GetPosition delivers
            // one-frame fragments instead of full transport packets.
            for (unsigned i = 0; i < total;) {
                const unsigned count = std::min(fragment, total - i);
                assert(bridge->pushPlayback(samples.data() + i * stride, count,
                                            1000 + FrameClock::timeForFrames(i, rate), 100));
                i += count;
            }
            auto packet = std::make_unique<FloatDuplexBridge::Packet>();
            unsigned consumed = 0, sequence = 0;
            loopz::windows::PlaybackClock clock(rate);
            while (consumed < total) {
                assert(bridge->takePlayback(session, 100, *packet));
                assert(packet->sequence == sequence++);
                assert(packet->frames == FloatDuplexBridge::packetFrames);
                const auto expected = 1000 + FrameClock::timeForFrames(consumed, rate);
                const auto difference =
                    packet->time > expected ? packet->time - expected : expected - packet->time;
                assert(difference <= 1);
                const auto stamp = clock.append(packet->time, packet->frames, false);
                assert(stamp.valid && !stamp.discontinuity);
                assert(std::equal(packet->samples, packet->samples + packet->frames * stride,
                                  samples.data() + consumed * stride));
                consumed += packet->frames;
            }
            assert(!bridge->takePlayback(session, 100, *packet) && !packet->frames);
            // A timestamp gap must keep separate packets, without hiding it.
            assert(bridge->pushPlayback(samples.data(), 1, 1000000, 100));
            assert(bridge->pushPlayback(samples.data() + stride, 1, 2000000, 100));
            assert(bridge->takePlayback(session, 100, *packet));
            assert(packet->frames == 1 && packet->time == 1000000);
            assert(clock.append(packet->time, packet->frames, false).discontinuity);
            assert(bridge->takePlayback(session, 100, *packet));
            assert(packet->frames == 1 && packet->time == 2000000);
            assert(clock.append(packet->time, packet->frames, false).discontinuity);
        }
    }
}
void wrapAndOverflow() {
    const AudioFormat format{48000, 64};
    const unsigned capacity = format.ringFrames(), stride = format.channels * 4;
    std::vector<unsigned char> storage(capacity * stride), initial(storage.size()),
        extra(480 * stride), output(storage.size());
    for (unsigned i = 0; i < initial.size() / 4; ++i) {
        const unsigned bits = 0x3f000000U | i;
        std::memcpy(initial.data() + i * 4, &bits, 4);
    }
    for (unsigned i = 0; i < extra.size() / 4; ++i) {
        const unsigned bits = 0x3e000000U | i;
        std::memcpy(extra.data() + i * 4, &bits, 4);
    }
    auto ring = std::make_unique<FloatBridgeRing>();
    assert(ring->configure(format, storage.data(), static_cast<unsigned>(storage.size())));
    ring->reset(true);
    ring->capture(true);
    ring->write(initial.data(), capacity);
    ring->read(output.data(), 240);
    assert(std::equal(output.begin(), output.begin() + 240 * stride, initial.begin()));
    ring->write(extra.data(), 240);
    ring->write(extra.data() + 240 * stride, 240);
    assert(ring->queued() == capacity && ring->dropped() == 240);
    ring->read(output.data(), capacity);
    assert(std::equal(output.begin(), output.begin() + (capacity - 480) * stride,
                      initial.begin() + 480 * stride));
    assert(std::equal(output.begin() + (capacity - 480) * stride, output.end(), extra.begin()));
    assert(ring->missing() == 0 && ring->underruns() == 0);
}
void packedIngressLifecycle() {
    for (unsigned rate : {44100u, 192000u})
        for (unsigned channels : {2u, 64u}) {
            const AudioFormat format{rate, channels};
            const unsigned stride = channels * 4, guard = 32;
            auto bridge = std::make_unique<FloatDuplexBridge>();
            const auto ingressBytes = FloatDuplexBridge::ingressBytes(format);
            const auto recordingBytes = FloatDuplexBridge::recordingBytes(format);
            std::vector<unsigned char> ingress(ingressBytes + guard * 2, 211),
                recording(recordingBytes + guard * 2, 177), samples(12000 * stride);
            for (unsigned i = 0; i < samples.size(); ++i)
                samples[i] = static_cast<unsigned char>(i * 19 + i / 257);
            assert(bridge->configure(format, ingress.data() + guard, ingressBytes,
                                     recording.data() + guard, recordingBytes));
            bridge->playback(true);
            const auto session = bridge->connect(100);
            const auto push = [&](unsigned begin, unsigned end, unsigned fragment) {
                for (unsigned frame = begin; frame < end;) {
                    const auto count = std::min(fragment, end - frame);
                    assert(bridge->pushPlayback(samples.data() + frame * stride, count,
                                                1000 + FrameClock::timeForFrames(frame, rate),
                                                100));
                    frame += count;
                }
            };
            auto packet = std::make_unique<FloatDuplexBridge::Packet>();
            unsigned sequence = 0;
            loopz::windows::PlaybackClock clock(rate);
            const auto read = [&](unsigned begin, unsigned end) {
                for (unsigned frame = begin; frame < end; frame += 240) {
                    assert(bridge->takePlayback(session, 100, *packet));
                    assert(packet->sequence == sequence++ && packet->frames == 240);
                    const auto stamp = clock.append(packet->time, packet->frames, false);
                    assert(stamp.valid && !stamp.discontinuity);
                    assert(std::equal(packet->samples, packet->samples + 240 * stride,
                                      samples.data() + frame * stride));
                }
            };
            push(0, 9600, 37);
            read(0, 2400);
            push(9600, 12000, 239); // Wrap into the slots just consumed.
            read(2400, 12000);
            assert(!bridge->takePlayback(session, 100, *packet));
            // A partial tail must be readable without waiting to become full.
            assert(bridge->pushPlayback(samples.data(), 17, 1000000, 100));
            assert(bridge->takePlayback(session, 100, *packet));
            assert(packet->frames == 17 && packet->sequence == sequence++);
            assert(clock.append(packet->time, packet->frames, false).discontinuity);
            assert(std::equal(packet->samples, packet->samples + 17 * stride, samples.data()));
            // Exceed real capacity: discard the oldest full packet and signal it.
            push(0, 9840, 240);
            ++sequence;
            assert(bridge->takePlayback(session, 100, *packet));
            assert(packet->discontinuity == 1 && packet->sequence == sequence++);
            clock.reset();
            assert(clock.append(packet->time, packet->frames, false).valid);
            assert(std::equal(packet->samples, packet->samples + 240 * stride,
                              samples.data() + 240 * stride));
            read(480, 9840);
            assert(!bridge->takePlayback(session, 100, *packet));
            for (unsigned i = 0; i < guard; ++i) {
                assert(ingress[i] == 211 && ingress[ingress.size() - 1 - i] == 211);
                assert(recording[i] == 177 && recording[recording.size() - 1 - i] == 177);
            }
        }
}
int main() {
    packedIngressLifecycle();
    fragmentedIngress();
    wrapAndOverflow();
    static_assert(offsetof(FloatTransportProcessed, samples) ==
                  offsetof(TransportProcessed, samples));
    static_assert(offsetof(FloatDuplexBridge::Packet, samples) ==
                  offsetof(DuplexBridge::Packet, samples));
    static_assert(sizeof(AudioFormatRecord) == 16); // Protected registry v1 stays unchanged.
    static_assert(DuplexBridge::packetFrames == 480 && DuplexBridge::packetCapacity == 20);
    static_assert(FloatDuplexBridge::packetFrames == 240 &&
                  FloatDuplexBridge::packetCapacity == 40);
    assert(TransportInstance{}.version == 2 && TransportCapabilities{}.bitsPerSample == 16);
    assert(FloatTransportInstance{}.version == 3 &&
           FloatTransportCapabilities{}.bitsPerSample == 32);
    assert(FloatTransportCapabilities{}.maxPacketFrames == 240);
    for (unsigned rate : {44100U, 48000U, 88200U, 96000U, 176400U, 192000U}) {
        for (unsigned channels = 2; channels <= 64; channels += 2) {
            const AudioFormat format{rate, channels};
            auto bridge = std::make_unique<FloatDuplexBridge>();
            constexpr unsigned guard = 32;
            const auto ingressBytes = FloatDuplexBridge::ingressBytes(format);
            const auto recordingBytes = FloatDuplexBridge::recordingBytes(format);
            assert(ingressBytes == DuplexBridge::ingressBytes(format) * 2);
            assert(recordingBytes == format.ringBytes() * 2);
            std::vector<unsigned char> ingress(ingressBytes + guard * 2, 211),
                recording(recordingBytes + guard * 2, 177),
                input(FloatDuplexBridge::packetFrames * channels * 4), output(input.size());
            // Low mantissa bits, signed subnormals and headroom are not PCM16.
            constexpr std::array<unsigned, 8> patterns{0x3f000001U, 0xbf000001U, 1U, 0x80000001U,
                                                       0x40000001U, 0xc0000001U, 0U, 0x80000000U};
            for (unsigned i = 0; i < input.size() / 4; ++i) {
                const unsigned bits = patterns[i % patterns.size()];
                assert(finiteFloatBits(bits) == bits);
                std::memcpy(input.data() + i * 4, &bits, 4);
            }
            assert(!bridge->configure(format, ingress.data() + guard, ingressBytes - 1,
                                      recording.data() + guard, recordingBytes));
            assert(!bridge->configure(format, ingress.data() + guard, ingressBytes,
                                      recording.data() + guard, recordingBytes - 1));
            assert(bridge->configure(format, ingress.data() + guard, ingressBytes,
                                     recording.data() + guard, recordingBytes));
            assert(bridge->frameBytes() == channels * 4);
            bridge->playback(true);
            bridge->capture(true);
            const auto session = bridge->connect(100);
            assert(session);
            assert(bridge->pushPlayback(input.data(), FloatDuplexBridge::packetFrames, 1234, 100));
            auto packet = std::make_unique<FloatDuplexBridge::Packet>();
            assert(bridge->takePlayback(session, 100, *packet));
            assert(packet->time == 1234 && packet->frames == FloatDuplexBridge::packetFrames);
            assert(std::equal(input.begin(), input.end(), packet->samples));
            assert(std::all_of(packet->samples + input.size(),
                               packet->samples + sizeof(packet->samples), [](unsigned char byte) {
                                   return byte == 0;
                               }));
            const auto blocks = (format.prefillFrames() + FloatDuplexBridge::packetFrames - 1) /
                                FloatDuplexBridge::packetFrames;
            for (unsigned i = 0; i < blocks; ++i)
                assert(bridge->pushProcessed(session, 100, input.data(),
                                             FloatDuplexBridge::packetFrames));
            bridge->readRecording(output.data(), FloatDuplexBridge::packetFrames, 100);
            assert(output == input); // Every float bit survives the complete bridge.
            bridge->capture(false);
            assert(
                bridge->pushProcessed(session, 100, input.data(), FloatDuplexBridge::packetFrames));
            assert(bridge->diagnostics().queuedFrames == 0); // No stale closed-stream audio.
            bridge->capture(true);
            bridge->readRecording(output.data(), FloatDuplexBridge::packetFrames, 100);
            assert(std::all_of(output.begin(), output.end(), [](unsigned char b) {
                return b == 0;
            }));
            assert(bridge->diagnostics().underruns == 0); // Same 20 ms priming contract.
            assert(!bridge->pushProcessed(session + 1, 100, input.data(),
                                          FloatDuplexBridge::packetFrames));
            for (unsigned i = 0; i < blocks; ++i)
                assert(bridge->pushProcessed(session, 100, input.data(),
                                             FloatDuplexBridge::packetFrames));
            bridge->readRecording(output.data(), FloatDuplexBridge::packetFrames,
                                  101 + FloatDuplexBridge::leaseDuration);
            assert(std::all_of(output.begin(), output.end(), [](unsigned char b) {
                return b == 0;
            }));
            assert(bridge->diagnostics().leaseSilentFrames == FloatDuplexBridge::packetFrames);
            for (unsigned i = 0; i < guard; ++i) {
                assert(ingress[i] == 211 && ingress[ingress.size() - i - 1] == 211);
                assert(recording[i] == 177 && recording[recording.size() - i - 1] == 177);
            }
        }
    }
}
