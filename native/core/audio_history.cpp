#include <loopz/audio_history.hpp>
#include <loopz/timeline.hpp>
#include <loopz/audio_resampler.hpp>
#include <loopz/sample_clock.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace loopz {
namespace {
bool validRate(double value) noexcept {
    return std::isfinite(value) && value >= 8000 && value <= 192000;
}
bool validMeasuredRate(double value) noexcept {
    return std::isfinite(value) && value >= 8000 * 0.995 && value <= 192000 * 1.005;
}
void silence(RoutingOutput output, uint32_t frames) noexcept {
    for (auto* plane : output.channels)
        if (plane)
            std::fill_n(plane, std::min(frames, output.capacityFrames), 0.0f);
}
struct FrameTag {
    std::atomic<uint64_t> frame{UINT64_MAX}, revision{0};
    std::atomic<uint32_t> available{0};
};
struct ReadDiagnostics {
    std::atomic<uint64_t> reads{0}, requestedFrames{0}, publishedEnd{0};
    std::array<std::atomic<uint64_t>, static_cast<size_t>(AudioHistoryMissingReason::Count)>
        missingFrames{};
    std::atomic<uint64_t> unpublishedSampleReads{0}, expiredSampleReads{0}, racingSampleReads{0},
        absentSampleReads{0};

    void begin(uint32_t frames) noexcept {
        reads.fetch_add(1, std::memory_order_relaxed);
        requestedFrames.fetch_add(frames, std::memory_order_relaxed);
    }
    void missing(AudioHistoryMissingReason reason, uint32_t frames) noexcept {
        missingFrames[static_cast<size_t>(reason)].fetch_add(frames, std::memory_order_relaxed);
    }
    void unavailable(uint64_t position, bool racing, uint32_t capacity) noexcept {
        const auto end = publishedEnd.load(std::memory_order_relaxed);
        auto& counter = racing                      ? racingSampleReads
                        : position >= end           ? unpublishedSampleReads
                        : end - position > capacity ? expiredSampleReads
                                                    : absentSampleReads;
        counter.fetch_add(1, std::memory_order_relaxed);
    }
};
} // namespace
struct AudioHistory::Impl {
    const uint32_t channels, capacity;
    const AudioResampler resampler;
    const AudioResampler linear;
    const ResamplingFilter filter;
    SampleClockHistory clock;
    std::unique_ptr<FrameTag[]> tags;
    std::unique_ptr<std::atomic<uint32_t>[]> samples;
    const std::unique_ptr<ReadDiagnostics> diagnostics;
    std::atomic<bool> online{false};
    std::atomic<uint64_t> version{0}, generation{1}, anchorFrame{0}, anchorNs{0}, rateBits{0};
    std::atomic<uint64_t> nominalBits{0};
    std::atomic<uint32_t> anchorFrames{0}, available{0};
    std::atomic<uint64_t> callbacks{0}, discontinuities{0};
    uint64_t cursor{0}, originNs{0}, originFrame{0}, previousNs{0};
    uint64_t recentNs{0}, recentFrame{0};
    double previousRate{0}, previousSource{0};
    uint32_t previousFrames{0};
    bool previousSourceValid{false};
    Impl(uint32_t count, uint32_t frames, ResamplingFilter filter, bool readDiagnostics)
        : channels(count), capacity(frames), resampler(filter), filter(filter), clock(frames + 1),
          tags(std::make_unique<FrameTag[]>(frames)),
          samples(std::make_unique<std::atomic<uint32_t>[]>(static_cast<size_t>(count) * frames)),
          diagnostics(readDiagnostics ? std::make_unique<ReadDiagnostics>() : nullptr) {}
    ResampleFrame frame(uint64_t position, std::span<float> destination) const noexcept {
        const auto slot = position % capacity;
        const auto& tag = tags[slot];
        const auto revision = tag.revision.load();
        if ((revision & 1) || tag.frame.load() != position) {
            if (diagnostics)
                diagnostics->unavailable(position, revision & 1, capacity);
            return {false, 0};
        }
        const auto frameAvailable = tag.available.load();
        for (uint32_t channel = 0; channel < destination.size(); ++channel)
            destination[channel] = std::bit_cast<float>(samples[slot * channels + channel].load());
        const bool valid = tag.frame.load() == position && tag.revision.load() == revision;
        if (!valid && diagnostics)
            diagnostics->unavailable(position, true, capacity);
        return {valid, frameAvailable};
    }
    static ResampleFrame readFrame(const void* context, uint64_t position,
                                   std::span<float> values) noexcept {
        return static_cast<const Impl*>(context)->frame(position, values);
    }
};
AudioHistory::AudioHistory(uint32_t channels, uint32_t capacityFrames)
    : AudioHistory(channels, capacityFrames, ResamplingFilter::LinearPhase) {}
AudioHistory::AudioHistory(uint32_t channels, uint32_t capacityFrames, ResamplingFilter filter,
                           bool readDiagnostics) {
    if (!channels || channels > kMaximumRoutingChannels || capacityFrames < kBlock * 2 ||
        capacityFrames > kCapacity)
        throw std::invalid_argument("Invalid audio history capacity");
    impl_ = std::make_unique<Impl>(channels, capacityFrames, filter, readDiagnostics);
}
AudioHistory::~AudioHistory() = default;
uint32_t AudioHistory::channels() const noexcept {
    return impl_->channels;
}
bool AudioHistory::append(RoutingInput input, uint32_t frames, uint64_t firstHostNs,
                          double nominalRate, double sourceFrame, bool sourceFrameValid) noexcept {
    auto& self = *impl_;
    if (!frames || frames > self.capacity || frames > input.capacityFrames ||
        input.channels.empty() || input.channels.size() > channels() || !firstHostNs ||
        firstHostNs > UINT64_MAX - 10000000000ULL || !validRate(nominalRate) ||
        (sourceFrameValid && !std::isfinite(sourceFrame)) ||
        std::any_of(input.channels.begin(), input.channels.end(), [](auto* plane) {
            return !plane;
        }))
        return false;
    if (self.cursor > UINT64_MAX - self.capacity - frames)
        return false;
    self.version.fetch_add(1);
    bool discontinuity = false;
    if (self.previousNs) {
        const auto expected = self.previousNs + frameNs(self.previousFrames, self.previousRate);
        const auto gap = firstHostNs >= expected ? firstHostNs - expected : expected - firstHostNs;
        discontinuity = firstHostNs <= self.previousNs || gap > 2000000 ||
                        nominalRate != self.previousRate ||
                        input.channels.size() != self.available.load() ||
                        (sourceFrameValid && self.previousSourceValid &&
                         std::fabs(sourceFrame - self.previousSource - self.previousFrames) > 0.5);
    }
    if (discontinuity) {
        self.generation.fetch_add(1);
        self.clock.reset();
        self.cursor += self.capacity;
        self.originNs = firstHostNs;
        self.originFrame = self.cursor;
        self.recentNs = firstHostNs;
        self.recentFrame = self.cursor;
        self.discontinuities.fetch_add(1);
    }
    if (!self.originNs) {
        self.originNs = firstHostNs;
        self.originFrame = self.cursor;
        self.recentNs = firstHostNs;
        self.recentFrame = self.cursor;
    }
    // Alternate half-second anchors so rate estimates cover the most recent
    // 0.5–1 second, rather than averaging over the entire capture lifetime.
    // Constant work and storage; the producer never walks callback history.
    constexpr uint64_t windowNs = 500000000;
    if (firstHostNs - self.recentNs >= windowNs) {
        if (self.recentNs != self.originNs) {
            self.originNs = self.recentNs;
            self.originFrame = self.recentFrame;
        }
        self.recentNs = firstHostNs;
        self.recentFrame = self.cursor;
    }
    double measuredRate = nominalRate;
    if (firstHostNs - self.originNs >= windowNs) {
        measuredRate = static_cast<double>(self.cursor - self.originFrame) * 1e9 /
                       static_cast<double>(firstHostNs - self.originNs);
        if (measuredRate < nominalRate * 0.995 || measuredRate > nominalRate * 1.005) {
            measuredRate = nominalRate;
            self.originNs = firstHostNs;
            self.originFrame = self.cursor;
            self.recentNs = firstHostNs;
            self.recentFrame = self.cursor;
        }
    }
    for (uint32_t index = 0; index < frames; ++index) {
        const auto position = self.cursor + index;
        const auto slot = position % self.capacity;
        auto& tag = self.tags[slot];
        tag.revision.fetch_add(1);
        tag.frame.store(UINT64_MAX);
        for (uint32_t channel = 0; channel < channels(); ++channel) {
            const float value =
                channel < input.channels.size() ? input.channels[channel][index] : 0;
            self.samples[slot * channels() + channel].store(
                std::bit_cast<uint32_t>(std::isfinite(value) ? value : 0));
        }
        tag.available.store(static_cast<uint32_t>(input.channels.size()));
        tag.frame.store(position);
        tag.revision.fetch_add(1);
    }
    const bool contiguous = !discontinuity && sourceFrameValid && self.previousSourceValid;
    if (!self.clock.append(self.cursor, firstHostNs, contiguous)) {
        self.online.store(false);
        self.version.fetch_add(1);
        return false;
    }
    self.previousNs = firstHostNs;
    self.previousFrames = frames;
    self.previousRate = nominalRate;
    self.previousSource = sourceFrame;
    self.previousSourceValid = sourceFrameValid;
    self.anchorFrame.store(self.cursor);
    self.anchorNs.store(firstHostNs);
    self.rateBits.store(std::bit_cast<uint64_t>(measuredRate));
    self.nominalBits.store(std::bit_cast<uint64_t>(nominalRate));
    self.anchorFrames.store(frames);
    self.available.store(static_cast<uint32_t>(input.channels.size()));
    if (self.diagnostics)
        self.diagnostics->publishedEnd.store(self.cursor + frames, std::memory_order_relaxed);
    self.version.fetch_add(1);
    self.cursor += frames;
    self.callbacks.fetch_add(1);
    self.online.store(true);
    return true;
}
void AudioHistory::stop() noexcept {
    auto& self = *impl_;
    self.online.store(false);
    self.version.fetch_add(1);
    self.generation.fetch_add(1);
    self.clock.reset();
    self.anchorNs.store(0);
    self.available.store(0);
    self.originNs = self.previousNs = 0;
    self.recentNs = self.recentFrame = 0;
    self.previousSourceValid = false;
    // Reserve a disjoint timestamp region even for a provider that restarts
    // at the same sample position. Old tags cannot match the new generation.
    if (self.cursor <= UINT64_MAX - self.capacity)
        self.cursor += self.capacity;
    self.version.fetch_add(1);
}
AudioHistoryStatus AudioHistory::status() const noexcept {
    const auto& self = *impl_;
    return {self.online.load(),          self.available.load(),
            self.anchorNs.load(),        self.callbacks.load(),
            self.discontinuities.load(), std::bit_cast<double>(self.rateBits.load())};
}
AudioHistoryReadSnapshot AudioHistory::readDiagnostics() const noexcept {
    const auto* counters = impl_->diagnostics.get();
    if (!counters)
        return {};
    AudioHistoryReadSnapshot result;
    result.enabled = true;
    result.reads = counters->reads.load(std::memory_order_relaxed);
    result.requestedFrames = counters->requestedFrames.load(std::memory_order_relaxed);
    for (size_t index = 0; index < result.missingFrames.size(); ++index)
        result.missingFrames[index] =
            counters->missingFrames[index].load(std::memory_order_relaxed);
    result.unpublishedSampleReads =
        counters->unpublishedSampleReads.load(std::memory_order_relaxed);
    result.expiredSampleReads = counters->expiredSampleReads.load(std::memory_order_relaxed);
    result.racingSampleReads = counters->racingSampleReads.load(std::memory_order_relaxed);
    result.absentSampleReads = counters->absentSampleReads.load(std::memory_order_relaxed);
    return result;
}
AudioReadResult AudioHistory::read(uint64_t firstHostNs, double rate, RoutingOutput output,
                                   uint32_t frames, AudioReadCursor& cursor) const noexcept {
    return readWithCoverage(firstHostNs, rate, output, frames, cursor, {});
}
AudioReadResult AudioHistory::readWithCoverage(uint64_t firstHostNs, double rate,
                                               RoutingOutput output, uint32_t frames,
                                               AudioReadCursor& cursor,
                                               std::span<uint8_t> availableFrames) const noexcept {
    return readUsing(firstHostNs, rate, output, frames, cursor, availableFrames, impl_->resampler);
}
AudioReadResult AudioHistory::readWithFilter(uint64_t firstHostNs, double rate,
                                             RoutingOutput output, uint32_t frames,
                                             AudioReadCursor& cursor,
                                             ResamplingFilter filter) const noexcept {
    if (filter == ResamplingFilter::LinearPhase)
        return readUsing(firstHostNs, rate, output, frames, cursor, {}, impl_->linear);
    if (filter == ResamplingFilter::MinimumPhase && impl_->filter == filter)
        return readUsing(firstHostNs, rate, output, frames, cursor, {}, impl_->resampler);
    silence(output, frames);
    if (impl_->diagnostics) {
        impl_->diagnostics->begin(frames);
        impl_->diagnostics->missing(AudioHistoryMissingReason::InvalidRequest, frames);
    }
    return {frames, false};
}
AudioReadResult AudioHistory::readUsing(uint64_t firstHostNs, double rate, RoutingOutput output,
                                        uint32_t frames, AudioReadCursor& cursor,
                                        std::span<uint8_t> availableFrames,
                                        const AudioResampler& resampler) const noexcept {
    const auto& self = *impl_;
    if (self.diagnostics)
        self.diagnostics->begin(frames);
    const auto failed = [&](AudioHistoryMissingReason reason, bool online) {
        if (self.diagnostics)
            self.diagnostics->missing(reason, frames);
        return AudioReadResult{frames, online};
    };
    silence(output, frames);
    std::fill_n(availableFrames.begin(), std::min<size_t>(frames, availableFrames.size()),
                uint8_t{0});
    if (!frames || frames > kBlock || frames > output.capacityFrames || output.channels.empty() ||
        (!availableFrames.empty() && availableFrames.size() < frames) ||
        output.channels.size() > channels() || !validMeasuredRate(rate) ||
        firstHostNs > UINT64_MAX - 10000000000ULL ||
        std::any_of(output.channels.begin(), output.channels.end(), [](auto* plane) {
            return !plane;
        }))
        return failed(AudioHistoryMissingReason::InvalidRequest, false);
    if (!self.online.load())
        return failed(AudioHistoryMissingReason::Offline, false);
    const auto generation = self.generation.load();
    AudioReadCursor anchor;
    bool coherent = false;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        const auto version = self.version.load();
        if (!version || (version & 1))
            continue;
        anchor = {self.anchorFrame.load(), self.anchorNs.load(), generation,
                  std::bit_cast<double>(self.rateBits.load()), self.anchorFrames.load()};
        if (version == self.version.load() && self.generation.load() == generation) {
            coherent = true;
            break;
        }
    }
    if (!coherent) {
        if (!cursor.hostNs || cursor.generation != generation)
            return failed(AudioHistoryMissingReason::AnchorUnavailable, true);
        anchor = cursor;
    }
    if (!anchor.hostNs || !validMeasuredRate(anchor.rate))
        return failed(AudioHistoryMissingReason::AnchorUnavailable, false);
    cursor = anchor;
    const auto clock = self.clock.snapshot();
    const auto nominal = std::bit_cast<double>(self.nominalBits.load());
    const auto through =
        firstHostNs + static_cast<uint64_t>(static_cast<long double>(frames - 1) * 1e9L / rate);
    std::array<float*, kMaximumRoutingChannels> planes{};
    uint32_t missing = 0, clockMissing = 0;
    for (uint32_t offset = 0; offset < frames;) {
        // Preserve the fractional output grid instead of rounding each split
        // to another whole nanosecond. Absolute timestamps remain integers.
        const long double elapsed = static_cast<long double>(offset) * 1e9L / rate;
        const auto whole = static_cast<uint64_t>(elapsed);
        const auto ns = firstHostNs + whole;
        const auto fraction = elapsed - whole;
        const auto point = self.clock.locate(clock, ns, fraction, nominal, anchor.rate, through);
        if (!point.valid) {
            ++missing;
            ++clockMissing;
            ++offset;
            continue;
        }
        auto count = frames - offset;
        if (point.endNs != UINT64_MAX) {
            const long double remaining =
                (static_cast<long double>(point.endNs - ns) - fraction) * rate / 1e9L;
            if (remaining < count)
                count = std::max(1U, static_cast<uint32_t>(std::ceil(remaining)));
        }
        for (size_t channel = 0; channel < output.channels.size(); ++channel)
            planes[channel] = output.channels[channel] + offset;
        missing += resampler.read(
            {point.frame, point.offset, point.rate, rate, point.localClockCorrection, nominal},
            {{planes.data(), output.channels.size()}, output.capacityFrames - offset}, count,
            Impl::readFrame, &self,
            availableFrames.empty() ? std::span<uint8_t>{}
                                    : availableFrames.subspan(offset, count));
        offset += count;
    }
    if (self.generation.load() != generation || !self.online.load()) {
        silence(output, frames);
        std::fill_n(availableFrames.begin(), std::min<size_t>(frames, availableFrames.size()),
                    uint8_t{0});
        return failed(AudioHistoryMissingReason::GenerationChanged, false);
    }
    if (self.diagnostics) {
        self.diagnostics->missing(AudioHistoryMissingReason::ClockMapping, clockMissing);
        self.diagnostics->missing(AudioHistoryMissingReason::ResamplingSupport,
                                  missing - clockMissing);
    }
    return {missing, true};
}
} // namespace loopz
