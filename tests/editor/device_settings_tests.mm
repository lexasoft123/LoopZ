#include <loopz/macos/device_settings.hpp>
#include <loopz/macos/local_file.hpp>
#include <loopz/route_edits.hpp>
#import <Foundation/Foundation.h>

#include <cassert>
#include <fstream>
#include <sys/stat.h>

using namespace loopz;
using namespace loopz::macos;
namespace {
id parsed(const std::string& json) {
    return [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:json.data()
                                                                  length:json.size()]
                                           options:0
                                             error:nil];
}
std::string request(uint32_t before, uint32_t frames) {
    return "{\"version\":1,\"uid\":\"hardware\",\"expectedFrames\":" + std::to_string(before) +
           ",\"frames\":" + std::to_string(frames) + "}";
}
RouteDocument graph() {
    const std::array<edit::Command, 2> commands{
        edit::CreateDevice{"mix", "Mix", 2, 48000, false},
        edit::AddSource{"mix", "input", SourceKind::InputDevice, "hardware", 1}};
    return edit::applyCommands({}, commands);
}
void rejects(const std::function<void()>& operation) {
    bool failed = false;
    try {
        operation();
    } catch (const std::exception&) {
        failed = true;
    }
    assert(failed);
}
} // namespace

int main() {
    @autoreleasepool {
        const auto root = std::filesystem::path(NSTemporaryDirectory().UTF8String) /
                          ("loopz-settings-" + std::string([NSUUID UUID].UUIDString.UTF8String));
        std::filesystem::create_directories(root);
        const auto file = root / "settings.json";
        singz::AudioHostDeviceInfo device;
        device.uid = "hardware";
        device.label = "Studio interface";
        device.inputChannels = device.outputChannels = 2;
        device.nominalSampleRate = 48000;
        device.bufferFrames = {32, 1024, 512, 0};
        singz::AudioHostInventory devices;
        devices.devices.push_back(device);
        bool failWriter = false, failProvider = false, verifySavedBeforeWrite = false;
        unsigned writes = 0, providerCalls = 0;
        const auto provider = [&] {
            ++providerCalls;
            if (failProvider)
                throw std::runtime_error("Device discovery disconnected");
            return devices;
        };
        const auto writer = [&](const auto& uid, uint32_t expected, uint32_t frames) {
            assert(uid == "hardware" && !devices.devices.empty());
            assert(devices.devices[0].bufferFrames.preferredFrames == expected);
            if (verifySavedBeforeWrite)
                assert([parsed(readLocalFile(file, 1024))[@"buffers"][0][@"frames"]
                           unsignedIntValue] == frames);
            if (failWriter)
                throw std::runtime_error("Device disappeared during Apply");
            ++writes;
            devices.devices[0].bufferFrames.preferredFrames = frames;
        };
        {
            DeviceSettings settings(file, provider, writer);
            settings.reload();
            settings.poll(100);
            assert(!writes && !std::filesystem::exists(file));
            assert([parsed(settings.snapshot(101))[@"bufferSettingsPersistence"] boolValue]);
            rejects([&] {
                settings.apply(request(128, 32));
            });
            assert(!writes && !std::filesystem::exists(file));
            verifySavedBeforeWrite = true;
            id reply = parsed(settings.apply(request(512, 32)));
            verifySavedBeforeWrite = false;
            assert([reply[@"saved"] boolValue] && [reply[@"applied"] boolValue]);
            assert(writes == 1 && settings.restoredFor(graph()));
            struct stat info{};
            assert(!stat(file.c_str(), &info) && (info.st_mode & 0777) == 0600);
            auto saved = readLocalFile(file, 1024);
            assert([parsed(saved)[@"buffers"][0][@"frames"] intValue] == 32);
            settings.apply(request(32, 64));
            assert(writes == 2 &&
                   [parsed(readLocalFile(file.string() + ".backup", 1024))[@"buffers"][0][@"frames"]
                       intValue] == 32);
        }
        devices.devices[0].bufferFrames.preferredFrames = 512;
        DeviceSettings restarted(file, provider, writer);
        restarted.reload();
        restarted.poll(200);
        assert(writes == 3 && devices.devices[0].bufferFrames.preferredFrames == 64);
        assert(restarted.restoredFor(graph()));
        restarted.retryNow();
        const auto previousCalls = providerCalls;
        restarted.poll(201);
        (void)restarted.snapshot(202);
        assert(providerCalls == previousCalls + 1 && writes == 3);
        devices.devices.clear();
        restarted.retryNow();
        restarted.poll(300);
        assert(writes == 3 && !restarted.restoredFor(graph()) && restarted.restoredFor({}));
        auto replacement = device;
        replacement.uid = "different-identity";
        devices.devices.push_back(replacement);
        restarted.retryNow();
        restarted.poll(400);
        assert(writes == 3 && !restarted.restoredFor(graph()));
        devices.devices[0] = device;
        restarted.retryNow();
        restarted.poll(500);
        assert(writes == 4 && restarted.restoredFor(graph()));
        failWriter = true;
        id pending = parsed(restarted.apply(request(64, 32)));
        assert([pending[@"saved"] boolValue] && ![pending[@"applied"] boolValue]);
        assert(!restarted.restoredFor(graph()));
        assert([parsed(readLocalFile(file, 1024))[@"buffers"][0][@"frames"] intValue] == 32);
        failWriter = false;
        restarted.retryNow();
        restarted.poll(600);
        assert(writes == 5 && restarted.restoredFor(graph()));
        failProvider = true;
        restarted.retryNow();
        restarted.poll(700);
        assert(writes == 5 && !restarted.restoredFor(graph()));
        failProvider = false;
        restarted.retryNow();
        restarted.poll(800);
        assert(writes == 5 && restarted.restoredFor(graph()));
        devices.devices[0].bufferFrames = {128, 1024, 512, 0};
        restarted.retryNow();
        restarted.poll(900);
        assert(writes == 5 && !restarted.restoredFor(graph()));
        devices.devices[0] = device;
        restarted.retryNow();
        restarted.poll(1000);
        assert(writes == 6 && restarted.restoredFor(graph()));

        writeLocalFileAtomic(file, "broken");
        DeviceSettings recovered(file, provider, writer);
        recovered.reload();
        recovered.poll(1100);
        assert([parsed(recovered.snapshot(1101))[@"bufferSettingsRecoveredBackup"] boolValue]);
        assert(devices.devices[0].bufferFrames.preferredFrames == 64);
        recovered.apply(request(64, 32));
        assert([parsed(readLocalFile(file.string() + ".backup", 1024))[@"buffers"][0][@"frames"]
                   intValue] == 64);
        assert(![parsed(recovered.snapshot(1200))[@"bufferSettingsRecoveredBackup"] boolValue]);
        const auto newer = "{\"version\":3,\"future\":true,\"extra\":1,\"buffers\":[]}";
        writeLocalFileAtomic(file, newer);
        DeviceSettings future(file, provider, writer);
        future.reload();
        future.poll(1300);
        assert([parsed(future.snapshot(1301))[@"bufferSettingsError"] length]);
        assert(!future.restoredFor(graph()));
        rejects([&] {
            future.apply(request(32, 64));
        });
        assert(readLocalFile(file, 1024) == newer);
        writeLocalFileAtomic(file, "broken");
        writeLocalFileAtomic(file.string() + ".backup", "also broken");
        DeviceSettings corrupt(file, provider, writer);
        corrupt.reload();
        rejects([&] {
            corrupt.apply(request(32, 64));
        });
        assert(readLocalFile(file, 1024) == "broken");
        std::filesystem::remove(file);
        std::filesystem::remove(file.string() + ".backup");
        std::filesystem::create_symlink(root / "elsewhere", file);
        DeviceSettings linked(file, provider, writer);
        linked.reload();
        rejects([&] {
            linked.apply(request(32, 64));
        });
        assert(std::filesystem::is_symlink(file) && !std::filesystem::exists(root / "elsewhere"));
        std::filesystem::remove(file);
        const auto blocked = root / "not-a-directory";
        writeLocalFileAtomic(blocked, "keep");
        DeviceSettings unwritable(blocked / "settings.json", provider, writer);
        unwritable.reload();
        const auto previousWrites = writes;
        rejects([&] {
            unwritable.apply(request(32, 64));
        });
        assert(writes == previousWrites && devices.devices[0].bufferFrames.preferredFrames == 32);
        const auto locked = root / "locked";
        std::filesystem::create_directory(locked);
        const auto valid = R"({"version":1,"buffers":[{"uid":"hardware","frames":32}]})";
        writeLocalFileAtomic(locked / "settings.json", valid);
        DeviceSettings storageFailure(locked / "settings.json", provider, writer);
        storageFailure.reload();
        assert(!chmod(locked.c_str(), 0500));
        rejects([&] {
            storageFailure.apply(request(32, 64));
        });
        assert(!chmod(locked.c_str(), 0700));
        assert(writes == previousWrites && readLocalFile(locked / "settings.json", 1024) == valid);
        writeLocalFileAtomic(file, valid);
        writeLocalFileAtomic(file.string() + ".backup", newer);
        DeviceSettings newerBackup(file, provider, writer);
        newerBackup.reload();
        rejects([&] {
            newerBackup.apply(request(32, 64));
        });
        assert(writes == previousWrites && readLocalFile(file.string() + ".backup", 1024) == newer);
        std::filesystem::remove(file.string() + ".backup");
        for (
            const auto* invalid :
            {R"({"version":1,"buffers":[{"uid":"hardware","frames":32},{"uid":"hardware","frames":64}]})",
             R"({"version":1,"buffers":[{"uid":"com.lexasoft.loopz.device.mix","frames":32}]})",
             R"({"version":true,"buffers":[]})",
             R"({"version":1,"buffers":[{"uid":"hardware","frames":true}]})"}) {
            writeLocalFileAtomic(file, invalid);
            DeviceSettings invalidSettings(file, provider, writer);
            invalidSettings.reload();
            invalidSettings.poll(1400);
            assert([parsed(invalidSettings.snapshot(1401))[@"bufferSettingsError"] length]);
            assert(writes == previousWrites);
        }
        writeLocalFileAtomic(file, std::string(1024 * 1024 + 1, 'x'));
        DeviceSettings oversized(file, provider, writer);
        oversized.reload();
        assert([parsed(oversized.snapshot(1500))[@"bufferSettingsError"] length]);
        assert(writes == previousWrites);

        // The global choice follows configured hardware, independent of the default output.
        {
            const auto globalFile = root / "global.json";
            auto input = device, output = device, unused = device;
            input.uid = "input";
            output.uid = "output";
            unused.uid = "unused";
            singz::AudioHostInventory hardware;
            hardware.devices = {input, output, unused};
            auto document = edit::applyCommands(
                {}, std::array<edit::Command, 4>{
                        edit::CreateDevice{"mix", "Mix", 2, 48000, false},
                        edit::AddSource{"mix", "input", SourceKind::InputDevice, "input", 2},
                        edit::AddMonitor{"mix", "monitor", "output", 2},
                        edit::AddMonitor{"mix", "duplicate", "input", 2}});
            unsigned globalWrites = 0;
            bool failOutput = false;
            auto globalProvider = [&] {
                return hardware;
            };
            auto globalWriter = [&](const std::string& uid, uint32_t expected, uint32_t frames) {
                assert([parsed(readLocalFile(globalFile, 4096))[@"routingBufferFrames"]
                           unsignedIntValue] == frames);
                assert(uid != "unused");
                if (failOutput && uid == "output")
                    throw std::runtime_error("Output temporarily unavailable");
                for (auto& item : hardware.devices)
                    if (item.uid == uid) {
                        assert(item.bufferFrames.preferredFrames == expected);
                        item.bufferFrames.preferredFrames = frames;
                        ++globalWrites;
                    }
            };
            DeviceSettings global(globalFile, globalProvider, globalWriter);
            global.setRoutingDocument(document);
            global.reload();
            global.poll(100);
            id result = parsed(global.apply(R"({"version":2,"frames":32})"));
            assert([result[@"applied"] boolValue] && globalWrites == 2);
            assert(global.hardwareGeneration() == 2);
            assert(hardware.devices[2].bufferFrames.preferredFrames == 512);
            id snapshot = parsed(global.snapshot(101));
            assert([snapshot[@"routingBufferDevices"] count] == 2);
            assert([snapshot[@"routingBufferFrames"] intValue] == 32);
            assert(global.restoredFor(document));
            // Validate the entire set before any device is changed or intent is saved.
            hardware.devices[1].bufferFrames.minimumFrames = 128;
            rejects([&] {
                global.apply(R"({"version":2,"frames":64})");
            });
            assert(globalWrites == 2);
            assert([parsed(readLocalFile(globalFile, 4096))[@"routingBufferFrames"] intValue] ==
                   32);
            hardware.devices[1].bufferFrames.minimumFrames = 32;
            failOutput = true;
            result = parsed(global.apply(R"({"version":2,"frames":64})"));
            assert([result[@"saved"] boolValue] && ![result[@"applied"] boolValue]);
            assert(globalWrites == 3 && !global.restoredFor(document));
            failOutput = false;
            global.retryNow();
            global.poll(200);
            assert(globalWrites == 4 && global.restoredFor(document));
            hardware.devices.erase(hardware.devices.begin() + 1);
            global.retryNow();
            global.poll(300);
            assert(!global.restoredFor(document));
            hardware.devices.push_back(output);
            global.retryNow();
            global.poll(400);
            assert(globalWrites == 5 && global.restoredFor(document));
            // Restart restores the same choice, and new used devices inherit it.
            hardware.devices[0].bufferFrames.preferredFrames = 512;
            DeviceSettings restartedGlobal(globalFile, globalProvider, globalWriter);
            restartedGlobal.setRoutingDocument(document);
            restartedGlobal.reload();
            restartedGlobal.poll(500);
            assert(globalWrites == 6 && restartedGlobal.restoredFor(document));
            document = edit::applyCommands(
                document, std::array<edit::Command, 1>{edit::AddSource{
                              "mix", "later", SourceKind::InputDevice, "unused", 2}});
            // Use a writer that accepts the formerly unused device only after it is added.
            DeviceSettings added(globalFile, globalProvider,
                                 [&](const auto& uid, uint32_t expected, uint32_t frames) {
                                     assert(uid == "unused" && expected == 512 && frames == 64);
                                     hardware.devices[1].bufferFrames.preferredFrames = frames;
                                     ++globalWrites;
                                 });
            added.setRoutingDocument(document);
            added.reload();
            added.poll(600);
            assert(globalWrites == 7 && added.restoredFor(document));
            rejects([&] {
                added.apply(R"({"version":2,"frames":true})");
            });
            rejects([&] {
                added.apply(R"({"version":2,"frames":33})");
            });
            rejects([&] {
                added.apply(R"({"version":2,"frames":32,"extra":1})");
            });
            DeviceSettings empty(root / "empty-global.json", globalProvider, globalWriter);
            empty.reload();
            empty.poll(700);
            result = parsed(empty.apply(R"({"version":2,"frames":32})"));
            assert([result[@"saved"] boolValue] && [result[@"applied"] boolValue]);
            assert(globalWrites == 7);
        }
        std::filesystem::remove_all(root);
        std::puts("Device buffer choices persist, restore on reconnect, and preserve corrupt/newer "
                  "files");
    }
}
