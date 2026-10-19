#pragma once
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace loopz::probe {
// A time-coded, channel-specific marker. The code changes every 10 ms and
// never repeats on the time scale of this test. Its clock is Mach host time.
inline double marker(uint64_t ns, unsigned channel) noexcept {
    uint64_t x = ns / 10000000 + 0x9e3779b97f4a7c15ULL * (channel + 1);
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return ((x ^ (x >> 31)) & 1) ? 0.03 : -0.03;
}
struct Point {
    uint64_t ns{};
    double sample[2]{};
};
struct Window {
    std::array<Point, 1000> points{};
    double real[2]{}, imag[2]{}, crossReal[2]{}, crossImag[2]{};
    uint64_t frames{};
};
// Single callback producer; control-thread consumer. Four seconds of fixed
// storage, no callback allocation and no raw recording written to disk.
class StreamCheck {
  public:
    double rate{48000}, frequencies[2]{1100, 2200};
    std::atomic<uint64_t> timestampErrors{0}, queueOverruns{0};
    uint64_t windows{0}, badWindows{0};
    double minimumAmplitude{1}, maximumCross{0}, minimumCorrelation{1};
    double minimumDelayMs{1000}, maximumDelayMs{0};
    void append(const float* left, const float* right, uint32_t frames, uint64_t ns,
                double sampleTime, bool valid) noexcept {
        if (!valid || (havePrevious_ && std::fabs(sampleTime - expectedSample_) > 0.5))
            timestampErrors.fetch_add(1);
        expectedSample_ = sampleTime + frames;
        havePrevious_ = true;
        for (uint32_t i = 0; i < frames; ++i, ++seen_) {
            if (seen_ < static_cast<uint64_t>(rate))
                continue;
            if (write_.load(std::memory_order_relaxed) - read_.load(std::memory_order_acquire) >=
                queue_.size()) {
                queueOverruns.fetch_add(1);
                continue;
            }
            auto& window = queue_[write_.load(std::memory_order_relaxed) % queue_.size()];
            const uint64_t time = ns + static_cast<uint64_t>((i + 0.5) * 1e9 / rate);
            const double value[2]{left[i], right[i]};
            for (unsigned c = 0; c < 2; ++c) {
                const double angle = 2 * M_PI * frequencies[c] * sampleTime / rate +
                                     2 * M_PI * frequencies[c] * i / rate;
                const double other = 2 * M_PI * frequencies[1 - c] * sampleTime / rate +
                                     2 * M_PI * frequencies[1 - c] * i / rate;
                window.real[c] += value[c] * std::cos(angle);
                window.imag[c] += value[c] * std::sin(angle);
                window.crossReal[c] += value[c] * std::cos(other);
                window.crossImag[c] += value[c] * std::sin(other);
                sums_[c] += value[c];
            }
            ++window.frames;
            if (!pointFrames_)
                firstPointNs_ = time;
            if (++pointFrames_ >= static_cast<uint32_t>(std::llround(rate / 1000))) {
                window.points[point_++] = {firstPointNs_ + (time - firstPointNs_) / 2,
                                           {sums_[0] / pointFrames_, sums_[1] / pointFrames_}};
                pointFrames_ = 0;
                sums_[0] = sums_[1] = 0;
                if (point_ == window.points.size()) {
                    point_ = 0;
                    write_.fetch_add(1, std::memory_order_release);
                }
            }
        }
    }
    void consume() noexcept {
        while (read_.load(std::memory_order_relaxed) < write_.load(std::memory_order_acquire)) {
            auto& window = queue_[read_.load(std::memory_order_relaxed) % queue_.size()];
            bool good = true;
            for (unsigned c = 0; c < 2; ++c) {
                const double amplitude =
                    2 * std::hypot(window.real[c], window.imag[c]) / window.frames;
                const double cross =
                    2 * std::hypot(window.crossReal[c], window.crossImag[c]) / window.frames;
                minimumAmplitude = std::min(minimumAmplitude, amplitude);
                maximumCross = std::max(maximumCross, cross);
                good &= amplitude > 0.07 && amplitude < 0.13 && cross < 0.01;
                double best = -1;
                unsigned delay = 0;
                for (unsigned ms = 0; ms <= 200; ++ms) {
                    double dot = 0, signal = 0, reference = 0;
                    for (const auto& point : window.points) {
                        const auto expected = marker(point.ns - ms * 1000000ULL, c);
                        dot += point.sample[c] * expected;
                        signal += point.sample[c] * point.sample[c];
                        reference += expected * expected;
                    }
                    const double correlation = signal > 0 ? dot / std::sqrt(signal * reference) : 0;
                    if (correlation > best) {
                        best = correlation;
                        delay = ms;
                    }
                }
                minimumCorrelation = std::min(minimumCorrelation, best);
                minimumDelayMs = std::min(minimumDelayMs, static_cast<double>(delay));
                maximumDelayMs = std::max(maximumDelayMs, static_cast<double>(delay));
                good &= best > 0.75 && delay < 150;
            }
            ++windows;
            if (!good)
                ++badWindows;
            window = Window{};
            read_.fetch_add(1, std::memory_order_release);
        }
    }

  private:
    std::array<Window, 4> queue_{};
    std::atomic<uint64_t> write_{0}, read_{0};
    uint64_t seen_{0}, firstPointNs_{0};
    uint32_t point_{0}, pointFrames_{0};
    double sums_[2]{}, expectedSample_{0};
    bool havePrevious_{false};
};
} // namespace loopz::probe
