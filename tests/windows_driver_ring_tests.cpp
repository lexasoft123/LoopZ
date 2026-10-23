#include "../native/platform/windows/driver/frame_clock.hpp"
#include "../native/platform/windows/driver/endpoint_reference.hpp"
#include <string>
#include "../native/platform/windows/driver/oem_format.hpp"
#include "../native/platform/windows/driver/bridge_ring.hpp"
#include "../native/platform/windows/driver/duplex_bridge.hpp"
#include "../native/platform/windows/playback_clock.hpp"
#include <array>
#include <algorithm>
#include <cassert>
#include <iostream>
#include <random>
#include <deque>
#include <vector>
using loopz_driver::BridgeRing;
void configuredRingReference() {
    constexpr unsigned guard = 31, maximumRead = 777;
    for (unsigned rate : {44100u, 48000u, 88200u, 96000u, 176400u, 192000u})
        for (unsigned channels : {2u, 18u, 64u}) {
            const loopz_driver::AudioFormat format{rate, channels};
            const auto width = format.frameBytes(), capacity = format.ringFrames();
            std::vector<unsigned char> storage(format.ringBytes() + guard * 2, 209),
                input((capacity + 17) * width), output(maximumRead * width + guard * 2);
            for (size_t index = 0; index < input.size(); ++index)
                input[index] = static_cast<unsigned char>(index * 13 + index / width + 11);
            BridgeRing ring;
            assert(ring.configure(format, storage.data() + guard, format.ringBytes()));
            ring.reset(true);
            ring.capture(true);
            std::deque<std::array<unsigned char, 128>> reference;
            std::mt19937 random(rate + channels);
            bool primed = false;
            uint64_t missing = 0, dropped = 0;
            for (unsigned iteration = 0; iteration < 240; ++iteration) {
                if (iteration == 100) {
                    ring.capture(false);
                    ring.write(input.data(), 997);
                    assert(ring.queued() == 0);
                    ring.capture(true);
                    reference.clear();
                    primed = false;
                }
                if (iteration == 7 || random() & 1) {
                    const unsigned frames = iteration == 7 ? capacity + 17 : random() % 998;
                    ring.write(input.data(), frames);
                    const auto accepted = std::min(frames, capacity);
                    const auto first = frames - accepted;
                    if (reference.size() + accepted > capacity)
                        dropped += reference.size() + accepted - capacity;
                    for (unsigned frame = first; frame < frames; ++frame) {
                        std::array<unsigned char, 128> value{};
                        std::copy_n(input.data() + frame * width, width, value.begin());
                        reference.push_back(value);
                    }
                    while (reference.size() > capacity)
                        reference.pop_front();
                } else {
                    const unsigned frames = random() % (maximumRead + 1);
                    std::fill(output.begin(), output.end(), static_cast<unsigned char>(167));
                    if (!primed && reference.size() >= format.prefillFrames())
                        primed = true;
                    const auto available = primed ? std::min<size_t>(frames, reference.size()) : 0;
                    ring.read(output.data() + guard, frames);
                    for (unsigned frame = 0; frame < frames; ++frame) {
                        for (unsigned byte = 0; byte < width; ++byte)
                            assert(output[guard + frame * width + byte] ==
                                   (frame < available ? reference.front()[byte] : 0));
                        if (frame < available)
                            reference.pop_front();
                    }
                    if (available < frames) {
                        missing += frames - available;
                        primed = false;
                    }
                    assert(std::all_of(output.begin(), output.begin() + guard, [](auto value) {
                        return value == 167;
                    }));
                    assert(std::all_of(output.begin() + guard + frames * width, output.end(),
                                       [](auto value) {
                                           return value == 167;
                                       }));
                }
                assert(ring.queued() == reference.size());
                assert(ring.missing() == missing && ring.dropped() == dropped);
            }
            for (unsigned index = 0; index < guard; ++index) {
                assert(storage[index] == 209);
                assert(storage[guard + format.ringBytes() + index] == 209);
            }
        }
}
void splitPlaybackClockQuantization() {
    for (unsigned rate : {44100u, 48000u, 88200u, 96000u, 176400u, 192000u}) {
        unsigned falseResets = 0;
        for (unsigned firstFrame : {0u, 1u, 47u, 1237u}) {
            loopz::windows::PlaybackClock exact(rate), doubleRounded(rate);
            constexpr uint64_t anchor = 300000000;
            uint64_t total = 0;
            std::mt19937 random(rate + firstFrame);
            for (unsigned block = 0; block < 10000; ++block) {
                const unsigned frames = 1 + random() % 480;
                const auto roundedStart =
                    anchor + loopz_driver::FrameClock::timeForFrames(firstFrame + total, rate);
                for (unsigned offset = 0; offset < frames;) {
                    const unsigned count =
                        std::min(frames - offset, loopz_driver::FloatDuplexBridge::packetFrames);
                    const auto tick = anchor + loopz_driver::FrameClock::timeForFrames(
                                                   firstFrame + total + offset, rate);
                    const auto stamp = exact.append(tick, count, false);
                    assert(stamp.valid && !stamp.discontinuity);
                    assert(stamp.sourceFrame == total + offset);
                    // Reproduce the old float bridge's separately rounded offset.
                    const auto old = doubleRounded.append(
                        roundedStart + loopz_driver::FrameClock::timeForFrames(offset, rate), count,
                        false);
                    assert(old.valid);
                    falseResets += old.discontinuity;
                    offset += count;
                }
                total += frames;
            }
        }
        if (rate == 44100 || rate == 88200 || rate == 176400)
            assert(falseResets > 0); // Negative control detects the quantization bug.
        std::cout << "rate=" << rate << " old-double-round-resets=" << falseResets
                  << " corrected-resets=0\n";
    }
}
void playbackClockQuantization() {
    for (unsigned rate : {44100u, 48000u, 88200u, 96000u, 176400u, 192000u}) {
        for (unsigned firstFrame : {0u, 1u, 47u, 1237u}) {
            loopz::windows::PlaybackClock clock(rate);
            const uint64_t origin = 300000000;
            const auto firstTick =
                origin + loopz_driver::FrameClock::timeForFrames(firstFrame, rate);
            std::mt19937 random(rate + firstFrame);
            uint64_t total = 0;
            for (unsigned packet = 0; packet < 10000; ++packet) {
                const unsigned frames = 1 + random() % 480;
                const auto tick =
                    origin + loopz_driver::FrameClock::timeForFrames(firstFrame + total, rate);
                const auto stamp = clock.append(tick, frames, false);
                assert(stamp.valid && !stamp.discontinuity && stamp.sourceFrame == total);
                assert(stamp.hostNs == firstTick * 100 + total * 1000000000ULL / rate);
                const auto observed = tick * 100;
                const auto difference =
                    observed > stamp.hostNs ? observed - stamp.hostNs : stamp.hostNs - observed;
                assert(difference <= 100);
                total += frames;
            }
            const auto jump =
                origin + loopz_driver::FrameClock::timeForFrames(firstFrame + total, rate) + 1000;
            auto stamp = clock.append(jump, 17, false);
            assert(stamp.valid && stamp.discontinuity && stamp.sourceFrame == 0 &&
                   stamp.hostNs == jump * 100);
            const auto next =
                origin + loopz_driver::FrameClock::timeForFrames(firstFrame + total + 17, rate) +
                1000;
            stamp = clock.append(next, 13, false);
            assert(stamp.valid && !stamp.discontinuity && stamp.sourceFrame == 17);
            stamp = clock.append(next + 1000, 2, true);
            assert(stamp.valid && stamp.discontinuity && stamp.sourceFrame == 0);
            clock.reset();
            stamp = clock.append(firstTick, 2, false);
            assert(stamp.valid && !stamp.discontinuity && stamp.sourceFrame == 0);
            assert(!clock.append(0, 1, false).valid);
            assert(!clock.append(firstTick, 0, false).valid);
            assert(!clock.append(firstTick, 481, false).valid);
            assert(!clock.append(UINT64_MAX, 1, false).valid);
        }
    }
    assert(!loopz::windows::PlaybackClock(48001).append(100, 1, false).valid);
}
int main() {
    splitPlaybackClockQuantization();
    configuredRingReference();
    {
        std::array<wchar_t, 192> first{}, second{}, capture{};
        using loopz_driver::endpointReference;
        assert(endpointReference(L"ROOT\\LOOPZAUDIO\\LZ_abcdef", L"WaveSpeaker", first.data(),
                                 static_cast<unsigned>(first.size())));
        assert(std::wstring(first.data()) == L"LoopZ-WaveSpeaker-LZ_ABCDEF");
        assert(endpointReference(L"root\\loopzaudio\\lz_ABCDEF", L"WaveSpeaker", second.data(),
                                 static_cast<unsigned>(second.size())));
        assert(first == second);
        assert(endpointReference(L"ROOT\\LOOPZAUDIO\\LZ_abcdee", L"WaveSpeaker", second.data(),
                                 static_cast<unsigned>(second.size())));
        assert(first != second);
        assert(endpointReference(L"ROOT\\LOOPZAUDIO\\LZ_abcdef", L"WaveMicArray1", capture.data(),
                                 static_cast<unsigned>(capture.size())));
        assert(first != capture);
        assert(endpointReference(L"ROOT\\LOOPZAUDIO\\0000", L"TopologySpeaker", second.data(),
                                 static_cast<unsigned>(second.size())));
        for (const auto invalid : {L"", L"ROOT\\OTHER\\0000", L"ROOT\\LOOPZAUDIO\\",
                                   L"ROOT\\LOOPZAUDIO\\a-b", L"ROOT\\LOOPZAUDIO\\a\\b"})
            assert(!endpointReference(invalid, L"WaveSpeaker", second.data(),
                                      static_cast<unsigned>(second.size())));
        assert(!endpointReference(nullptr, L"WaveSpeaker", second.data(),
                                  static_cast<unsigned>(second.size())));
        assert(!endpointReference(L"ROOT\\LOOPZAUDIO\\0000", L"bad/name", second.data(),
                                  static_cast<unsigned>(second.size())));
        assert(!endpointReference(L"ROOT\\LOOPZAUDIO\\0000", L"", second.data(),
                                  static_cast<unsigned>(second.size())));
        const auto length = static_cast<unsigned>(std::wstring(first.data()).size());
        std::array<wchar_t, 192> bounded{};
        bounded.fill(L'!');
        assert(!endpointReference(L"ROOT\\LOOPZAUDIO\\LZ_abcdef", L"WaveSpeaker", bounded.data(),
                                  length));
        assert(bounded[0] == 0 && bounded[length] == L'!');
        assert(endpointReference(L"ROOT\\LOOPZAUDIO\\LZ_abcdef", L"WaveSpeaker", bounded.data(),
                                 length + 1));
        assert(bounded[length] == 0 && bounded[length + 1] == L'!');
        const std::wstring longInstance = L"ROOT\\LOOPZAUDIO\\" + std::wstring(128, L'x');
        assert(!endpointReference(longInstance.c_str(), L"WaveSpeaker", second.data(),
                                  static_cast<unsigned>(second.size())));
        std::array<wchar_t, 32> unterminatedFilter{};
        unterminatedFilter.fill(L'x');
        assert(!endpointReference(L"ROOT\\LOOPZAUDIO\\0000", unterminatedFilter.data(),
                                  second.data(), static_cast<unsigned>(second.size())));
    }

    playbackClockQuantization();
    loopz_driver::OemFormatProperty property;
    assert(property.build({48000, 2}));
    const std::array<unsigned char, 48> expectedFormat{
        0x41, 0,    0, 0, 0x28, 0, 0,    0, 0xfe, 0xff, 2,  0,    0x80, 0xbb, 0,    0,
        0,    0xee, 2, 0, 4,    0, 16,   0, 22,   0,    16, 0,    3,    0,    0,    0,
        1,    0,    0, 0, 0,    0, 0x10, 0, 0x80, 0,    0,  0xaa, 0,    0x38, 0x9b, 0x71};
    assert(std::equal(expectedFormat.begin(), expectedFormat.end(), property.data));
    const auto word = [](const unsigned char* b) {
        return unsigned(b[0]) | (unsigned(b[1]) << 8);
    };
    const auto dword = [&](const unsigned char* b) {
        return word(b) | (word(b + 2) << 16);
    };
    for (unsigned rate : {44100u, 48000u, 88200u, 96000u, 176400u, 192000u}) {
        for (unsigned channels = 2; channels <= 64; channels += 2) {
            const loopz_driver::AudioFormat format{rate, channels};
            assert(property.build(format, 4));
            assert(word(property.data + 20) == channels * 4);
            assert(word(property.data + 22) == 32 && word(property.data + 26) == 32);
            assert(dword(property.data + 16) == rate * channels * 4);
            assert(dword(property.data + 32) == 3);
            assert(format.acceptsNotificationBuffer(format.ringFrames() * channels * 4, 1, 4));
            assert(!format.acceptsNotificationBuffer(channels * 4 - 2, 1, 4));
            assert(format.acceptsAllocatedBuffer(format.ringFrames() * channels * 4,
                                                 format.ringFrames() * channels * 4, 4));
            assert(!format.acceptsAllocatedBuffer(1, 1, 3));
            // Real shared-mode setup uses two 10 ms periods. At 48 kHz,
            // 22 channels exceeded the old fixed stereo buffer limit.
            const auto periodFrames = (rate + 99) / 100;
            const auto normalBuffer = periodFrames * 2 * format.frameBytes();
            assert(format.acceptsNotificationBuffer(normalBuffer, 2));
            assert(format.acceptsAllocatedBuffer(normalBuffer, normalBuffer));
            assert(format.acceptsAllocatedBuffer(normalBuffer, normalBuffer + 4095));
            assert(!format.acceptsAllocatedBuffer(normalBuffer, normalBuffer - 1));
            assert(!format.acceptsAllocatedBuffer(normalBuffer, 0));
            assert(!format.acceptsAllocatedBuffer(0, normalBuffer));
            assert(!format.acceptsAllocatedBuffer(format.ringBytes() + 1, 0xffffffffu));
            assert(format.acceptsNotificationBuffer(format.ringBytes(), 1));
            assert(!format.acceptsNotificationBuffer(format.ringBytes() + format.frameBytes(), 1));
            assert(!format.acceptsNotificationBuffer(normalBuffer + 1, 2));
            assert(!format.acceptsNotificationBuffer(normalBuffer, 0));
            assert(!format.acceptsNotificationBuffer(0, 2));
            assert(property.build({rate, channels}));
            assert(dword(property.data) == 65 && dword(property.data + 4) == 40);
            assert(word(property.data + 8) == 0xfffe && word(property.data + 10) == channels);
            assert(dword(property.data + 12) == rate);
            assert(dword(property.data + 16) == rate * channels * 2);
            assert(word(property.data + 20) == channels * 2);
            assert(word(property.data + 22) == 16 && word(property.data + 24) == 22);
            assert(word(property.data + 26) == 16);
            assert(dword(property.data + 28) == (channels == 2 ? 3u : 0u));
            assert(
                std::equal(expectedFormat.begin() + 32, expectedFormat.end(), property.data + 32));
        }
    }
    assert(!property.build({48000, 3}));
    assert((!loopz_driver::AudioFormat{0, 2}.acceptsNotificationBuffer(4, 1)));
    assert((!loopz_driver::AudioFormat{0, 2}.acceptsAllocatedBuffer(4, 4096)));
    assert(std::all_of(std::begin(property.data), std::end(property.data), [](auto b) {
        return b == 0;
    }));
    for (unsigned rate : {44100u, 48000u, 88200u, 96000u, 176400u, 192000u}) {
        loopz_driver::FrameClock varied;
        assert(varied.configure(rate));
        unsigned long long elapsed = 0, frames = 0;
        unsigned long long notifiedPackets = 0;
        bool observedBetweenNotifications = false;
        const unsigned packetFrames = rate / 100, frameBytes = 64;
        const unsigned dmaBytes = 2 * packetFrames * frameBytes;
        std::mt19937 variedRng(rate);
        for (unsigned i = 0; i < 100000; ++i) {
            const auto delta = 1u + variedRng() % 17777;
            elapsed += delta;
            frames += varied.advance(delta);
            assert(frames == elapsed * rate / 10000000);
            const auto packets =
                loopz_driver::FrameClock::completedPackets(frames * frameBytes, dmaBytes, 2);
            assert(packets == elapsed / 100000); // Exactly completed 10 ms packets.
            if (i % 11 == 0)
                notifiedPackets = packets;
            else if (packets > notifiedPackets)
                observedBetweenNotifications = true;
            assert(varied.fractionalTime() < (10000000u + rate - 1) / rate);
        }
        assert(observedBetweenNotifications); // Position queries can outrun notifications.
        assert(loopz_driver::FrameClock::completedPackets(
                   ((1ULL << 32) * packetFrames + 1) * frameBytes, dmaBytes, 2) == (1ULL << 32));
        const auto yearsFrames = 3ULL * 365 * 86400 * rate;
        assert(loopz_driver::FrameClock::timeForFrames(yearsFrames, rate) ==
               3ULL * 365 * 86400 * 10000000);
        assert(!varied.configure(0));
        assert(!varied.configure(384000));
        varied.reset();
        assert(varied.advance(10000000) == rate);
    }
    // Eight-millisecond endpoint periods round to whole frames, not whole
    // milliseconds. Check the instant before/at each actual completion for the
    // fractional-rate packets as well as exact 48/96/192 kHz periods.
    for (unsigned rate : {44100u, 48000u, 88200u, 96000u, 176400u, 192000u}) {
        const unsigned packetFrames = (rate * 8 + 999) / 1000;
        const unsigned frameBytes = 4;
        const unsigned dmaBytes = packetFrames * frameBytes * 2;
        for (unsigned packet = 1; packet <= 1000; ++packet) {
            const auto totalFrames = static_cast<unsigned long long>(packet) * packetFrames;
            const auto boundary = (totalFrames * 10000000 + rate - 1) / rate;
            loopz_driver::FrameClock fractional;
            assert(fractional.configure(rate));
            auto frames = fractional.advance(boundary - 1);
            assert(loopz_driver::FrameClock::completedPackets(frames * frameBytes, dmaBytes, 2) ==
                   packet - 1);
            frames += fractional.advance(1);
            assert(loopz_driver::FrameClock::completedPackets(frames * frameBytes, dmaBytes, 2) ==
                   packet);
        }
    }
    assert(loopz_driver::FrameClock::completedPackets(1, 0, 2) == 0);
    assert(loopz_driver::FrameClock::completedPackets(1, 4, 0) == 0);
    assert(loopz_driver::FrameClock::completedPackets(1, 5, 2) == 0);
    loopz_driver::FrameClock clock;
    unsigned long long totalTime = 0, totalFrames = 0;
    std::mt19937 timeRng(722);
    for (unsigned i = 0; i < 100000; ++i) {
        const unsigned delta = 1 + timeRng() % 17777;
        totalTime += delta;
        totalFrames += clock.advance(delta);
        assert(totalFrames == totalTime * 48000 / 10000000);
        assert(clock.fractionalTime() < 209);
    }
    const auto years = 3ULL * 365 * 86400 * 10000000 + 1234567;
    assert(loopz_driver::FrameClock::timeForFrames(0) == 0);
    assert(loopz_driver::FrameClock::timeForFrames(1) == 208);
    assert(loopz_driver::FrameClock::timeForFrames(48000) == 10000000);
    assert(loopz_driver::FrameClock::timeForFrames(3ULL * 365 * 86400 * 48000) ==
           3ULL * 365 * 86400 * 10000000);
    assert(loopz_driver::FrameClock::toQpc(years, 10000000) == years);
    assert(loopz_driver::FrameClock::toQpc(1000000000000000ULL, 24000000) == 2400000000000000ULL);
    clock.reset();
    assert(clock.advance(10000000) == 48000);
    clock.reset();
    assert(clock.advance(100) == 0);
    assert(clock.advance(109) == 1);

    BridgeRing ring;
    std::array<unsigned char, BridgeRing::capacity * 8> input{};
    std::array<unsigned char, BridgeRing::capacity * 4> output{};
    for (unsigned i = 0; i < input.size(); ++i)
        input[i] = static_cast<unsigned char>(i * 17 + 3);
    ring.reset(true);
    ring.write(input.data(), 1200);
    assert(ring.queued() == 0); // no historical audio while capture is closed
    ring.capture(true);
    ring.write(input.data(), 959);
    ring.read(output.data(), 48);
    for (unsigned i = 0; i < 192; ++i)
        assert(output[i] == 0);
    assert(ring.queued() == 959);
    ring.write(input.data() + 959 * 4, 1);
    ring.read(output.data(), 960);
    for (unsigned i = 0; i < 960 * 4; ++i)
        assert(output[i] == input[i]);
    ring.reset(false);
    ring.read(output.data(), 960);
    for (unsigned i = 0; i < 960 * 4; ++i)
        assert(output[i] == 0);
    ring.reset(true);
    ring.write(input.data(), BridgeRing::capacity * 2);
    assert(ring.queued() == BridgeRing::capacity);
    ring.read(output.data(), BridgeRing::capacity);
    for (unsigned i = 0; i < output.size(); ++i)
        assert(output[i] == input[BridgeRing::capacity * 4 + i]);
    // Exercise wrap, overflow and varying callback sizes against an independent FIFO.
    ring.reset(true);
    std::deque<unsigned char> expected;
    std::mt19937 rng(1234);
    bool primed = false;
    for (unsigned iteration = 0; iteration < 10000; ++iteration) {
        const unsigned frames = 1 + rng() % 1700;
        if (rng() & 1) {
            ring.write(input.data(), frames);
            expected.insert(expected.end(), input.begin(), input.begin() + frames * 4);
            while (expected.size() > BridgeRing::capacity * 4)
                expected.pop_front();
        } else {
            if (!primed && expected.size() >= BridgeRing::prefill * 4)
                primed = true;
            const unsigned available =
                primed ? std::min(frames * 4, static_cast<unsigned>(expected.size())) : 0;
            ring.read(output.data(), frames);
            for (unsigned i = 0; i < frames * 4; ++i) {
                if (i < available) {
                    assert(output[i] == expected.front());
                    expected.pop_front();
                } else
                    assert(output[i] == 0);
            }
            if (available < frames * 4)
                primed = false;
        }
        assert(ring.queued() * 4 == expected.size());
    }
    ring.capture(false);
    ring.write(input.data(), 1200);
    assert(ring.queued() == 0);
    assert(ring.missing() > 0);
    std::cout << "Windows driver FIFO lifecycle and randomized wrap checks passed\n";
}
