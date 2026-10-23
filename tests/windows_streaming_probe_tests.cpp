#include "../native/platform/windows/probe_streaming_capture.hpp"
#include <atomic>
#include <cassert>
#include <limits>
#include <string_view>
#include <thread>
#include <utility>

using loopz::windows::StreamingProbeCapture;
namespace {
constexpr double tau = 6.283185307179586;
void close(double actual, double expected, double tolerance = 2e-10) {
    assert(std::isfinite(actual) && std::abs(actual - expected) < tolerance);
}
void reference(unsigned rate, unsigned channels, unsigned packetFrames) {
    constexpr unsigned frames = 8203;
    std::vector<float> samples(size_t(frames) * channels);
    for (unsigned f = 0; f < frames; ++f)
        for (unsigned c = 0; c < channels; ++c)
            samples[size_t(f) * channels + c] =
                static_cast<float>(.025 * std::sin(tau * (443 + c * 37) * f / rate + .137 * c));
    StreamingProbeCapture capture(rate, channels);
    for (unsigned offset = 0; offset < frames;) {
        const auto count = std::min(packetFrames, frames - offset);
        assert(capture.push(samples.data() + size_t(offset) * channels, count));
        while (capture.consumeOne()) {
        }
        offset += count;
    }
    capture.finishProducer();
    while (capture.consumeOne()) {
    }
    assert(capture.frames() == frames && capture.acceptedFrames() == frames &&
           !capture.pendingPackets() && !capture.droppedFrames() && !capture.invalidSamples());
    assert(capture.sampleStorageBytes() <= size_t(32) * 4096 * channels * sizeof(float));
    double maximumStep = 0;
    uint64_t zeroFrames = 0, longest = 0, run = 0, longestStart = 0;
    for (unsigned f = 0; f < frames; ++f) {
        bool zero = true;
        for (unsigned c = 0; c < channels; ++c) {
            const double value = samples[size_t(f) * channels + c];
            zero &= value == 0;
            if (f)
                maximumStep =
                    std::max(maximumStep, std::abs(value - samples[size_t(f - 1) * channels + c]));
        }
        if (zero) {
            ++zeroFrames;
            if (++run > longest) {
                longest = run;
                longestStart = f + 1 - run;
            }
        } else
            run = 0;
    }
    assert(capture.zeroFrames() == zeroFrames && capture.longestZeroRun() == longest &&
           capture.longestZeroStart() == longestStart);
    close(capture.maximumStep(), maximumStep, 1e-15);
    for (unsigned c = 0; c < channels; ++c) {
        double real = 0, imaginary = 0, otherReal = 0, otherImaginary = 0, peak = 0;
        for (unsigned f = 0; f < frames; ++f) {
            const double value = samples[size_t(f) * channels + c];
            const double phase = tau * f / rate;
            real += value * std::cos(phase * (443 + c * 37));
            imaginary += value * std::sin(phase * (443 + c * 37));
            otherReal += value * std::cos(phase * (443 + ((c + 1) % channels) * 37));
            otherImaginary += value * std::sin(phase * (443 + ((c + 1) % channels) * 37));
            peak = std::max(peak, std::abs(value));
        }
        const auto measured = capture.spectrum(c);
        assert(measured.frames == frames);
        close(measured.peak, peak, 1e-15);
        close(measured.target, std::hypot(real, imaginary) * 2 / frames);
        close(measured.other, std::hypot(otherReal, otherImaginary) * 2 / frames);
    }
}
void boundaries() {
    StreamingProbeCapture capture(192000, 2);
    std::vector<float> prefix(23 * 2, 1), suffix(17 * 2, .25f);
    assert(capture.push(prefix.data(), 23));
    assert(capture.push(nullptr, 7000, true));
    assert(capture.push(nullptr, 5000, true));
    assert(capture.push(suffix.data(), 17));
    capture.finishProducer();
    while (capture.consumeOne()) {
    }
    assert(capture.frames() == 12040 && capture.zeroFrames() == 12000 &&
           capture.longestZeroRun() == 12000 && capture.longestZeroStart() == 23);
    close(capture.maximumStep(), 1);
    assert(!capture.push(prefix.data(), 1) && capture.droppedFrames() == 1);

    StreamingProbeCapture invalid(48000, 2);
    const std::array<float, 8> values{.125f,
                                      .125f,
                                      std::numeric_limits<float>::quiet_NaN(),
                                      std::numeric_limits<float>::infinity(),
                                      -.125f,
                                      -.125f,
                                      0,
                                      0};
    assert(invalid.push(values.data(), 4));
    invalid.finishProducer();
    while (invalid.consumeOne()) {
    }
    assert(invalid.invalidSamples() == 2 && invalid.frames() == 4 && invalid.zeroFrames() == 2);

    StreamingProbeCapture full(48000, 2);
    const std::array<float, 2> value{.125f, .125f};
    assert(full.push(nullptr, 0));
    for (unsigned i = 0; i < full.slotCount; ++i)
        assert(full.push(value.data(), 1));
    assert(!full.push(nullptr, 4));
    assert(full.consumeOne());
    assert(!full.push(nullptr, 4097, true)); // Only one slot is available.
    assert(!full.push(value.data(), UINT32_MAX));
    full.finishProducer();
    while (full.consumeOne()) {
    }
    assert(full.frames() == full.slotCount && full.droppedFrames() == uint64_t(UINT32_MAX) + 4101);
    close(full.spectrum(0).peak, .125);
}
void wrapAndConcurrency() {
    constexpr unsigned channels = 64, rate = 192000, packet = 193;
    constexpr uint64_t attempts = 1500;
    StreamingProbeCapture capture(rate, channels);
    std::vector<float> samples(size_t(packet) * channels, .125f);
    std::thread consumer([&] {
        for (;;) {
            if (capture.consumeOne())
                continue;
            if (capture.producerFinished()) {
                while (capture.consumeOne()) {
                }
                break;
            }
            std::this_thread::yield();
        }
    });
    uint64_t accepted = 0;
    std::vector<float> acceptedValues;
    acceptedValues.reserve(attempts);
    for (uint64_t i = 0; i < attempts; ++i) {
        const float value = .125f + .0001f * float(i % 17);
        std::fill(samples.begin(), samples.end(), value);
        if (capture.push(samples.data(), packet)) {
            accepted += packet;
            acceptedValues.push_back(value);
        }
        // Pace the test without blocking inside capture.push(). Overflow remains
        // an explicit result; no packet is retried to conceal a dropped attempt.
        if (i % 16 == 0)
            std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    capture.finishProducer();
    consumer.join();
    assert(accepted && capture.frames() == accepted && capture.acceptedFrames() == accepted &&
           !capture.pendingPackets());
    assert(capture.droppedFrames() == attempts * packet - accepted);
    assert(!capture.invalidSamples() && !capture.zeroFrames());
    double expectedStep = 0;
    for (size_t i = 1; i < acceptedValues.size(); ++i)
        expectedStep =
            std::max(expectedStep, std::abs(double(acceptedValues[i]) - acceptedValues[i - 1]));
    close(capture.maximumStep(), expectedStep, 1e-15);
    const auto referenceAmplitude = [&](unsigned hz) {
        const double omega = tau * hz / rate;
        const double sum = std::sin(packet * omega / 2) / std::sin(omega / 2);
        double real = 0, imaginary = 0;
        for (size_t i = 0; i < acceptedValues.size(); ++i) {
            const double center = double(i * packet) + (packet - 1) * .5;
            real += acceptedValues[i] * sum * std::cos(omega * center);
            imaginary += acceptedValues[i] * sum * std::sin(omega * center);
        }
        return std::hypot(real, imaginary) * 2 / double(accepted);
    };
    for (unsigned c = 0; c < channels; ++c) {
        close(capture.spectrum(c).target, referenceAmplitude(443 + c * 37));
        close(capture.spectrum(c).other, referenceAmplitude(443 + ((c + 1) % channels) * 37));
        close(capture.spectrum(c).peak,
              *std::max_element(acceptedValues.begin(), acceptedValues.end()), 1e-15);
    }
    // A separate deterministic run exceeds the complete queue's frame capacity.
    StreamingProbeCapture wrapped(rate, 2);
    std::vector<float> flat(997 * 2, .125f);
    for (unsigned i = 0; i < 200; ++i) {
        assert(wrapped.push(flat.data(), 997));
        while (wrapped.consumeOne()) {
        }
    }
    wrapped.finishProducer();
    while (wrapped.consumeOne()) {
    }
    assert(wrapped.frames() == 199400 && wrapped.frames() > uint64_t(32) * 4096);
}
void longWindow() {
    constexpr unsigned rate = 192000, channels = 2, seconds = 300;
    // Independently calculated one-second table has an integer number of cycles.
    // Repeating it validates the entire supported five-minute phase/count range
    // without retaining that recording or waiting five minutes in a unit test.
    std::vector<float> period(size_t(rate) * channels);
    for (unsigned f = 0; f < rate; ++f)
        for (unsigned c = 0; c < channels; ++c)
            period[size_t(f) * channels + c] =
                static_cast<float>(.025 * std::sin(tau * (443 + c * 37) * f / rate));
    StreamingProbeCapture capture(rate, channels);
    for (unsigned second = 0; second < seconds; ++second)
        for (unsigned offset = 0; offset < rate;) {
            const auto count = std::min(4096u, rate - offset);
            assert(capture.push(period.data() + size_t(offset) * channels, count));
            while (capture.consumeOne()) {
            }
            offset += count;
        }
    capture.finishProducer();
    while (capture.consumeOne()) {
    }
    assert(capture.frames() == uint64_t(rate) * seconds && !capture.droppedFrames() &&
           !capture.invalidSamples());
    assert(capture.zeroFrames() == seconds && capture.longestZeroRun() == 1);
    assert(capture.maximumStep() < .05 * std::sin(tau * .5 * 480 / rate) + 1e-8);
    for (unsigned c = 0; c < channels; ++c) {
        close(capture.spectrum(c).target, .025, 2e-8);
        close(capture.spectrum(c).other, 0, 2e-8);
    }
}
} // namespace
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--long-window") {
        longWindow();
        return 0;
    }
    assert(argc == 1);
    for (unsigned rate : {44100u, 48000u, 88200u, 96000u, 176400u, 192000u})
        for (unsigned channels : {2u, 18u, 64u})
            for (unsigned packet : {63u, 511u, 4097u})
                reference(rate, channels, packet);
    boundaries();
    wrapAndConcurrency();
    for (auto [rate, channels] : {std::pair{0u, 2u}, {48000u, 0u}, {48000u, 3u}, {48000u, 66u}}) {
        bool rejected = false;
        try {
            StreamingProbeCapture invalid(rate, channels);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        assert(rejected);
    }
}
