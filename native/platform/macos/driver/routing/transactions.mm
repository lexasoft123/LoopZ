#include "state.hpp"
#include <dispatch/dispatch.h>

#include <algorithm>
#include <stdexcept>

namespace loopz::macos::hal {
namespace {
constexpr auto kConfigurationTimeout = std::chrono::seconds(10);
constexpr size_t kMaximumRetiredTables = 256;
constexpr OSStatus kCancelled = -128;
using Phase = DriverConfigurationPhase;
} // namespace

struct State::Control {
    dispatch_queue_t requests{nullptr};
    dispatch_source_t timer{nullptr};
    std::atomic<uint32_t> outstanding{0};
    ~Control() {
        if (timer)
            dispatch_source_cancel(timer);
    }
};

void State::destroyControl(Control* control) noexcept {
    delete control;
}
State::ControlPtr State::makeControl() {
    ControlPtr result(new Control, destroyControl);
    result->requests =
        dispatch_queue_create("com.lexasoft.loopz.routing.configuration", DISPATCH_QUEUE_SERIAL);
    auto supervision =
        dispatch_queue_create("com.lexasoft.loopz.routing.supervision", DISPATCH_QUEUE_SERIAL);
    if (!result->requests || !supervision)
        throw std::bad_alloc();
    result->timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, supervision);
    if (!result->timer)
        throw std::bad_alloc();
    dispatch_source_set_timer(result->timer, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC),
                              100 * NSEC_PER_MSEC, 20 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(result->timer, ^{
        expireConfiguration();
    });
    dispatch_resume(result->timer);
    return result;
}
uint32_t State::outstandingRequests() const noexcept {
    return control_->outstanding.load();
}
void State::queueRequest(AudioObjectID device, uint64_t action) {
    if (!action)
        return;
    AudioServerPlugInHostRef host;
    {
        std::lock_guard guard(mutex_);
        if (!pending_ || pending_->waitingAction != action)
            return;
        host = host_;
        control_->outstanding.fetch_add(1);
    }
    dispatch_async(control_->requests, ^{
        bool current;
        {
            std::lock_guard guard(mutex_);
            current = pending_ && pending_->waitingAction == action;
        }
        OSStatus status = noErr;
        if (current)
            status = host->RequestDeviceConfigurationChange(host, device, action, nullptr);
        control_->outstanding.fetch_sub(1);
        if (status)
            requestFailed(device, action, status);
    });
}

State::CatalogPlan State::plan(std::vector<std::shared_ptr<Device>> devices,
                               std::vector<std::shared_ptr<Device>> dead) {
    if (devices.size() > kMaximumVirtualDevices || dead.size() > kMaximumRetiredDevices)
        throw std::runtime_error("Driver device retention limit reached");
    auto table = prepareTable(devices, dead);
    return {std::move(devices), std::move(dead), std::move(table), {}};
}
void State::install(CatalogPlan& replacement) noexcept {
    devices_.swap(replacement.devices);
    dead_.swap(replacement.dead);
    publish(std::move(replacement.table));
}
void State::broadcast(Notices notices) {
    AudioServerPlugInHostRef host;
    {
        std::lock_guard guard(mutex_);
        host = host_;
    }
    for (const auto& device : notices.removed)
        notify(host, device->object, kAudioDevicePropertyDeviceIsAlive);
    for (const auto& device : notices.renamed)
        notify(host, device->object, kAudioObjectPropertyName);
    if (notices.deviceList) {
        notify(host, kAudioObjectPlugInObject, kAudioPlugInPropertyDeviceList);
        notify(host, kAudioObjectPlugInObject, kAudioObjectPropertyOwnedObjects);
    }
    {
        std::lock_guard guard(mutex_);
        for (const auto& device : notices.removed)
            device->runtime->lifecycle->removalNoticePending = false;
        collectDeadLocked();
    }
}
void State::remember(DriverConfigurationResult result) noexcept {
    outcomes_[nextOutcome_] = result;
    nextOutcome_ = (nextOutcome_ + 1) % outcomes_.size();
    haveOutcome_ = true;
}
DriverConfigurationResult State::configurationResult(uint64_t revision) {
    std::lock_guard guard(mutex_);
    if (pending_ && (!revision || pending_->revision == revision))
        return {pending_->revision, pending_->phase, pending_->error};
    if (!revision)
        return haveOutcome_ ? outcomes_[(nextOutcome_ + outcomes_.size() - 1) % outcomes_.size()]
                            : DriverConfigurationResult{0, Phase::Committed, noErr};
    for (const auto& outcome : outcomes_)
        if (outcome.revision == revision)
            return outcome;
    return {revision, Phase::Unknown, noErr};
}

OSStatus State::apply(DeviceTransportSnapshot snapshot) {
    std::lock_guard publication(publicationMutex_);
    Notices notices;
    AudioObjectID requestDevice = 0;
    uint64_t requestAction = 0;
    {
        std::lock_guard guard(mutex_);
        if (!host_ || pending_ || snapshot.revision <= highestRequestedRevision_)
            return kAudioHardwareNotReadyError;
        for (const auto& mapping : snapshot.devices)
            if (!(sampleRateMask_ & virtualSampleRateBit(mapping.view().sampleRate())))
                return kAudioDeviceUnsupportedFormatError;
        auto transaction = std::make_unique<Transaction>();
        transaction->revision = snapshot.revision;
        transaction->nextObject = nextObject_;
        std::vector<std::shared_ptr<Device>> prepared, retained;
        for (const auto& old : dead_)
            if (!old->runtime->lifecycle->clients.empty() ||
                old->runtime->lifecycle->removalNoticePending)
                retained.push_back(old);
        auto action = nextAction_;
        for (auto& mapping : snapshot.devices) {
            if (mapping.view().header().retired.load())
                return kAudioHardwareNotReadyError;
            const auto old =
                std::find_if(devices_.begin(), devices_.end(), [&](const auto& device) {
                    return device->runtime->transport.id() == mapping.id();
                });
            if (old == devices_.end()) {
                if (transaction->nextObject > UINT32_MAX - 3)
                    return kAudioHardwareIllegalOperationError;
                auto runtime = std::make_shared<Runtime>(std::move(mapping), timebase_);
                prepared.push_back(std::make_shared<Device>(transaction->nextObject,
                                                            runtime->transport.name(), runtime,
                                                            runtime->transport.hasOutput()));
                transaction->nextObject += 3;
                continue;
            }
            const auto incoming = mapping.view();
            const auto& existing = (*old)->runtime->audio;
            // A changed origin needs fresh audio storage. Otherwise samples
            // rendered under the old timing could be exposed with a new delay.
            if (mapping.inputLatencyFrames() != (*old)->runtime->transport.inputLatencyFrames() &&
                incoming.mappingIdentity() == existing.mappingIdentity())
                return kAudioHardwareIllegalOperationError;
            const bool mappingChanged = incoming.mappingIdentity() != existing.mappingIdentity() ||
                                        incoming.generation() != existing.generation();
            const bool formatChanged =
                incoming.channels() != existing.channels() ||
                incoming.sampleRate() != existing.sampleRate() ||
                mapping.hasOutput() != (*old)->hasOutput ||
                mapping.inputLatencyFrames() != (*old)->runtime->transport.inputLatencyFrames();
            std::shared_ptr<Device> replacement;
            if (!mappingChanged && !formatChanged && (*old)->label == mapping.name()) {
                prepared.push_back(*old);
                continue;
            }
            if (mappingChanged || formatChanged) {
                if (formatChanged && action > UINT64_MAX - 2)
                    return kAudioHardwareIllegalOperationError;
                auto runtime = std::make_shared<Runtime>(std::move(mapping), timebase_,
                                                         (*old)->runtime->lifecycle);
                runtime->configurationMuted.store(formatChanged);
                replacement = std::make_shared<Device>((*old)->object, runtime->transport.name(),
                                                       runtime, runtime->transport.hasOutput());
                if (formatChanged) {
                    transaction->changes.push_back({*old, replacement, action, action + 1});
                    action += 2;
                } else {
                    // Storage replacement does not alter the HAL format. RCU
                    // keeps old callbacks' mappings alive; publish the new one
                    // only when the complete transaction commits.
                    transaction->rebindings.push_back({*old, replacement});
                }
            } else {
                replacement = std::make_shared<Device>((*old)->object, mapping.name(),
                                                       (*old)->runtime, (*old)->hasOutput);
            }
            if ((*old)->label != replacement->label) {
                notices.renamed.push_back(replacement);
                if (formatChanged)
                    transaction->aborted.renamed.push_back(*old);
            }
            prepared.push_back(std::move(replacement));
        }
        auto finalDead = retained;
        for (const auto& old : devices_) {
            if (std::none_of(prepared.begin(), prepared.end(), [&](const auto& device) {
                    return old->object == device->object;
                })) {
                notices.removed.push_back(old);
                finalDead.push_back(old);
            }
        }
        if (prepared == devices_) {
            highestRequestedRevision_ = snapshot.revision;
            revision_ = snapshot.revision;
            remember({snapshot.revision, Phase::Committed, noErr});
            return noErr;
        }
        notices.deviceList = !std::equal(prepared.begin(), prepared.end(), devices_.begin(),
                                         devices_.end(), [](const auto& left, const auto& right) {
                                             return left->object == right->object;
                                         });
        transaction->final = plan(std::move(prepared), std::move(finalDead));
        transaction->final.notices = std::move(notices);
        if (!transaction->changes.empty()) {
            if (!host_->RequestDeviceConfigurationChange || outstandingRequests())
                return kAudioHardwareNotReadyError;
            auto prefix = devices_;
            transaction->rollback.push_back(plan(prefix, retained));
            for (size_t index = 0; index < transaction->changes.size(); ++index) {
                const auto& change = transaction->changes[index];
                const auto found =
                    std::find_if(prefix.begin(), prefix.end(), [&](const auto& device) {
                        return device->object == change.before->object;
                    });
                *found = change.after;
                transaction->forward.push_back(plan(prefix, retained));
                if (index + 1 < transaction->changes.size())
                    transaction->rollback.push_back(plan(prefix, retained));
                auto quarantineDevices = devices_;
                auto quarantineDead = retained;
                Notices quarantineNotices;
                for (size_t affected = 0; affected <= index; ++affected) {
                    const auto& changed = transaction->changes[affected];
                    std::erase_if(quarantineDevices, [&](const auto& device) {
                        return device->object == changed.before->object;
                    });
                    quarantineDead.push_back(changed.after);
                    quarantineNotices.removed.push_back(changed.after);
                }
                auto quarantine = plan(std::move(quarantineDevices), std::move(quarantineDead));
                quarantineNotices.deviceList = true;
                quarantine.notices = std::move(quarantineNotices);
                transaction->quarantine.push_back(std::move(quarantine));
            }
            const auto budget = retired_.size() + 2 * transaction->changes.size() + 2;
            if (budget > kMaximumRetiredTables)
                return kAudioHardwareNotReadyError;
            retired_.reserve(budget);
        }
        highestRequestedRevision_ = snapshot.revision;
        nextAction_ = action;
        pending_ = std::move(transaction);
        if (pending_->changes.empty()) {
            finishLocked(Phase::Committed, notices);
        } else {
            requestDevice = pending_->changes[0].before->object;
            requestAction = pending_->changes[0].forwardAction;
            pending_->waitingAction = requestAction;
            pending_->deadline = std::chrono::steady_clock::now() + kConfigurationTimeout;
        }
    }
    broadcast(std::move(notices));
    queueRequest(requestDevice, requestAction);
    return noErr;
}

void State::finishLocked(Phase phase, Notices& notices) noexcept {
    auto& transaction = *pending_;
    if (phase == Phase::Committed) {
        for (const auto& rebinding : transaction.rebindings) {
            auto& before = *rebinding.before->runtime;
            auto& after = *rebinding.after->runtime;
            // Copy at commit, not preparation: client start/stop may have
            // advanced the clock while another device awaited format approval.
            after.inheritClock(before);
            before.configurationMuted.store(true);
            before.audio.header().driverOnline.store(0);
            after.audio.header().driverHeartbeatNs.store(after.toNs(mach_absolute_time()));
            after.audio.header().driverOnline.store(serviceAvailable_.load() &&
                                                    after.lifecycle->running.load() != 0);
        }
        for (const auto& removed : transaction.final.notices.removed) {
            removed->runtime->lifecycle->removalNoticePending = true;
            removed->runtime->lifecycle->alive.store(false);
            removed->runtime->audio.header().driverOnline.store(0);
        }
        install(transaction.final);
        for (const auto& change : transaction.changes)
            change.after->runtime->configurationMuted.store(false);
        nextObject_ = transaction.nextObject;
        revision_ = transaction.revision;
        notices = std::move(transaction.final.notices);
    } else if (phase == Phase::Aborted) {
        for (const auto& change : transaction.changes) {
            change.before->runtime->configurationMuted.store(false);
            change.after->runtime->configurationMuted.store(true);
        }
        notices = std::move(transaction.aborted);
    }
    remember({transaction.revision, phase, transaction.error});
    pending_.reset();
}

void State::failLocked(OSStatus error, Notices& notices, AudioObjectID& device,
                       uint64_t& action) noexcept {
    auto& transaction = *pending_;
    transaction.waitingAction = 0;
    if (transaction.phase == Phase::RollingBack) {
        transaction.error = error;
        auto& quarantine = transaction.quarantine[transaction.applied - 1];
        for (const auto& removed : quarantine.notices.removed) {
            auto& runtime = *removed->runtime;
            runtime.configurationMuted.store(true);
            runtime.lifecycle->alive.store(false);
            runtime.lifecycle->removalNoticePending = true;
            // A same-format transition may share the original mapping. Mark
            // it retired so recovery cannot accidentally reuse that generation.
            auto& header = runtime.audio.header();
            header.retired.store(1);
            header.helperOnline.store(0);
            header.driverOnline.store(0);
        }
        install(quarantine);
        notices = std::move(quarantine.notices);
        finishLocked(Phase::Quarantined, notices);
        return;
    }
    transaction.error = error;
    if (!transaction.applied) {
        finishLocked(Phase::Aborted, notices);
        return;
    }
    transaction.phase = Phase::RollingBack;
    const auto& last = transaction.changes[transaction.applied - 1];
    device = last.before->object;
    action = last.rollbackAction;
    transaction.waitingAction = action;
    transaction.deadline = std::chrono::steady_clock::now() + kConfigurationTimeout;
}

OSStatus State::performChange(AudioObjectID device, uint64_t action) {
    Notices notices;
    AudioServerPlugInHostRef latencyHost = nullptr;
    AudioObjectID nextDevice = 0;
    uint64_t nextAction = 0;
    OSStatus result = noErr;
    {
        std::lock_guard guard(mutex_);
        if (!pending_ || !action || pending_->waitingAction != action)
            return kAudioHardwareIllegalOperationError;
        auto& transaction = *pending_;
        const bool forward = transaction.phase == Phase::Pending;
        const auto index = forward ? transaction.applied : transaction.applied - 1;
        const auto& change = transaction.changes[index];
        if (device != change.before->object)
            return kAudioHardwareBadDeviceError;
        auto replacement = forward ? change.after : change.before;
        const auto previous = forward ? change.before : change.after;
        const bool playbackVisibilityOnly =
            previous->hasOutput != replacement->hasOutput &&
            previous->runtime->audio.channels() == replacement->runtime->audio.channels() &&
            previous->runtime->audio.sampleRate() == replacement->runtime->audio.sampleRate() &&
            previous->runtime->transport.inputLatencyFrames() ==
                replacement->runtime->transport.inputLatencyFrames();
        bool clockReady = false;
        if (!replacement->runtime->audio.header().retired.load()) {
            if (playbackVisibilityOnly) {
                // The input timeline has not changed. Copy the latest clock
                // at host approval (also on rollback), since client IO may
                // have restarted while this configuration request waited.
                replacement->runtime->inheritClock(*previous->runtime);
                clockReady = true;
            } else {
                clockReady = replacement->runtime->restartClock();
            }
        }
        if (!clockReady) {
            result = kAudioHardwareIllegalOperationError;
            failLocked(result, notices, nextDevice, nextAction);
        } else {
            if (playbackVisibilityOnly &&
                previous->runtime->audio.mappingIdentity() ==
                    replacement->runtime->audio.mappingIdentity() &&
                previous->runtime->audio.generation() == replacement->runtime->audio.generation()) {
                // The host has approved this device, and its input still
                // reads the committed mapping with the same format/timing.
                // Keep it usable while other devices await approval or this
                // prefix awaits rollback. Fresh storage must remain muted
                // until the complete graph commits and its producer is ready.
                replacement->runtime->configurationMuted.store(false);
            }
            if (change.before->runtime->transport.inputLatencyFrames() !=
                change.after->runtime->transport.inputLatencyFrames())
                latencyHost = host_;
            transaction.waitingAction = 0;
            (forward ? change.before : change.after)->runtime->audio.header().driverOnline.store(0);
            replacement->runtime->audio.header().driverHeartbeatNs.store(
                replacement->runtime->toNs(mach_absolute_time()));
            replacement->runtime->audio.header().driverOnline.store(
                serviceAvailable_.load() && replacement->runtime->lifecycle->running.load() != 0);
            if (forward) {
                install(transaction.forward[index]);
                ++transaction.applied;
                if (transaction.applied == transaction.changes.size()) {
                    finishLocked(Phase::Committed, notices);
                } else {
                    const auto& next = transaction.changes[transaction.applied];
                    nextDevice = next.before->object;
                    nextAction = next.forwardAction;
                }
            } else {
                install(transaction.rollback[index]);
                --transaction.applied;
                replacement->runtime->configurationMuted.store(false);
                if (!transaction.applied) {
                    finishLocked(Phase::Aborted, notices);
                } else {
                    const auto& next = transaction.changes[transaction.applied - 1];
                    nextDevice = next.before->object;
                    nextAction = next.rollbackAction;
                }
            }
            if (pending_ && nextAction) {
                pending_->waitingAction = nextAction;
                pending_->deadline = std::chrono::steady_clock::now() + kConfigurationTimeout;
            }
        }
    }
    broadcast(std::move(notices));
    if (latencyHost)
        notify(latencyHost, device, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput);
    queueRequest(nextDevice, nextAction);
    return result;
}

OSStatus State::requestFailed(AudioObjectID device, uint64_t action, OSStatus error) {
    Notices notices;
    AudioObjectID nextDevice = 0;
    uint64_t nextAction = 0;
    {
        std::lock_guard guard(mutex_);
        if (!pending_ || pending_->waitingAction != action)
            return kAudioHardwareIllegalOperationError;
        const auto index =
            pending_->phase == Phase::Pending ? pending_->applied : pending_->applied - 1;
        if (pending_->changes[index].before->object != device)
            return kAudioHardwareBadDeviceError;
        failLocked(error, notices, nextDevice, nextAction);
    }
    broadcast(std::move(notices));
    queueRequest(nextDevice, nextAction);
    return noErr;
}
OSStatus State::abortChange(AudioObjectID device, uint64_t action) {
    return requestFailed(device, action, kAudioHardwareUnspecifiedError);
}
OSStatus State::cancelConfiguration(uint64_t revision) {
    Notices notices;
    AudioObjectID device = 0;
    uint64_t action = 0;
    {
        std::lock_guard guard(mutex_);
        if (!pending_ || pending_->revision != revision)
            return kAudioHardwareIllegalOperationError;
        if (pending_->phase == Phase::RollingBack)
            return noErr;
        failLocked(kCancelled, notices, device, action);
    }
    broadcast(std::move(notices));
    queueRequest(device, action);
    return noErr;
}
void State::expireConfiguration() {
    Notices notices;
    AudioObjectID device = 0;
    uint64_t action = 0;
    {
        std::lock_guard guard(mutex_);
        if (!pending_) {
            collectDeadLocked();
            if (!readers_.load())
                retired_.clear();
            return;
        }
        if (std::chrono::steady_clock::now() < pending_->deadline)
            return;
        failLocked(kAudioHardwareNotReadyError, notices, device, action);
    }
    broadcast(std::move(notices));
    queueRequest(device, action);
}

} // namespace loopz::macos::hal
