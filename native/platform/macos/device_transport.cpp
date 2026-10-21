#include <loopz/sample_rates.hpp>
#include <loopz/macos/device_transport.hpp>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <sys/mman.h>
#include <unistd.h>

namespace loopz::macos {
namespace {
constexpr uint64_t kMagic = 0x4c5a444556303032;
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<uint64_t>) == sizeof(uint64_t));
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t));

struct alignas(16) FrameTag {
    std::atomic<uint64_t> frame{UINT64_MAX};
    std::atomic<uint64_t> revision{0};
};
static_assert(sizeof(FrameTag) == 16);
static_assert(sizeof(DeviceAudioHeader) == 128);
static_assert(offsetof(DeviceAudioHeader, helperHeartbeatNs) == 48);
static_assert(offsetof(DeviceAudioHeader, captureReadFrames) == 72);
static_assert(offsetof(DeviceAudioHeader, mappingIdentityHigh) == 112);

struct Layout {
    size_t captureTags, captureSamples, playbackTags, playbackSamples, bytes;
};

Layout layout(uint32_t channels) {
    if (channels < 2 || channels > kDeviceAudioMaximumChannels || channels % 2)
        throw std::invalid_argument("Device transport requires 2..64 channels in pairs");
    const size_t tags = sizeof(FrameTag) * kDeviceAudioCapacity;
    const size_t samples = sizeof(std::atomic<uint32_t>) * kDeviceAudioCapacity * channels;
    const size_t captureTags = sizeof(DeviceAudioHeader);
    const size_t captureSamples = captureTags + tags;
    const size_t playbackTags = captureSamples + samples;
    const size_t playbackSamples = playbackTags + tags;
    const auto bytes = (playbackSamples + samples + kDeviceAudioMappingAlignment - 1) /
                       kDeviceAudioMappingAlignment * kDeviceAudioMappingAlignment;
    return {captureTags, captureSamples, playbackTags, playbackSamples, bytes};
}

template <typename T> T* at(void* mapping, size_t offset) noexcept {
    return reinterpret_cast<T*>(static_cast<std::byte*>(mapping) + offset);
}

bool validBlock(uint64_t firstFrame, size_t sampleCapacity, uint32_t frames,
                uint32_t channels) noexcept {
    return frames > 0 && frames <= kDeviceAudioCapacity && firstFrame <= UINT64_MAX - frames &&
           sampleCapacity >= static_cast<size_t>(frames) * channels;
}

bool alive(const std::atomic<uint32_t>& online, const std::atomic<uint64_t>& heartbeat,
           uint64_t now, uint64_t skew) noexcept {
    if (!online.load())
        return false;
    const auto last = heartbeat.load();
    return last != 0 &&
           (now >= last ? now - last < kDeviceAudioHeartbeatTimeoutNs
                        : last - now <= std::min(skew, kDeviceAudioConcurrentHeartbeatSkewNs));
}
} // namespace

DeviceAudioView::DeviceAudioView(void* mapping, uint32_t channels, uint32_t sampleRate,
                                 uint64_t generation, size_t captureTags, size_t captureSamples,
                                 size_t playbackTags, size_t playbackSamples) noexcept
    : mapping_(static_cast<std::byte*>(mapping)), channels_(channels), sampleRate_(sampleRate),
      generation_(generation), captureTags_(captureTags), captureSamples_(captureSamples),
      playbackTags_(playbackTags), playbackSamples_(playbackSamples) {}

DeviceAudioView DeviceAudioView::attach(void* mapping, size_t bytes) {
    if (!mapping || reinterpret_cast<uintptr_t>(mapping) % alignof(DeviceAudioHeader) ||
        bytes < sizeof(DeviceAudioHeader))
        throw std::invalid_argument("Invalid device audio mapping address or size");
    const auto& header = *static_cast<DeviceAudioHeader*>(mapping);
    if (header.magic != kMagic || header.protocol != kDeviceAudioProtocol ||
        header.headerBytes != sizeof(DeviceAudioHeader) || header.mappingBytes != bytes ||
        !header.generation || header.capacityFrames != kDeviceAudioCapacity || header.reserved ||
        header.retired.load() > 1 || (!header.mappingIdentityHigh && !header.mappingIdentityLow) ||
        !supportedVirtualSampleRate(header.sampleRate))
        throw std::invalid_argument("Device audio mapping schema mismatch");
    const auto offsets = layout(header.channels);
    if (offsets.bytes != bytes)
        throw std::invalid_argument("Device audio mapping layout mismatch");
    return {mapping,
            header.channels,
            header.sampleRate,
            header.generation,
            offsets.captureTags,
            offsets.captureSamples,
            offsets.playbackTags,
            offsets.playbackSamples};
}

DeviceAudioHeader& DeviceAudioView::header() const noexcept {
    return *reinterpret_cast<DeviceAudioHeader*>(mapping_);
}
uint32_t DeviceAudioView::channels() const noexcept {
    return channels_;
}
uint32_t DeviceAudioView::sampleRate() const noexcept {
    return sampleRate_;
}
uint64_t DeviceAudioView::generation() const noexcept {
    return generation_;
}
std::array<uint64_t, 2> DeviceAudioView::mappingIdentity() const noexcept {
    return {header().mappingIdentityHigh, header().mappingIdentityLow};
}

bool DeviceAudioView::write(bool capture, uint64_t firstFrame, std::span<const float> samples,
                            uint32_t frames) const noexcept {
    if (!validBlock(firstFrame, samples.size(), frames, channels_) || header().retired.load())
        return false;
    auto* tags = at<FrameTag>(mapping_, capture ? captureTags_ : playbackTags_);
    auto* payload =
        at<std::atomic<uint32_t>>(mapping_, capture ? captureSamples_ : playbackSamples_);
    for (uint32_t index = 0; index < frames; ++index) {
        const auto frame = firstFrame + index;
        const auto slot = frame % kDeviceAudioCapacity;
        auto& tag = tags[slot];
        tag.revision.fetch_add(1);
        tag.frame.store(UINT64_MAX);
        for (uint32_t channel = 0; channel < channels_; ++channel) {
            const float value = samples[static_cast<size_t>(index) * channels_ + channel];
            payload[slot * channels_ + channel].store(
                std::bit_cast<uint32_t>(std::isfinite(value) ? value : 0.0f));
        }
        tag.frame.store(frame);
        tag.revision.fetch_add(1);
    }
    return true;
}

DeviceAudioTransfer DeviceAudioView::read(bool capture, uint64_t firstFrame,
                                          std::span<float> samples, uint32_t frames, uint64_t nowNs,
                                          uint64_t concurrentHeartbeatSkewNs) const noexcept {
    if (!validBlock(firstFrame, samples.size(), frames, channels_)) {
        std::fill(samples.begin(), samples.end(), 0);
        return {false, frames};
    }
    auto& state = header();
    const bool available =
        !state.retired.load() && (capture ? alive(state.helperOnline, state.helperHeartbeatNs,
                                                  nowNs, concurrentHeartbeatSkewNs)
                                          : alive(state.driverOnline, state.driverHeartbeatNs,
                                                  nowNs, concurrentHeartbeatSkewNs));
    auto* tags = at<FrameTag>(mapping_, capture ? captureTags_ : playbackTags_);
    auto* payload =
        at<std::atomic<uint32_t>>(mapping_, capture ? captureSamples_ : playbackSamples_);
    uint32_t missing = 0;
    for (uint32_t index = 0; index < frames; ++index) {
        auto destination = samples.subspan(static_cast<size_t>(index) * channels_, channels_);
        const auto frame = firstFrame + index;
        const auto slot = frame % kDeviceAudioCapacity;
        const auto& tag = tags[slot];
        const auto revision = tag.revision.load();
        bool valid = available && !(revision & 1) && tag.frame.load() == frame;
        if (valid) {
            for (uint32_t channel = 0; channel < channels_; ++channel)
                destination[channel] =
                    std::bit_cast<float>(payload[slot * channels_ + channel].load());
            valid = tag.frame.load() == frame && tag.revision.load() == revision;
        }
        if (!valid) {
            std::fill(destination.begin(), destination.end(), 0);
            ++missing;
        }
    }
    if (capture) {
        state.captureReadFrames.fetch_add(frames);
        state.captureMissingFrames.fetch_add(missing);
    } else {
        state.playbackReadFrames.fetch_add(frames);
        state.playbackMissingFrames.fetch_add(missing);
    }
    return {true, missing};
}

bool DeviceAudioView::writeCapture(uint64_t firstFrame, std::span<const float> samples,
                                   uint32_t frames) const noexcept {
    return write(true, firstFrame, samples, frames);
}
bool DeviceAudioView::writePlayback(uint64_t firstFrame, std::span<const float> samples,
                                    uint32_t frames) const noexcept {
    return write(false, firstFrame, samples, frames);
}
DeviceAudioTransfer
DeviceAudioView::readCapture(uint64_t firstFrame, std::span<float> samples, uint32_t frames,
                             uint64_t nowNs, uint64_t concurrentHeartbeatSkewNs) const noexcept {
    return read(true, firstFrame, samples, frames, nowNs, concurrentHeartbeatSkewNs);
}
DeviceAudioTransfer
DeviceAudioView::readPlayback(uint64_t firstFrame, std::span<float> samples, uint32_t frames,
                              uint64_t nowNs, uint64_t concurrentHeartbeatSkewNs) const noexcept {
    return read(false, firstFrame, samples, frames, nowNs, concurrentHeartbeatSkewNs);
}

DeviceAudioRegion::DeviceAudioRegion(void* mapping, size_t bytes) noexcept
    : mapping_(mapping), bytes_(bytes) {}
DeviceAudioRegion::~DeviceAudioRegion() {
    if (mapping_) {
        retire();
        ::munmap(mapping_, bytes_);
    }
}
std::shared_ptr<DeviceAudioRegion> DeviceAudioRegion::create(uint32_t channels, uint32_t sampleRate,
                                                             uint64_t generation) {
    const auto offsets = layout(channels);
    if (!supportedVirtualSampleRate(sampleRate) || !generation)
        throw std::invalid_argument("Invalid device transport rate or generation");
    // XPC maps whole VM pages. Align the advertised length so exports map back
    // to the exact negotiated size, including across current Darwin page sizes.
    const auto pageSize = ::sysconf(_SC_PAGESIZE);
    if (pageSize <= 0 || kDeviceAudioMappingAlignment % static_cast<size_t>(pageSize))
        throw std::runtime_error("Unsupported VM page size for device audio transport");
    std::array<uint64_t, 2> identity{};
    ::arc4random_buf(identity.data(), sizeof(identity));
    if (!identity[0] && !identity[1])
        throw std::runtime_error("Could not create a device mapping identity");
    auto result = std::shared_ptr<DeviceAudioRegion>(new DeviceAudioRegion(nullptr, 0));
    auto* mapping =
        ::mmap(nullptr, offsets.bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED)
        throw std::system_error(errno, std::generic_category(), "Allocate device audio mapping");
    result->mapping_ = mapping;
    result->bytes_ = offsets.bytes;
    auto* header = std::construct_at(static_cast<DeviceAudioHeader*>(mapping));
    header->magic = kMagic;
    header->protocol = kDeviceAudioProtocol;
    header->headerBytes = sizeof(DeviceAudioHeader);
    header->mappingBytes = offsets.bytes;
    header->generation = generation;
    header->channels = channels;
    header->sampleRate = sampleRate;
    header->capacityFrames = kDeviceAudioCapacity;
    header->reserved = 0;
    header->mappingIdentityHigh = identity[0];
    header->mappingIdentityLow = identity[1];
    for (const auto [tagsOffset, samplesOffset] :
         {std::pair{offsets.captureTags, offsets.captureSamples},
          std::pair{offsets.playbackTags, offsets.playbackSamples}}) {
        // Non-allocating placement array new establishes array lifetimes for
        // indexed access. The mapping already owns all storage.
        ::new (at<FrameTag>(mapping, tagsOffset)) FrameTag[kDeviceAudioCapacity];
        ::new (at<std::atomic<uint32_t>>(mapping, samplesOffset))
            std::atomic<uint32_t>[ static_cast<size_t>(kDeviceAudioCapacity) * channels ];
    }
    return result;
}
DeviceAudioView DeviceAudioRegion::view() const {
    return DeviceAudioView::attach(mapping_, bytes_);
}
DeviceAudioHeader& DeviceAudioRegion::header() const noexcept {
    return *static_cast<DeviceAudioHeader*>(mapping_);
}
void DeviceAudioRegion::retire() noexcept {
    auto& state = header();
    state.retired.store(1);
    state.helperOnline.store(0);
    state.driverOnline.store(0);
}
void* DeviceAudioRegion::data() const noexcept {
    return mapping_;
}
size_t DeviceAudioRegion::bytes() const noexcept {
    return bytes_;
}
} // namespace loopz::macos
