#include <loopz/macos/device_inventory.hpp>
#include <loopz/macos/driver_protocol.hpp>
#include <cassert>
#include <cstdio>
#include <algorithm>
#include <string>
#include <chrono>
#include <atomic>
#include <memory>
#include <semaphore>
#include <thread>

namespace {
using loopz::macos::DeviceInventory;
struct DiscoveryGate {
    std::binary_semaphore entered{0}, release{0}, returned{0};
    std::atomic<unsigned> calls{0};
};
NSDictionary* completed(DeviceInventory& inventory, uint64_t now) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        auto* result = inventory.snapshot(now);
        if (![result[@"devicesRefreshing"] boolValue])
            return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("Fake background metadata did not complete");
}
void backgroundDiscoveryDoesNotBlockOrPublishStaleResults() {
    const auto gate = std::make_shared<DiscoveryGate>();
    std::atomic<unsigned> writes{0};
    DeviceInventory inventory(
        [gate] {
            const auto call = gate->calls.fetch_add(1);
            if (!call) {
                gate->entered.release();
                gate->release.acquire();
            }
            if (call == 2)
                throw std::runtime_error("Transient background failure");
            singz::AudioHostInventory result;
            singz::AudioHostDeviceInfo device;
            device.uid = "hardware";
            device.label = call ? "Current interface" : "Stale interface";
            device.inputChannels = 8;
            device.bufferFrames.preferredFrames = call ? 512 : 64;
            result.devices.push_back(device);
            return result;
        },
        [&](const auto&, auto, auto) {
            ++writes;
        },
        DeviceInventory::DiscoveryMode::Background);
    auto* pending = inventory.snapshot(100);
    assert([pending[@"devicesRefreshing"] boolValue] && ![pending[@"devicesFresh"] boolValue]);
    assert(gate->entered.try_acquire_for(std::chrono::seconds(2)));
    for (unsigned attempt = 0; attempt < 256; ++attempt)
        assert([inventory.snapshot(100)[@"devicesRefreshing"] boolValue]);
    assert(gate->calls == 1 && !writes); // At most one in-flight provider.
    inventory.invalidate();
    gate->release.release();
    auto* ready = completed(inventory, 100);
    assert(gate->calls == 2 && [ready[@"devicesFresh"] boolValue]);
    assert([ready[@"devices"][0][@"name"] isEqual:@"Current interface"]);
    assert([ready[@"devices"][0][@"bufferFrames"] unsignedIntValue] == 512);
    assert(inventory.snapshot(2099) == ready && gate->calls == 2);
    pending = inventory.snapshot(2100);
    assert([pending[@"devicesRefreshing"] boolValue]);
    assert([pending[@"devices"] isEqual:ready[@"devices"]]);
    auto* failed = completed(inventory, 2100);
    assert(gate->calls == 3 && ![failed[@"devicesFresh"] boolValue]);
    assert([failed[@"devicesError"] length] && [failed[@"devices"] isEqual:ready[@"devices"]]);
    ready = completed(inventory, 4100);
    assert(gate->calls == 4 && [ready[@"devicesFresh"] boolValue] && !writes);

    const auto blocked = std::make_shared<DiscoveryGate>();
    {
        DeviceInventory closing(
            [blocked] {
                blocked->entered.release();
                blocked->release.acquire();
                blocked->returned.release();
                return singz::AudioHostInventory{};
            },
            {}, DeviceInventory::DiscoveryMode::Background);
        (void)closing.snapshot(1);
        assert(blocked->entered.try_acquire_for(std::chrono::seconds(2)));
    } // Must not join the blocked provider or leave it referring to `closing`.
    blocked->release.release();
    assert(blocked->returned.try_acquire_for(std::chrono::seconds(2)));
}
} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc == 2 && std::string(argv[1]) == "--live-metadata") {
            const auto start = std::chrono::steady_clock::now();
            singz::AudioHost host;
            const auto reference = host.enumerate();
            const auto fullEnd = std::chrono::steady_clock::now();
            loopz::macos::DeviceInventory inventory(
                {}, {}, loopz::macos::DeviceInventory::DiscoveryMode::Synchronous);
            auto* actual = inventory.snapshot(1);
            const auto metadataEnd = std::chrono::steady_clock::now();
            assert([actual[@"devicesFresh"] boolValue]);
            size_t expected = 0;
            for (const auto& device : reference.devices) {
                if (device.uid == loopz::kDeviceUID ||
                    device.uid.starts_with("com.lexasoft.loopz.device.") ||
                    (!device.inputChannels && !device.outputChannels))
                    continue;
                ++expected;
                NSDictionary* match = nil;
                for (NSDictionary* entry in actual[@"devices"])
                    if ([entry[@"uid"] isEqualToString:@(device.uid.c_str())])
                        match = entry;
                assert(match && [match[@"name"] isEqualToString:@(device.label.c_str())]);
                assert([match[@"inputChannels"] unsignedIntValue] == device.inputChannels);
                assert([match[@"outputChannels"] unsignedIntValue] == device.outputChannels);
                assert([match[@"rate"] doubleValue] == device.nominalSampleRate);
                assert([match[@"bufferFrames"] unsignedIntValue] ==
                       device.bufferFrames.preferredFrames);
                assert([match[@"minimumBufferFrames"] unsignedIntValue] ==
                       device.bufferFrames.minimumFrames);
                assert([match[@"maximumBufferFrames"] unsignedIntValue] ==
                       device.bufferFrames.maximumFrames);
                assert([match[@"defaultInput"] boolValue] == device.defaultInput);
                assert([match[@"defaultOutput"] boolValue] == device.defaultOutput);
            }
            assert([actual[@"devices"] count] == expected);
            std::printf("{\"passed\":true,\"readOnly\":true,\"devices\":%zu,"
                        "\"fullInventoryMs\":%.3f,\"metadataInventoryMs\":%.3f}\n",
                        expected,
                        std::chrono::duration<double, std::milli>(fullEnd - start).count(),
                        std::chrono::duration<double, std::milli>(metadataEnd - fullEnd).count());
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--live-buffer") {
            const std::string uid(argv[2]);
            singz::AudioHost host;
            const auto current = [&] {
                const auto inventory = host.enumerate();
                const auto device = std::find_if(inventory.devices.begin(), inventory.devices.end(),
                                                 [&](const auto& item) {
                                                     return item.uid == uid;
                                                 });
                if (device == inventory.devices.end())
                    throw std::runtime_error("Live buffer test device unavailable");
                return device->bufferFrames.preferredFrames;
            };
            loopz::macos::DeviceInventory inventory;
            const auto original = current();
            if (original != 512)
                throw std::runtime_error(
                    "Live test requires the verified original 512-frame state");
            try {
                inventory.setBufferFrames(uid, original, 128);
                if (current() != 128)
                    throw std::runtime_error("Live buffer test did not observe 128 frames");
                inventory.setBufferFrames(uid, 128, original);
                if (current() != original)
                    throw std::runtime_error("Live buffer test did not restore the original value");
                std::puts("{\"passed\":true,\"settingsBackend\":true,\"before\":512,\"tested\":128,"
                          "\"after\":512}");
                return 0;
            } catch (...) {
                if (current() == 128)
                    inventory.setBufferFrames(uid, 128, original);
                throw;
            }
        }
        assert(argc == 1);
        backgroundDiscoveryDoesNotBlockOrPublishStaleResults();
        singz::AudioHostInventory devices;
        singz::AudioHostDeviceInfo input;
        input.uid = "hardware-input";
        input.label = "Studio input";
        input.inputChannels = 16;
        input.nominalSampleRate = 44100;
        devices.devices.push_back(input);
        auto own = input;
        own.uid = loopz::kDeviceUID;
        devices.devices.push_back(own);
        own.uid = "com.lexasoft.loopz.device.mix";
        devices.devices.push_back(own);
        unsigned calls = 0;
        bool fail = false;
        loopz::macos::DeviceInventory inventory([&] {
            ++calls;
            if (fail)
                throw std::runtime_error("Transient hardware discovery failure");
            return devices;
        });
        NSDictionary* state = inventory.snapshot(100);
        assert(calls == 1 && [state[@"devicesFresh"] boolValue]);
        assert([state[@"devices"] count] == 1);
        assert([state[@"devices"][0][@"inputChannels"] unsignedIntValue] == 16);
        assert([state[@"devices"][0][@"name"] isEqual:@"Studio input"]);
        assert(inventory.snapshot(2099) == state && calls == 1);

        fail = true;
        NSDictionary* offline = inventory.snapshot(2100);
        assert(calls == 2 && ![offline[@"devicesFresh"] boolValue]);
        assert([offline[@"devicesError"] length] > 0);
        assert([offline[@"devices"] isEqual:state[@"devices"]]);
        assert(inventory.snapshot(4099) == offline && calls == 2);

        fail = false;
        devices.devices[0].label = "Reconnected input";
        state = inventory.snapshot(4100);
        assert(calls == 3 && [state[@"devicesFresh"] boolValue]);
        assert(![state[@"devicesError"] length]);
        assert([state[@"devices"][0][@"name"] isEqual:@"Reconnected input"]);
        devices.devices.clear();
        assert([inventory.snapshot(6100)[@"devices"] count] == 0);

        loopz::macos::DeviceInventory unavailable([]() -> singz::AudioHostInventory {
            throw std::runtime_error("Unavailable on first discovery");
        });
        state = unavailable.snapshot(1);
        assert(![state[@"devicesFresh"] boolValue] && [state[@"devices"] count] == 0);
        {
            singz::AudioHostInventory hardware;
            auto configurable = input;
            configurable.bufferFrames = {16, 1024, 512, 0};
            hardware.devices.push_back(configurable);
            unsigned writes = 0;
            loopz::macos::DeviceInventory buffers(
                [&] {
                    return hardware;
                },
                [&](const auto& uid, uint32_t expected, uint32_t frames) {
                    assert(uid == input.uid &&
                           expected == hardware.devices[0].bufferFrames.preferredFrames);
                    ++writes;
                    hardware.devices[0].bufferFrames.preferredFrames = frames;
                });
            const auto rejected = [&](const auto& uid, uint32_t expected, uint32_t frames) {
                bool caught = false;
                const auto before = writes;
                try {
                    buffers.setBufferFrames(uid, expected, frames);
                } catch (const std::exception&) {
                    caught = true;
                }
                assert(caught && writes == before);
            };
            auto* before = buffers.snapshot(1000);
            assert([before[@"devices"][0][@"bufferFrames"] unsignedIntValue] == 512);
            buffers.setBufferFrames(input.uid, 512, 128);
            assert(writes == 1);
            // Successful changes invalidate the two-second inventory cache.
            assert([buffers.snapshot(1001)[@"devices"][0][@"bufferFrames"] unsignedIntValue] ==
                   128);
            buffers.setBufferFrames(input.uid, 128, 128);
            assert(writes == 1);
            rejected(input.uid, 512, 64); // Stale UI cannot overwrite a newer value.
            rejected(input.uid, 128, 8);
            rejected(input.uid, 128, 192);
            rejected(input.uid, 128, 2048);
            rejected("missing-device", 128, 64);
            rejected(loopz::kDeviceUID, 128, 64);
            rejected("com.lexasoft.loopz.device.mix", 128, 64);
            const auto malformed = [&](std::string_view request) {
                bool caught = false;
                const auto before = writes;
                try {
                    buffers.applyBufferRequest(request);
                } catch (const std::exception&) {
                    caught = true;
                }
                assert(caught && before == writes);
            };
            for (
                const auto* request :
                {"{}", "[]", "null",
                 R"({"version":true,"uid":"hardware-input","expectedFrames":128,"frames":64})",
                 R"({"version":1,"uid":"hardware-input","expectedFrames":128,"frames":32.5})",
                 R"({"version":1,"uid":"hardware-input","expectedFrames":true,"frames":64})",
                 R"({"version":2,"uid":"hardware-input","expectedFrames":128,"frames":64})",
                 R"({"version":1,"uid":"hardware-input","expectedFrames":128,"frames":64,"extra":1})",
                 R"({"version":1,"uid":"hardware-input\u0000other","expectedFrames":128,"frames":64})"})
                malformed(request);
            const auto result = buffers.applyBufferRequest(
                R"({"version":1,"uid":"hardware-input","expectedFrames":128,"frames":64})");
            assert(result.find("\"ok\":true") != std::string::npos && writes == 2);
            assert(hardware.devices[0].bufferFrames.preferredFrames == 64);
            malformed(R"({"version":1,"uid":"hardware-input","expectedFrames":128,"frames":32})");
            hardware.devices.clear();
            rejected(input.uid, 128, 64);
        }
        std::puts("Desktop discovery preserves cached names, excludes self-routing, and recovers");
    }
}
