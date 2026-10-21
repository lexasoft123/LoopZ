#include <loopz/macos/recovery_events.hpp>
#include <loopz/macos/recovery_events_backend.hpp>
#import <AppKit/AppKit.h>
#include <CoreAudio/CoreAudio.h>

#include <array>
#include <atomic>
#include <stdexcept>

namespace loopz::macos {
namespace {
constexpr uint32_t bits(RecoveryEvent event) noexcept {
    return static_cast<uint32_t>(event);
}
uint64_t after(uint64_t now, uint64_t delay) noexcept {
    return now > UINT64_MAX - delay ? UINT64_MAX : now + delay;
}
struct Mailbox {
    std::atomic<uint32_t> pending{0};
    std::atomic<bool> active{true};
    void signal(RecoveryEvent event) noexcept {
        if (active.load())
            pending.fetch_or(bits(event));
    }
};
struct Listener {
    AudioObjectPropertyAddress address;
    AudioObjectPropertyListenerBlock block;
    bool installed{false};
};
} // namespace
struct RecoveryEvents::Impl {
    const Scope scope;
    const RecoveryEventBackend backend;
    const std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
    dispatch_queue_t queue = dispatch_queue_create("com.lexasoft.loopz.recovery-events", nullptr);
    NSNotificationCenter* center;
    NSMutableArray* observers = [NSMutableArray array];
    std::array<Listener, 6> listeners;
    uint64_t nextRegistration{0}, nextDelivery{0};
    int32_t registrationError{0};
    Impl(Scope selected, RecoveryEventBackend api)
        : scope(selected), backend(api), center(api.workspaceCenter) {
        if (!backend.add || !backend.remove || !center)
            throw std::invalid_argument("Recovery listeners require a complete native backend");
        const std::array<std::pair<AudioObjectPropertySelector, RecoveryEvent>, 6> properties{{
            {kAudioHardwarePropertyDevices, RecoveryEvent::AudioInventory},
            {kAudioHardwarePropertyDefaultInputDevice, RecoveryEvent::AudioInventory},
            {kAudioHardwarePropertyDefaultOutputDevice, RecoveryEvent::AudioInventory},
            {kAudioHardwarePropertyDefaultSystemOutputDevice, RecoveryEvent::AudioInventory},
            {kAudioHardwarePropertyProcessObjectList, RecoveryEvent::Applications},
            {kAudioHardwarePropertyServiceRestarted, RecoveryEvent::AudioServiceReset},
        }};
        for (size_t i = 0; i < properties.size(); ++i) {
            const auto state = mailbox;
            const auto event = properties[i].second;
            listeners[i].address = {properties[i].first, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
            listeners[i].block = ^(UInt32, const AudioObjectPropertyAddress*) {
                state->signal(event);
            };
        }
        observe(NSWorkspaceDidWakeNotification, RecoveryEvent::Wake);
        observe(NSWorkspaceSessionDidBecomeActiveNotification, RecoveryEvent::Wake);
        observe(NSWorkspaceDidLaunchApplicationNotification, RecoveryEvent::Applications);
        observe(NSWorkspaceDidTerminateApplicationNotification, RecoveryEvent::Applications);
    }
    void observe(NSNotificationName name, RecoveryEvent event) {
        const auto state = mailbox;
        id token = [center addObserverForName:name
                                       object:nil
                                        queue:nil
                                   usingBlock:^(NSNotification*) {
                                       state->signal(event);
                                   }];
        if (token)
            [observers addObject:token];
    }
    void removeAudio() noexcept {
        for (auto& listener : listeners)
            if (listener.installed) {
                backend.remove(kAudioObjectSystemObject, &listener.address, queue, listener.block);
                listener.installed = false;
            }
        // Every callback only writes the mailbox. Draining this private queue
        // cannot wait for the control thread, an audio callback or an IO stop.
        dispatch_sync_f(queue, nullptr, [](void*) {});
    }
    void installAudio(uint64_t now) {
        if (scope != Scope::WorkspaceAndAudio || now < nextRegistration)
            return;
        nextRegistration = after(now, 5000);
        registrationError = 0;
        for (auto& listener : listeners)
            if (!listener.installed) {
                const auto error =
                    backend.add(kAudioObjectSystemObject, &listener.address, queue, listener.block);
                listener.installed = error == noErr;
                if (error)
                    registrationError = error;
            }
    }
    ~Impl() {
        mailbox->active.store(false);
        for (id token in observers)
            [center removeObserver:token];
        removeAudio();
    }
};
RecoveryEvents::RecoveryEvents(Scope scope)
    : RecoveryEvents(scope,
                     {&AudioObjectAddPropertyListenerBlock, &AudioObjectRemovePropertyListenerBlock,
                      NSWorkspace.sharedWorkspace.notificationCenter}) {}
RecoveryEvents::RecoveryEvents(Scope scope, const RecoveryEventBackend& backend)
    : impl_(std::make_unique<Impl>(scope, backend)) {}
RecoveryEvents::~RecoveryEvents() = default;
RecoveryEvent RecoveryEvents::poll(uint64_t now) {
    auto& self = *impl_;
    self.installAudio(now);
    const auto pending = self.mailbox->pending.load();
    const auto urgent = bits(RecoveryEvent::AudioServiceReset) | bits(RecoveryEvent::Wake);
    if (!pending || (now < self.nextDelivery && !(pending & urgent)))
        return RecoveryEvent::None;
    auto delivered = self.mailbox->pending.exchange(0);
    // A reset can arrive between load() and exchange(). Act on the consumed
    // batch rather than the earlier observation, or that reset would be lost.
    if (delivered & bits(RecoveryEvent::AudioServiceReset)) {
        // Apple requires clients to re-establish cached state and listeners
        // after this notification. Retire queued callbacks before reattaching.
        self.removeAudio();
        delivered |= self.mailbox->pending.exchange(0);
        self.nextRegistration = 0;
        self.installAudio(now);
        // Keep notifications arriving during/after reattachment pending. A
        // second service reset can invalidate these newly added listeners.
    }
    self.nextDelivery = after(now, 250);
    return static_cast<RecoveryEvent>(delivered);
}
RecoveryEventStatus RecoveryEvents::status() const noexcept {
    RecoveryEventStatus result;
    result.registrationError = impl_->registrationError;
    for (const auto& listener : impl_->listeners)
        result.audioListeners += listener.installed;
    return result;
}
} // namespace loopz::macos
