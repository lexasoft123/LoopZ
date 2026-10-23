#include <loopz/audio_meter.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    using namespace loopz;
    if (argc == 2 && std::string_view(argv[1]) == "--benchmark") {
        std::vector<std::vector<float>> data(64, std::vector<float>(256));
        std::array<const float*, 64> planes{};
        for (uint32_t c = 0; c < 64; ++c) {
            planes[c] = data[c].data();
            for (uint32_t f = 0; f < 256; ++f)
                data[c][f] = .05f * std::sin(static_cast<float>(c + f) * .03f);
        }
        AudioMeter meter(64);
        for (unsigned i = 0; i < 100; ++i) meter.publish({planes, 256}, 256);
        const auto begin = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < 4000; ++i) meter.publish({planes, 256}, 256);
        const auto end = std::chrono::steady_clock::now();
        AudioMeterSnapshot snapshot;
        assert(meter.read(snapshot) && snapshot.frames == 256);
        std::printf("{\"channels\":64,\"frames\":256,\"calls\":4000,\"nsPerCall\":%.1f}\n",
            std::chrono::duration<double, std::nano>(end - begin).count() / 4000);
        return 0;
    }
    const std::array<uint32_t, 16> patterns{0, 0x80000000, 0x3d4ccccd, 0xbd4ccccd,
        0x7f800000, 0xff800000, 0x7fc12345, 0x7f812345, 1, 0x80000001,
        0x7f7fffff, 0xff7fffff, 0x00800000, 0x80800000, 0x3f800000, 0xbf800000};
    unsigned cases = 0;
    for (uint32_t channels : {1U, 2U, 8U, 17U, 64U})
        for (uint32_t frames : {1U, 2U, 3U, 4U, 7U, 63U, 128U, 255U, 256U, 1025U})
            for (unsigned mode = 0; mode < 4; ++mode)
                for (unsigned offset : {0U, 1U}) {
                    AudioMeter meter(channels);
                    std::vector<std::vector<float>> data(channels, std::vector<float>(frames + offset));
                    std::vector<const float*> planes(channels);
                    for (uint32_t c = 0; c < channels; ++c) {
                        planes[c] = data[c].data() + offset;
                        for (uint32_t f = 0; f < frames; ++f) {
                            const auto bits = patterns[(f + c) % patterns.size()];
                            data[c][offset + f] = mode == 0 ? std::bit_cast<float>(bits)
                                : mode == 1 ? .05f * std::sin(static_cast<float>(f * 3 + c) * .017f)
                                : mode == 2 ? std::numeric_limits<float>::quiet_NaN() : 0;
                        }
                    }
                    meter.publish({planes, frames}, frames);
                    AudioMeterSnapshot snapshot;
                    assert(meter.read(snapshot) && snapshot.frames == frames && snapshot.channels == channels);
                    for (uint32_t c = 0; c < channels; ++c) {
                        float peak = 0;
                        double energy = 0;
                        for (uint32_t f = 0; f < frames; ++f) {
                            const float v = std::isfinite(planes[c][f]) ? planes[c][f] : 0;
                            peak = std::max(peak, std::fabs(v));
                            energy += static_cast<double>(v) * v;
                        }
                        const float rms = static_cast<float>(std::sqrt(energy / frames));
                        assert(std::bit_cast<uint32_t>(snapshot.peak[c]) == std::bit_cast<uint32_t>(peak));
                        assert(std::isfinite(snapshot.rms[c]));
                        assert(std::fabs(static_cast<double>(snapshot.rms[c]) - rms) <=
                            std::max(static_cast<double>(std::numeric_limits<float>::denorm_min()),
                                     static_cast<double>(rms) * 2e-7));
                    }
                    if (channels > 1) {
                        planes.resize(1);
                        meter.publishPartial({planes, frames}, frames);
                        assert(meter.read(snapshot));
                        for (uint32_t c = 1; c < channels; ++c)
                            assert(snapshot.peak[c] == 0 && snapshot.rms[c] == 0);
                    }
                    meter.publish({planes, frames}, 0);
                    assert(meter.read(snapshot) && snapshot.frames == 0);
                    ++cases;
                }
    std::printf("Audio meters: %u scalar-reference cases passed\n", cases);
}
