#include <loopz/causal_resampler.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace loopz {
namespace {
bool scaledCeiling(uint64_t frame, uint32_t to, uint32_t from, uint64_t& result) noexcept {
    const auto whole = frame / from;
    if (whole > UINT64_MAX / to)
        return false;
    const auto product = (frame % from) * to;
    const auto tail = product / from + (product % from != 0);
    const auto base = whole * to;
    if (base > UINT64_MAX - tail)
        return false;
    result = base + tail;
    return true;
}
} // namespace

uint32_t CausalResampler::delayFrames(uint32_t sourceRate, uint32_t outputRate) noexcept {
    if (sourceRate < 8000 || sourceRate > 192000 || outputRate < 8000 || outputRate > 192000)
        return 0;
    if (sourceRate == outputRate)
        return 1;
    const auto cutoff = SincKernel::kCutoff * std::min(1.0, double(outputRate) / sourceRate);
    return static_cast<uint32_t>(std::ceil(SincKernel::kRadius / cutoff)) + 1;
}

struct CausalResampler::Impl {
    const uint32_t channels, sourceRate, outputRate, maximumInput, delay;
    const uint32_t capacity, padding;
    const AudioResampler reader;
    std::vector<float> history, converted;
    std::array<float*, kMaximumRoutingChannels> planes{};
    uint64_t streamStart{0}, begin{0}, end{0}, expected{0};
    bool continuous{false};
    Impl(uint32_t count, uint32_t source, uint32_t output, uint32_t maximum)
        : channels(count), sourceRate(source), outputRate(output), maximumInput(maximum),
          delay(CausalResampler::delayFrames(source, output)),
          capacity(std::bit_ceil(2 * delay + maximum + 4)), padding(3 * delay + maximum + 4),
          history(static_cast<size_t>(capacity) * count),
          converted(static_cast<size_t>((uint64_t(maximum) * output + source - 1) / source + 1) *
                    count) {}

    static ResampleFrame readFrame(const void* context, uint64_t position,
                                   std::span<float> destination) noexcept {
        const auto& self = *static_cast<const Impl*>(context);
        if (position < self.padding) {
            std::fill(destination.begin(), destination.end(), 0);
            return {true, self.channels};
        }
        if (position < self.begin || position >= self.end)
            return {false, 0};
        const auto slot = position & (self.capacity - 1);
        std::copy_n(self.history.data() + slot * self.channels, destination.size(),
                    destination.data());
        return {true, self.channels};
    }
};

CausalResampler::CausalResampler(uint32_t channels, uint32_t sourceRate, uint32_t outputRate,
                                 uint32_t maximumInputFrames) {
    if (!channels || channels > kMaximumRoutingChannels || !maximumInputFrames ||
        maximumInputFrames > kBlock || !delayFrames(sourceRate, outputRate))
        throw std::invalid_argument("Unsupported causal resampler format or block size");
    impl_ = std::make_unique<Impl>(channels, sourceRate, outputRate, maximumInputFrames);
}
CausalResampler::~CausalResampler() = default;
uint32_t CausalResampler::sourceRate() const noexcept {
    return impl_->sourceRate;
}
uint32_t CausalResampler::delayFrames() const noexcept {
    return impl_->delay;
}
void CausalResampler::reset() noexcept {
    impl_->continuous = false;
}

CausalResampleBlock CausalResampler::convert(uint64_t firstFrame, RoutingInput input,
                                             uint32_t frames) noexcept {
    auto& self = *impl_;
    if (!frames || frames > self.maximumInput || frames > input.capacityFrames ||
        input.channels.size() != self.channels || firstFrame > UINT64_MAX - self.padding - frames ||
        std::any_of(input.channels.begin(), input.channels.end(), [](auto* plane) {
            return !plane;
        })) {
        reset();
        return {};
    }
    uint64_t firstOutput = 0, endOutput = 0;
    if (!scaledCeiling(firstFrame, self.outputRate, self.sourceRate, firstOutput) ||
        !scaledCeiling(firstFrame + frames, self.outputRate, self.sourceRate, endOutput) ||
        endOutput - firstOutput > self.converted.size() / self.channels) {
        reset();
        return {};
    }
    if (!self.continuous || self.expected != firstFrame) {
        self.streamStart = firstFrame;
        self.begin = self.end = self.padding;
    }
    const auto start = self.padding + firstFrame - self.streamStart;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        const auto slot = (start + frame) & (self.capacity - 1);
        for (uint32_t channel = 0; channel < self.channels; ++channel) {
            const auto value = input.channels[channel][frame];
            self.history[slot * self.channels + channel] = std::isfinite(value) ? value : 0;
        }
    }
    self.end = start + frames;
    self.begin = self.end - std::min<uint64_t>(self.end - self.padding, self.capacity);
    const auto count = static_cast<uint32_t>(endOutput - firstOutput);
    if (self.sourceRate == self.outputRate) {
        // Equal clocks need the same declared one-sample causal delay, not a
        // filter workspace and per-frame provider calls. Copy the already
        // sanitized interleaved history; retain zero padding after any reset.
        const auto first = start - self.delay;
        const uint32_t prefix = first < self.padding ? 1 : 0;
        std::fill_n(self.converted.data(), prefix * self.channels, 0.f);
        const auto offset = (first + prefix) & (self.capacity - 1);
        const auto remaining = count - prefix;
        const auto tail = std::min<uint64_t>(remaining, self.capacity - offset);
        std::copy_n(self.history.data() + offset * self.channels, tail * self.channels,
                    self.converted.data() + prefix * self.channels);
        std::copy_n(self.history.data(), (remaining - tail) * self.channels,
                    self.converted.data() + (prefix + tail) * self.channels);
        self.expected = firstFrame + frames;
        self.continuous = true;
        return {true,
                firstOutput,
                count,
                {self.converted.data(), static_cast<size_t>(count) * self.channels}};
    }
    for (uint32_t offset = 0; offset < count; offset += AudioResampler::kMaximumFrames) {
        const auto length = std::min(count - offset, AudioResampler::kMaximumFrames);
        const auto endpoint = firstOutput + offset;
        const auto product = (endpoint % self.outputRate) * self.sourceRate;
        const auto integral =
            (endpoint / self.outputRate) * self.sourceRate + product / self.outputRate;
        const auto anchor = self.padding + integral - self.streamStart;
        const long double fraction =
            static_cast<long double>(product % self.outputRate) / self.outputRate;
        for (uint32_t channel = 0; channel < self.channels; ++channel)
            self.planes[channel] =
                self.converted.data() + static_cast<size_t>(offset) * self.channels + channel;
        const auto missing = self.reader.read(
            {anchor, fraction - self.delay, double(self.sourceRate), double(self.outputRate)},
            {{self.planes.data(), self.channels}, length, self.channels}, length, Impl::readFrame,
            &self);
        if (missing) {
            reset();
            return {};
        }
    }
    self.expected = firstFrame + frames;
    self.continuous = true;
    return {true,
            firstOutput,
            count,
            {self.converted.data(), static_cast<size_t>(count) * self.channels}};
}
} // namespace loopz
