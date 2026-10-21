#include <loopz/macos/audio_host_identity.hpp>
#include "../../native/platform/macos/desktop/maintenance_files.hpp"
#include "../../native/platform/macos/desktop/audio_maintenance.hpp"
#include <loopz/macos/route_store.hpp>
#include "../../native/platform/macos/desktop/maintenance_host_policy.hpp"
#import <Foundation/Foundation.h>
#include <cassert>
#include <fstream>
#include <functional>
#include <unistd.h>
#include <sys/stat.h>

namespace {
namespace fs = std::filesystem;
using namespace loopz::macos;
void put(const fs::path& path, const std::string& data) {
    std::ofstream file(path);
    file << data;
    file.close();
    assert(file.good());
}
std::string get(const fs::path& path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), {}};
}
void rejects(const std::function<void()>& operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::exception&) {
        rejected = true;
    }
    assert(rejected);
}
struct Fixture {
    fs::path root;
    std::vector<AudioInstallEntry> entries;
    Fixture() {
        auto pattern = (fs::temp_directory_path() / "loopz-native-maintenance-XXXXXX").string();
        assert(mkdtemp(pattern.data()));
        root = fs::canonical(pattern);
        fs::create_directory(root / "recovery");
        fs::create_directory(root / "source");
        fs::create_directory(root / "installed");
        for (int i = 0; i < 2; ++i) {
            const auto name = std::to_string(i);
            entries.push_back({root / "source" / name, root / "installed" / name});
            put(entries.back().source, "new-" + name);
            put(entries.back().target, "old-" + name);
        }
    }
    ~Fixture() {
        std::error_code e;
        fs::remove_all(root, e);
    }
    AudioInstallTransaction transaction() {
        return {root / "recovery", entries};
    }
};
void normalCommitAndIdempotency() {
    Fixture f;
    auto first = f.transaction();
    first.prepare();
    first.promote();
    first.commit();
    assert(get(f.entries[0].target) == "new-0");
    struct stat before{}, after{};
    assert(!stat(f.entries[0].target.c_str(), &before));
    auto repeated = f.transaction();
    repeated.prepare();
    repeated.promote();
    repeated.commit();
    assert(!stat(f.entries[0].target.c_str(), &after) && before.st_ino == after.st_ino);
    assert(!fs::exists(f.root / "recovery/maintenance.plist"));
}
void interruptionRestoresOriginalAndMissingComponents() {
    Fixture f;
    fs::remove(f.entries[1].target);
    {
        auto interrupted = f.transaction();
        interrupted.prepare();
        interrupted.promote();
    }
    assert(get(f.entries[0].target) == "new-0" && fs::exists(f.entries[1].target));
    auto recovery = f.transaction();
    assert(recovery.recover());
    assert(get(f.entries[0].target) == "old-0" && !fs::exists(f.entries[1].target));
    assert(!recovery.recover());
    recovery.prepare();
    recovery.promote();
    recovery.commit();
    assert(get(f.entries[1].target) == "new-1");
}
void failedPromotionRestoresOnlyItsOwnChanges() {
    Fixture f;
    auto transaction = f.transaction();
    transaction.prepare();
    put(f.entries[1].target, "external-change");
    rejects([&] {
        transaction.promote();
    });
    transaction.rollback();
    assert(get(f.entries[0].target) == "old-0");
    assert(get(f.entries[1].target) == "external-change");
}
void unknownReplacementPreservesRecovery() {
    Fixture f;
    {
        auto transaction = f.transaction();
        transaction.prepare();
        transaction.promote();
    }
    put(f.entries[0].target, "external-change");
    auto recovery = f.transaction();
    rejects([&] {
        recovery.recover();
    });
    assert(get(f.entries[0].target) == "external-change");
    assert(fs::exists(f.root / "recovery/maintenance.plist"));
}
void missingPromotedFileCanRecover() {
    Fixture f;
    {
        auto transaction = f.transaction();
        transaction.prepare();
        transaction.promote();
    }
    fs::remove(f.entries[0].target);
    auto recovery = f.transaction();
    assert(recovery.recover());
    assert(get(f.entries[0].target) == "old-0");
}
void linksAndEscapingJournalAreRejected() {
    Fixture f;
    fs::remove(f.entries[0].source);
    fs::create_symlink(f.entries[1].source, f.entries[0].source);
    auto transaction = f.transaction();
    rejects([&] {
        transaction.prepare();
    });
    assert(get(f.entries[0].target) == "old-0");
    fs::remove(f.entries[0].source);
    put(f.entries[0].source, "new-0");
    auto valid = f.transaction();
    valid.prepare();
    const auto path = f.root / "recovery/maintenance.plist";
    NSData* data = [NSData dataWithContentsOfFile:@(path.c_str())];
    NSMutableDictionary* journal =
        [[NSPropertyListSerialization propertyListWithData:data
                                                   options:NSPropertyListMutableContainers
                                                    format:nil
                                                     error:nil] mutableCopy];
    journal[@"workspace"] = @"../outside";
    NSData* altered =
        [NSPropertyListSerialization dataWithPropertyList:journal
                                                   format:NSPropertyListBinaryFormat_v1_0
                                                  options:0
                                                    error:nil];
    assert([altered writeToFile:@(path.c_str()) atomically:YES]);
    auto recovery = f.transaction();
    rejects([&] {
        recovery.recover();
    });
    assert(get(f.entries[0].target) == "old-0");
}
void unsafeRecoveryPermissionsAreRejected() {
    Fixture f;
    auto transaction = f.transaction();
    transaction.prepare();
    transaction.promote();
    const auto journal = f.root / "recovery/maintenance.plist";
    assert(!chmod(journal.c_str(), 0666));
    auto recovery = f.transaction();
    rejects([&] {
        recovery.recover();
    });
    assert(get(f.entries[0].target) == "new-0");
    assert(!chmod(journal.c_str(), 0600));
    assert(!chmod(transaction.workspace().c_str(), 0755));
    rejects([&] {
        recovery.recover();
    });
    assert(!chmod(transaction.workspace().c_str(), 0700));
    auto trusted = f.transaction();
    assert(trusted.recover());
    assert(get(f.entries[0].target) == "old-0");
}
void mappedImageSafety() {
    const auto inventory = [](const std::vector<std::string>& fields) {
        std::string result;
        for (const auto& field : fields) {
            result += field;
            result += '\0';
        }
        return result;
    };
    const std::string image =
        "n/Library/Audio/Plug-Ins/HAL/LoopZAudio.driver/Contents/MacOS/LoopZAudio";
    auto fields = std::vector<std::string>{"p123", "ftxt", "D100000a", "i987", image};
    const auto mapped = loopZMappedImage(inventory(fields), 123);
    assert(mapped && mapped->device == 0x100000a && mapped->inode == 987);
    rejects([&] {
        (void)loopZMappedImage(inventory(fields), 124);
    });
    fields[1] = "f12";
    assert(!loopZMappedImage(inventory(fields), 123));
    fields[1] = "ftxt";
    fields.push_back("ftxt");
    fields.push_back("n/Library/Audio/Plug-Ins/HAL/Other.driver/Contents/MacOS/Other");
    rejects([&] {
        (void)loopZMappedImage(inventory(fields), 123);
    });
    fields.resize(5);
    fields.erase(fields.begin() + 2);
    rejects([&] {
        (void)loopZMappedImage(inventory(fields), 123);
    });
    fields = {"p123", "fmem", "D100000a", "i987",
              "n/Library/Application "
              "Support/LoopZ/.maintenance-123/old/5/Contents/MacOS/LoopZAudio (deleted)"};
    assert(loopZMappedImage(inventory(fields), 123) == mapped);
    fields[4] = "n/System/Volumes/Data/Library/Audio/Plug-Ins/HAL/LoopZAudio.driver/Contents/MacOS/"
                "LoopZAudio";
    assert(loopZMappedImage(inventory(fields), 123) == mapped);
    fields.push_back("ftxt");
    fields.push_back("D100000a");
    fields.push_back("i988");
    fields.push_back(image);
    rejects([&] {
        (void)loopZMappedImage(inventory(fields), 123);
    });
}
} // namespace
int main() {
    @autoreleasepool {
        const auto validation = decodeRouteDocumentJSON(audioValidationDocument(true));
        assert(validation.devices.size() == 1 && validation.devices[0].routing.outputChannels == 2);
        assert(validation.devices[0].bindings.empty() &&
               validation.devices[0].routing.sources.empty() &&
               validation.devices[0].monitors.empty());
        assert(decodeRouteDocumentJSON(audioValidationDocument(false)).devices.empty());
        const std::string audioBase =
            "/System/Library/Frameworks/CoreAudio.framework/Versions/A/XPCServices/";
        assert(isolatedAudioHostPath(audioBase +
                                     "com.apple.audio.Core-Audio-Driver-Service.xpc/Contents/MacOS/"
                                     "com.apple.audio.Core-Audio-Driver-Service"));
        assert(isolatedAudioHostPath(
            audioBase + "com.apple.audio.Core-Audio-Driver-Service.helper.xpc/Contents/MacOS/"
                        "com.apple.audio.Core-Audio-Driver-Service.helper"));
        assert(!isolatedAudioHostPath("/usr/sbin/coreaudiod"));
        assert(!isolatedAudioHostPath("/tmp/com.apple.audio.Core-Audio-Driver-Service"));
        {
            Fixture fixture;
            const auto directory = fixture.root / "ownership";
            fs::create_directories(directory / "nested");
            const auto file = directory / "nested/file";
            put(file, "owned payload");
            chmod(file.c_str(), 0600);
            protectAudioTree(directory, getuid(), getgid());
            struct stat info{};
            assert(!lstat(file.c_str(), &info) && (info.st_mode & 0777) == 0644);
            fs::create_symlink(fixture.root / "source/0", directory / "escape");
            rejects([&] {
                protectAudioTree(directory, getuid(), getgid());
            });
            assert(fs::is_symlink(directory / "escape"));
            rejects([&] {
                protectAudioTree(directory / "escape", getuid(), getgid());
            });
        }
        normalCommitAndIdempotency();
        interruptionRestoresOriginalAndMissingComponents();
        failedPromotionRestoresOnlyItsOwnChanges();
        unknownReplacementPreservesRecovery();
        missingPromotedFileCanRecover();
        linksAndEscapingJournalAreRejected();
        unsafeRecoveryPermissionsAreRejected();
        mappedImageSafety();
    }
}
