#include "state.hpp"
#include <loopz/timeline.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace loopz::macos::hal {

Text::Text(const std::string& value)
    : value_(CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(value.data()),
                                     value.size(), kCFStringEncodingUTF8, false)) {
    if (!value_)
        throw std::invalid_argument("Device text must be valid UTF-8");
}
Text::~Text() {
    CFRelease(value_);
}
CFStringRef Text::get() const noexcept {
    return value_;
}

Runtime::Runtime(RemoteDeviceTransport mapping, mach_timebase_info_data_t clockTimebase,
                 std::shared_ptr<DeviceLifecycle> group)
    : transport(std::move(mapping)), audio(transport.view()), timebase(clockTimebase),
      lifecycle(group ? std::move(group) : std::make_shared<DeviceLifecycle>()) {
    anchorTicks.store(mach_absolute_time());
}
bool Runtime::restartClock() noexcept {
    if (lifecycle->clockSeed == UINT64_MAX)
        return false;
    clockRevision.fetch_add(1);
    anchorTicks.store(mach_absolute_time());
    seed.store(++lifecycle->clockSeed);
    clockRevision.fetch_add(1);
    return true;
}
void Runtime::inheritClock(const Runtime& previous) noexcept {
    // A rollback can restore a Runtime still retained by an older IO table.
    // Keep those readers from accepting a mixed clock origin and seed.
    clockRevision.fetch_add(1);
    anchorTicks.store(previous.anchorTicks.load());
    seed.store(previous.seed.load());
    clockRevision.fetch_add(1);
}
uint64_t Runtime::toNs(uint64_t ticks) const noexcept {
    return ticks / timebase.denom * timebase.numer +
           ticks % timebase.denom * timebase.numer / timebase.denom;
}
uint64_t Runtime::toTicks(uint64_t ns) const noexcept {
    return ns / timebase.numer * timebase.denom +
           ns % timebase.numer * timebase.denom / timebase.numer;
}
bool Runtime::clock(ClockSnapshot& result) const noexcept {
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        const auto revision = clockRevision.load();
        if (revision & 1)
            continue;
        ClockSnapshot candidate{anchorTicks.load(), seed.load()};
        if (clockRevision.load() == revision) {
            result = candidate;
            return true;
        }
    }
    return false;
}
bool Runtime::frameAt(double sampleTime, uint64_t& frame) const noexcept {
    ClockSnapshot snapshot{};
    if (!std::isfinite(sampleTime) || !clock(snapshot))
        return false;
    const long double ns = toNs(snapshot.anchorTicks) +
                           static_cast<long double>(sampleTime) * 1e9L / audio.sampleRate();
    if (ns < 0 || ns >= static_cast<long double>(UINT64_MAX))
        return false;
    frame = hostFrame(static_cast<uint64_t>(ns), audio.sampleRate());
    return true;
}

Device::Device(AudioObjectID id, std::string title, std::shared_ptr<Runtime> io, bool playback)
    : object(id), input(id + 1), output(id + 2), label(std::move(title)), name(label),
      uid(io->transport.uid()), runtime(std::move(io)), hasOutput(playback) {}

State::State() : current_(std::make_unique<Table>()) {
    if (mach_timebase_info(&timebase_) != KERN_SUCCESS || !timebase_.numer || !timebase_.denom)
        throw std::runtime_error("Could not read the device clock timebase");
    table_.store(current_.get());
    const auto uuid = CFUUIDCreate(nullptr);
    if (!uuid)
        throw std::bad_alloc();
    const auto text = CFUUIDCreateString(nullptr, uuid);
    CFRelease(uuid);
    if (!text)
        throw std::bad_alloc();
    char origin[64]{};
    const auto valid = CFStringGetCString(text, origin, sizeof(origin), kCFStringEncodingUTF8);
    CFRelease(text);
    if (!valid)
        throw std::runtime_error("Could not identify the driver rate request owner");
    rateOrigin_ = origin;
    control_ = makeControl();
}
State& state() {
    // The HAL owns plugin lifetime. Avoid static teardown racing its final IO
    // callbacks; the process reclaims the active catalog at exit.
    static State* instance = new State;
    return *instance;
}
OSStatus State::initialize(AudioServerPlugInHostRef host) {
    if (!host || !host->PropertiesChanged)
        return kAudioHardwareIllegalOperationError;
    std::lock_guard guard(mutex_);
    if (host_ && host_ != host)
        return kAudioHardwareIllegalOperationError;
    host_ = host;
    return noErr;
}

void State::setSampleRateMask(uint32_t mask) {
    if (!validVirtualSampleRateMask(mask))
        throw std::invalid_argument("Invalid driver sample-rate capabilities");
    AudioServerPlugInHostRef host;
    std::array<std::shared_ptr<Device>, kMaximumVirtualDevices> devices;
    size_t count;
    {
        std::lock_guard guard(mutex_);
        if (sampleRateMask_ == mask)
            return;
        count = devices_.size();
        std::copy(devices_.begin(), devices_.end(), devices.begin());
        host = host_;
        sampleRateMask_ = mask;
    }
    if (!host)
        return;
    // Host observers can synchronously query the new lists. Keep the current
    // format and clock intact and notify only after releasing the control lock.
    for (size_t index = 0; index < count; ++index) {
        const auto& device = devices[index];
        notify(host, device->object, kAudioDevicePropertyAvailableNominalSampleRates);
        for (const auto stream : {device->input, device->hasOutput ? device->output : 0U})
            if (stream) {
                notify(host, stream, kAudioStreamPropertyAvailableVirtualFormats);
                notify(host, stream, kAudioStreamPropertyAvailablePhysicalFormats);
            }
    }
}

void State::setServiceAvailable(bool available) {
    // Close the realtime gate before any control lock wait. Opening happens
    // only after the published mappings' availability has been updated.
    if (!available)
        serviceAvailable_.store(false);
    std::lock_guard guard(mutex_);
    for (const auto& device : devices_) {
        auto& runtime = *device->runtime;
        const bool demanded =
            available && runtime.lifecycle->alive.load() && runtime.lifecycle->running.load();
        if (demanded)
            runtime.audio.header().driverHeartbeatNs.store(runtime.toNs(mach_absolute_time()));
        runtime.audio.header().driverOnline.store(demanded);
    }
    serviceAvailable_.store(available);
}
Device* State::findLocked(AudioObjectID object) const noexcept {
    for (const auto& device : current_->owners)
        if (device->object == object || device->input == object ||
            (device->hasOutput && device->output == object))
            return device.get();
    return nullptr;
}
std::unique_ptr<State::Table>
State::prepareTable(const std::vector<std::shared_ptr<Device>>& active,
                    const std::vector<std::shared_ptr<Device>>& dead) {
    if (!readers_.load())
        retired_.clear();
    if (retired_.size() >= 256 || active.size() + dead.size() > kMaximumObjects)
        throw std::runtime_error("Driver is waiting for retired IO readers");
    retired_.reserve(retired_.size() + 1);
    auto result = std::make_unique<Table>();
    result->owners.reserve(active.size() + dead.size());
    result->owners.insert(result->owners.end(), active.begin(), active.end());
    result->owners.insert(result->owners.end(), dead.begin(), dead.end());
    for (const auto& device : result->owners)
        result->records[result->count++] = device.get();
    return result;
}
void State::publish(std::unique_ptr<Table> replacement) noexcept {
    auto previous = std::move(current_);
    current_ = std::move(replacement);
    table_.store(current_.get());
    retired_.push_back(std::move(previous));
    if (!readers_.load())
        retired_.clear();
}
void State::notify(AudioServerPlugInHostRef host, AudioObjectID object,
                   AudioObjectPropertySelector selector,
                   AudioObjectPropertyScope scope) const noexcept {
    const AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
    host->PropertiesChanged(host, object, 1, &address);
}

void State::collectDeadLocked() noexcept {
    if (pending_)
        return;
    try {
        if (std::none_of(dead_.begin(), dead_.end(), [](const auto& device) {
                return device->runtime->lifecycle->clients.empty() &&
                       !device->runtime->lifecycle->removalNoticePending;
            }))
            return;
        auto remaining = dead_;
        std::erase_if(remaining, [](const auto& device) {
            return device->runtime->lifecycle->clients.empty() &&
                   !device->runtime->lifecycle->removalNoticePending;
        });
        auto table = prepareTable(devices_, remaining);
        dead_.swap(remaining);
        publish(std::move(table));
    } catch (...) {
        // Lifecycle changes already succeeded. Bounded reclamation can retry
        // on the next control operation if readers or allocation prevent it.
    }
}

OSStatus State::addClient(AudioObjectID object, const AudioServerPlugInClientInfo* client) {
    if (!client)
        return kAudioHardwareIllegalOperationError;
    std::lock_guard guard(mutex_);
    auto* device = findLocked(object);
    if (!device || device->object != object || !device->runtime->lifecycle->alive.load())
        return kAudioHardwareBadDeviceError;
    return device->runtime->lifecycle->clients.emplace(client->mClientID, 0).second
               ? OSStatus(noErr)
               : kAudioHardwareIllegalOperationError;
}
OSStatus State::removeClient(AudioObjectID object, const AudioServerPlugInClientInfo* client) {
    if (!client)
        return kAudioHardwareIllegalOperationError;
    AudioServerPlugInHostRef runningHost = nullptr;
    {
        std::lock_guard guard(mutex_);
        auto* device = findLocked(object);
        if (!device || device->object != object)
            return kAudioHardwareBadDeviceError;
        auto& io = *device->runtime;
        const auto found = io.lifecycle->clients.find(client->mClientID);
        if (found == io.lifecycle->clients.end())
            return kAudioHardwareIllegalOperationError;
        const auto references = found->second;
        const auto running = io.lifecycle->running.load();
        if (references > running)
            return kAudioHardwareIllegalOperationError;
        // Host-confirmed removal also releases references left by a vanished
        // client. Other clients keep their IO, clock and device identity.
        if (references && io.lifecycle->running.fetch_sub(references) == references) {
            io.audio.header().driverOnline.store(0);
            if (io.lifecycle->alive.load())
                runningHost = host_;
        }
        io.lifecycle->clients.erase(found);
        collectDeadLocked();
    }
    if (runningHost)
        notify(runningHost, object, kAudioDevicePropertyDeviceIsRunning);
    return noErr;
}
OSStatus State::start(AudioObjectID object, UInt32 client) {
    AudioServerPlugInHostRef runningHost = nullptr;
    {
        std::lock_guard guard(mutex_);
        auto* device = findLocked(object);
        if (!device || device->object != object || !device->runtime->lifecycle->alive.load())
            return kAudioHardwareBadDeviceError;
        auto& io = *device->runtime;
        const auto found = io.lifecycle->clients.find(client);
        if (found == io.lifecycle->clients.end() || found->second == UINT32_MAX ||
            io.lifecycle->running.load() == UINT32_MAX)
            return kAudioHardwareIllegalOperationError;
        if (!io.lifecycle->running.load()) {
            const auto session = io.lifecycle->writeTiming.load() >> 32;
            if (session == UINT32_MAX || !io.restartClock())
                return kAudioHardwareIllegalOperationError;
            io.lifecycle->writeTiming.store((session + 1) << 32);
            io.lifecycle->callbackTiming.reset(static_cast<uint32_t>(session + 1));
            runningHost = host_;
        }
        ++found->second;
        io.lifecycle->running.fetch_add(1);
        io.audio.header().driverHeartbeatNs.store(io.toNs(mach_absolute_time()));
        io.audio.header().driverOnline.store(serviceAvailable_.load());
    }
    // Host callbacks can synchronously query properties; never notify while
    // holding the state mutex.
    if (runningHost)
        notify(runningHost, object, kAudioDevicePropertyDeviceIsRunning);
    return noErr;
}
OSStatus State::stop(AudioObjectID object, UInt32 client) {
    AudioServerPlugInHostRef runningHost = nullptr;
    {
        std::lock_guard guard(mutex_);
        auto* device = findLocked(object);
        if (!device || device->object != object)
            return kAudioHardwareBadDeviceError;
        auto& io = *device->runtime;
        const auto found = io.lifecycle->clients.find(client);
        if (found == io.lifecycle->clients.end() || !found->second)
            return kAudioHardwareIllegalOperationError;
        --found->second;
        if (io.lifecycle->running.fetch_sub(1) == 1) {
            io.audio.header().driverOnline.store(0);
            if (io.lifecycle->alive.load())
                runningHost = host_;
        }
    }
    if (runningHost)
        notify(runningHost, object, kAudioDevicePropertyDeviceIsRunning);
    return noErr;
}

State::Borrow::Borrow(State& state) noexcept : state_(state) {
    state_.readers_.fetch_add(1);
    table_ = state_.table_.load();
}
State::Borrow::~Borrow() {
    state_.readers_.fetch_sub(1);
}
Device* State::Borrow::device(AudioObjectID object) const noexcept {
    for (size_t index = 0; index < table_->count; ++index)
        if (table_->records[index]->object == object)
            return table_->records[index];
    return nullptr;
}
bool State::knownDevice(AudioObjectID object) noexcept {
    return Borrow(*this).device(object) != nullptr;
}
OSStatus State::stamp(AudioObjectID object, Float64* sample, UInt64* host, UInt64* seed) noexcept {
    if (!sample || !host || !seed)
        return kAudioHardwareIllegalOperationError;
    Borrow borrowed(*this);
    auto* device = borrowed.device(object);
    if (!device)
        return kAudioHardwareBadDeviceError;
    auto& io = *device->runtime;
    ClockSnapshot clock{};
    if (!io.clock(clock))
        return kAudioHardwareNotReadyError;
    const auto now = mach_absolute_time();
    const auto frame = hostFrame(io.toNs(now >= clock.anchorTicks ? now - clock.anchorTicks : 0),
                                 io.audio.sampleRate());
    const auto zero = frame / kClockPeriodFrames * kClockPeriodFrames;
    *sample = static_cast<double>(zero);
    *host = clock.anchorTicks + io.toTicks(frameNs(zero, io.audio.sampleRate()));
    *seed = clock.seed;
    return noErr;
}
OSStatus State::willDo(AudioObjectID object, UInt32 operation, Boolean* willDo,
                       Boolean* inPlace) noexcept {
    Borrow borrowed(*this);
    const auto* device = borrowed.device(object);
    if (!device)
        return kAudioHardwareBadDeviceError;
    if (willDo)
        *willDo = operation == kAudioServerPlugInIOOperationReadInput ||
                  (device->hasOutput && operation == kAudioServerPlugInIOOperationWriteMix);
    if (inPlace)
        *inPlace = true;
    return noErr;
}
OSStatus State::io(AudioObjectID object, AudioObjectID stream, UInt32 operation, UInt32 frames,
                   const AudioServerPlugInIOCycleInfo* cycle, void* buffer) noexcept {
    Borrow borrowed(*this);
    const auto* device = borrowed.device(object);
    if (!device)
        return kAudioHardwareBadDeviceError;
    const bool input =
        operation == kAudioServerPlugInIOOperationReadInput && stream == device->input;
    const bool output = operation == kAudioServerPlugInIOOperationWriteMix && device->hasOutput &&
                        stream == device->output;
    if (!input && !output)
        return kAudioHardwareBadStreamError;
    if (frames > kDeviceAudioCapacity || (frames && (!cycle || !buffer)))
        return kAudioHardwareIllegalOperationError;
    if (!frames)
        return noErr;
    auto& io = *device->runtime;
    const auto writeTimingAtEntry =
        output ? io.lifecycle->writeTiming.load(std::memory_order_relaxed) : 0;
    const auto callbackTimingAtEntry = output ? io.lifecycle->callbackTiming.snapshot() : 0;
    auto samples =
        std::span(static_cast<float*>(buffer), static_cast<size_t>(frames) * io.audio.channels());
    uint64_t first = 0;
    const auto& timestamp = input ? cycle->mInputTime : cycle->mOutputTime;
    const bool hostTime = timestamp.mFlags & kAudioTimeStampHostTimeValid;
    if (hostTime)
        first = hostFrame(io.toNs(timestamp.mHostTime), io.audio.sampleRate());
    const bool demanded =
        serviceAvailable_.load() && io.lifecycle->alive.load() && io.lifecycle->running.load();
    const auto now = io.toNs(mach_absolute_time());
    // Pending format approval silences public data, but live clients still
    // request capture. Publish their lease even while configuration is muted.
    if (demanded)
        io.audio.header().driverHeartbeatNs.store(now);
    const bool valid = demanded && !io.configurationMuted.load() &&
                       (hostTime || io.frameAt(timestamp.mSampleTime, first));
    if (!valid) {
        if (input)
            std::fill(samples.begin(), samples.end(), 0);
        return noErr;
    }
    if (input)
        io.audio.readCapture(first, samples, frames, now, kDeviceAudioConcurrentHeartbeatSkewNs);
    else {
        static_assert(kDeviceAudioCapacity <= CallbackTimingObservation::maximumFrames);
        io.lifecycle->callbackTiming.observe(callbackTimingAtEntry, frames,
                                             cycle->mNominalIOBufferFrameSize);
        const auto period = std::max(frames, cycle->mNominalIOBufferFrameSize);
        if (period <= kDeviceAudioCapacity) {
            auto previous = writeTimingAtEntry;
            const auto updated =
                (previous & 0xffffffff00000000ULL) | std::max(uint32_t(previous), period);
            // A concurrent session reset invalidates this old callback's
            // observation. Do not spin or let it overwrite the new session.
            if (updated != previous)
                io.lifecycle->writeTiming.compare_exchange_strong(previous, updated,
                                                                  std::memory_order_relaxed);
        }
        io.audio.writePlayback(first, samples, frames);
    }
    return noErr;
}

} // namespace loopz::macos::hal
