#include "../native/tools/recording_latency_analysis.hpp"

#include <cassert>
#include <random>
#include <limits>
#include <vector>

int main() {
    using namespace loopz::probe;
    std::vector<float> reference(60000), recording(60000), unrelated(60000);
    std::mt19937 random(42);
    float history = 0;
    for (auto& sample : reference) {
        history = history * 0.7f + (int(random() % 2001) - 1000) / 1000.f;
        sample = history;
    }
    for (auto& sample : unrelated)
        sample = (int(random() % 2001) - 1000) / 1000.f;
    // Both directions matter: independently opened clients have arbitrary origins.
    for (int lag : {-173, 0, 1197}) {
        std::fill(recording.begin(), recording.end(), 0);
        for (int i = 0; i < int(reference.size()); ++i)
            if (i + lag >= 0 && i + lag < int(recording.size()))
                recording[i + lag] = reference[i] * 0.5f;
        const auto match = matchWaveforms(reference, recording, 12000, 12000, 0, 2400);
        assert(match.lagFrames == lag && match.correlation > 0.99999);
    }
    assert(std::abs(matchWaveforms(reference, unrelated, 12000, 12000, 0, 2400).correlation) < 0.1);
    std::fill(recording.begin(), recording.end(), 0);
    assert(matchWaveforms(reference, recording, 12000, 12000, 0, 2400).correlation == 0);

    const auto waveform = [](double frame) {
        double value = 0;
        for (int tone = 1; tone <= 19; ++tone)
            value += std::sin(frame * (0.007 * tone + 0.000123 * tone * tone));
        return float(value / 19);
    };
    for (size_t i = 0; i < reference.size(); ++i) {
        reference[i] = waveform(i);
        recording[i] = waveform(double(i) - 127.375) * 0.5f;
    }
    const auto fractional = matchWaveforms(reference, recording, 12000, 12000, 0, 2400);
    assert(std::abs(fractional.lagFrames - 127.375) <= 1.0 / 32);
    assert(fractional.correlation > 0.9999);

    // A strong out-of-band tone must not shift a fractional microphone match.
    const auto noisyWave = [&](double frame) {
        return waveform(frame) + float(8 * std::sin(2.8 * frame));
    };
    for (size_t i = 0; i < reference.size(); ++i) {
        reference[i] = noisyWave(i);
        recording[i] = noisyWave(double(i) - 127.375) * 0.5f;
    }
    const auto cleanReference = matchingBand(reference), cleanRecording = matchingBand(recording);
    const auto bandMatch = matchWaveforms(cleanReference, cleanRecording, 12000, 12000, 0, 2400);
    assert(std::abs(bandMatch.lagFrames - 127.375) <= 1.0 / 32);
    assert(bandMatch.correlation > 0.9999);

    // A single decimated winner aliases this band into a false negative lag
    // (-64.09375 instead of +137.40625). Fractionally refine multiple coarse
    // hypotheses; do not relax the actual waveform acceptance threshold.
    for (int seed : {1, 17, 39}) {
        const auto narrowBand = [seed](double frame) {
            double value = 0;
            for (int tone = 0; tone < 13; ++tone) {
                const double hz = 3600 + ((seed * 317 + tone * 241) % 2400);
                value += std::sin(frame * 2 * std::numbers::pi * hz / 48000 + tone * .17);
            }
            return float(value / 13);
        };
        const auto lag = 137.375 + seed * .03125;
        for (size_t i = 0; i < reference.size(); ++i) {
            reference[i] = narrowBand(i);
            recording[i] = narrowBand(double(i) - lag);
        }
        const auto aliasSafe = matchWaveforms(reference, recording, 6000, 12000, 0, 1200);
        assert(std::abs(aliasSafe.lagFrames - lag) <= 1.0 / 16);
        assert(aliasSafe.correlation > .999);
    }

    // Deliberately false virtual acquisition timestamps must not affect arrival age.
    const std::vector<RecordingBlock> physical{{0, 1000000000, 1003000000, 64},
                                               {64, 1001333333, 1004333333, 64}};
    RecordingBlock virtualBlock{200, 99000000000, 1006000000, 32};
    const auto first = deliveredAgeNs(virtualBlock, 0, 200, physical, 48000);
    const auto last = deliveredAgeNs(virtualBlock, 31, 200, physical, 48000);
    assert(std::abs(first - 6000000) < 1);
    assert(std::abs(first - last - 31e9 / 48000) < 1);
    virtualBlock.acquiredNs = 1;
    assert(deliveredAgeNs(virtualBlock, 0, 200, physical, 48000) == first);
    bool rejected = false;
    try {
        deliveredAgeNs(virtualBlock, 32, 200, physical, 48000);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    assert(rejected);
    const auto corrected = hardwareAdjustedDeliveryAgeNs(4445000, 74, 0, 48000);
    assert(std::abs(corrected - (4445000 + 74e9 / 48000)) < 1);
    assert(std::abs(hardwareAdjustedDeliveryAgeNs(4445000, 74, 26, 48000) -
                    (4445000 + 100e9 / 48000)) < 1);
    assert(hardwareAdjustedDeliveryAgeNs(4445000, 0, 0, 48000) == 4445000);
    assert(std::isfinite(hardwareAdjustedDeliveryAgeNs(0, UINT32_MAX, UINT32_MAX, 8000)));
    for (const auto invalidRate : {0.0, 7999.0, 192001.0, std::numeric_limits<double>::infinity(),
                                   std::numeric_limits<double>::quiet_NaN()}) {
        bool invalid = false;
        try {
            hardwareAdjustedDeliveryAgeNs(0, 74, 0, invalidRate);
        } catch (const std::invalid_argument&) {
            invalid = true;
        }
        assert(invalid);
    }
}
