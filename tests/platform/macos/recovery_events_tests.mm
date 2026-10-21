#include <loopz/macos/recovery_events.hpp>
#include <loopz/macos/recovery_events_backend.hpp>
#import <AppKit/AppKit.h>

#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <map>
#include <string>
#include <vector>

using namespace loopz::macos;
namespace {
struct FakeAudio {
    uint32_t adds{0}, removes{0};
    bool failDefaultInput{true}, failedRemoval{false};
    std::map<AudioObjectPropertySelector, AudioObjectPropertyListenerBlock> active;
    std::vector<AudioObjectPropertyListenerBlock> retained;
    AudioObjectPropertyListenerBlock duringAdd{nil};
    void signal(AudioObjectPropertySelector selector) {
        const AudioObjectPropertyAddress address{selector, kAudioObjectPropertyScopeGlobal,
                                                 kAudioObjectPropertyElementMain};
        active.at(selector)(1, &address);
    }
};
FakeAudio* fake = nullptr;
OSStatus add(AudioObjectID object, const AudioObjectPropertyAddress* address,
             dispatch_queue_t queue, AudioObjectPropertyListenerBlock block) {
    assert(object == kAudioObjectSystemObject && queue && block && fake);
    ++fake->adds;
    if (address->mSelector == kAudioHardwarePropertyDefaultInputDevice && fake->failDefaultInput)
        return kAudioHardwareBadObjectError;
    assert(!fake->active.contains(address->mSelector));
    fake->active.emplace(address->mSelector, block);
    fake->retained.push_back(block);
    if (const auto callback = fake->duringAdd) {
        fake->duringAdd = nil;
        const AudioObjectPropertyAddress reset{kAudioHardwarePropertyServiceRestarted,
                                               kAudioObjectPropertyScopeGlobal,
                                               kAudioObjectPropertyElementMain};
        callback(1, &reset);
    }
    return noErr;
}
OSStatus remove(AudioObjectID object, const AudioObjectPropertyAddress* address,
                dispatch_queue_t queue, AudioObjectPropertyListenerBlock block) {
    assert(object == kAudioObjectSystemObject && queue && block && fake);
    ++fake->removes;
    assert(fake->active.erase(address->mSelector) == 1);
    if (fake->failedRemoval)
        return kAudioHardwareBadObjectError;
    return noErr;
}
void audioListenerRecovery() {
    FakeAudio state;
    fake = &state;
    {
        RecoveryEvents events(RecoveryEvents::Scope::WorkspaceAndAudio,
                              {add, remove, [NSNotificationCenter new]});
        assert(events.poll(0) == RecoveryEvent::None);
        assert(events.status().audioListeners == 5);
        assert(events.status().registrationError == kAudioHardwareBadObjectError);
        state.failDefaultInput = false;
        events.poll(4999);
        assert(state.adds == 6);
        events.poll(5000);
        assert(state.adds == 7 && events.status().audioListeners == 6);
        assert(!events.status().registrationError);
        state.signal(kAudioHardwarePropertyDevices);
        assert(events.poll(5001) == RecoveryEvent::AudioInventory);
        state.signal(kAudioHardwarePropertyProcessObjectList);
        assert(events.poll(5002) == RecoveryEvent::None);
        state.signal(kAudioHardwarePropertyServiceRestarted);
        state.failedRemoval = true; // The old service no longer owns its objects.
        const auto reset = events.poll(5003);
        assert(includes(reset, RecoveryEvent::AudioServiceReset));
        assert(includes(reset, RecoveryEvent::Applications));
        assert(state.removes == 6 && state.adds == 13);
        assert(events.status().audioListeners == 6 && !events.status().registrationError);
        assert(events.poll(5004) == RecoveryEvent::None);
        state.signal(kAudioHardwarePropertyDefaultOutputDevice);
        assert(events.poll(5253) == RecoveryEvent::AudioInventory);
        // A second reset during reattachment invalidates the new listeners.
        // It must remain pending for another reattachment on the next poll.
        state.duringAdd = state.active.at(kAudioHardwarePropertyServiceRestarted);
        state.signal(kAudioHardwarePropertyServiceRestarted);
        assert(includes(events.poll(5254), RecoveryEvent::AudioServiceReset));
        assert(state.removes == 12);
        assert(includes(events.poll(5255), RecoveryEvent::AudioServiceReset));
        assert(state.removes == 18);
    }
    assert(state.removes == 24 && state.active.empty());
    // Even a provider retaining a callback after failed removal cannot reach
    // the destroyed observer or runtime. The independent mailbox is inactive.
    const AudioObjectPropertyAddress reset{kAudioHardwarePropertyServiceRestarted,
                                           kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMain};
    for (auto block : state.retained)
        block(1, &reset);
    fake = nullptr;
}
void resetArrivalDuringDelivery() {
    FakeAudio state;
    state.failDefaultInput = false;
    fake = &state;
    {
        RecoveryEvents events(RecoveryEvents::Scope::WorkspaceAndAudio,
                              {add, remove, [NSNotificationCenter new]});
        events.poll(0);
        const auto inventory = state.active.at(kAudioHardwarePropertyDevices);
        const auto reset = state.active.at(kAudioHardwarePropertyServiceRestarted);
        std::atomic<bool> running{true};
        std::thread producer([&] {
            const AudioObjectPropertyAddress inventoryAddress{kAudioHardwarePropertyDevices,
                                                              kAudioObjectPropertyScopeGlobal,
                                                              kAudioObjectPropertyElementMain};
            const AudioObjectPropertyAddress resetAddress{kAudioHardwarePropertyServiceRestarted,
                                                          kAudioObjectPropertyScopeGlobal,
                                                          kAudioObjectPropertyElementMain};
            while (running.load()) {
                inventory(1, &inventoryAddress);
                reset(1, &resetAddress);
                std::this_thread::yield();
            }
        });
        for (uint64_t now = 250; now < 500000; now += 250) {
            const auto before = state.removes;
            const auto delivered = events.poll(now);
            assert(state.removes ==
                   before + (includes(delivered, RecoveryEvent::AudioServiceReset) ? 6 : 0));
        }
        running = false;
        producer.join();
    }
    fake = nullptr;
}
} // namespace
int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc == 2 && std::string(argv[1]) == "--inspect-listeners") {
            RecoveryEvents events;
            events.poll(0);
            const auto status = events.status();
            std::printf("{\"audioListeners\":%u,\"registrationError\":%d,"
                        "\"audioIOOpened\":false,\"permissionRequested\":false}\n",
                        status.audioListeners, status.registrationError);
            return status.audioListeners == 6 && !status.registrationError ? 0 : 1;
        }
        audioListenerRecovery();
        resetArrivalDuringDelivery();
        NSNotificationCenter* center = [NSNotificationCenter new];
        const RecoveryEventBackend backend{add, remove, center};
        const auto post = [&](NSNotificationName name) {
            [center postNotificationName:name object:nil];
        };
        {
            RecoveryEvents events(RecoveryEvents::Scope::WorkspaceOnly, backend);
            assert(!events.status().audioListeners);
            assert(events.poll(0) == RecoveryEvent::None);
            post(NSWorkspaceDidLaunchApplicationNotification);
            assert(events.poll(1) == RecoveryEvent::Applications);
            for (unsigned i = 0; i < 100; ++i)
                post(NSWorkspaceDidTerminateApplicationNotification);
            assert(events.poll(2) == RecoveryEvent::None);
            assert(events.poll(250) == RecoveryEvent::None);
            assert(events.poll(251) == RecoveryEvent::Applications);
            assert(events.poll(252) == RecoveryEvent::None);
            post(NSWorkspaceDidWakeNotification);
            assert(events.poll(253) == RecoveryEvent::Wake);
            post(NSWorkspaceDidLaunchApplicationNotification);
            post(NSWorkspaceSessionDidBecomeActiveNotification);
            const auto combined = events.poll(254);
            assert(includes(combined, RecoveryEvent::Wake));
            assert(includes(combined, RecoveryEvent::Applications));
            assert(events.poll(255) == RecoveryEvent::None);
        }
        // Notification delivery may run on another thread. Destruction removes
        // observers while in-flight callbacks retain only their own mailbox.
        std::atomic<bool> running{true}, began{false};
        std::thread producer([&] {
            @autoreleasepool {
                began = true;
                while (running.load()) {
                    @autoreleasepool {
                        [center postNotificationName:NSWorkspaceDidWakeNotification object:nil];
                    }
                    std::this_thread::yield();
                }
            }
        });
        while (!began.load())
            std::this_thread::yield();
        for (unsigned i = 0; i < 100; ++i) {
            RecoveryEvents events(RecoveryEvents::Scope::WorkspaceOnly, backend);
            events.poll(i);
        }
        running = false;
        producer.join();
        // This is a process-local posted notification, never a machine sleep.
        post(NSWorkspaceDidWakeNotification);
        std::puts("Workspace recovery signals: coalescing, immediate wake and teardown passed");
    }
}
