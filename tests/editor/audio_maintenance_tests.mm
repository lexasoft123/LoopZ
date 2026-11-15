#include <loopz/macos/audio_host_identity.hpp>
#include "../../native/platform/macos/desktop/maintenance_files.hpp"
#include "../../native/platform/macos/desktop/audio_maintenance.hpp"
#include "../../native/platform/macos/desktop/audio_maintenance_testing.hpp"
#include "../../native/platform/macos/desktop/engine_migration.hpp"
#include <loopz/macos/route_store.hpp>
#include "../../native/platform/macos/desktop/maintenance_host_policy.hpp"
#import <Foundation/Foundation.h>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <functional>
#include <unistd.h>
#include <fcntl.h>
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
// Real transaction/journal and policy, with all destinations and consent metadata
// isolated. These fixtures never launch installed services or request OS consent.
struct MigrationFixture : Fixture {
    fs::path legacy = root / "installed/legacy.app";
    fs::path engine = root / "installed/new.app";
    fs::path job = root / "installed/engine.plist";
    MigrationFixture(bool fresh = false) {
        fs::create_directory(root / "source/engine.app");
        put(root / "source/engine.app/executable", "signed-engine-fixture");
        put(root / "source/engine.plist", engine.string());
        entries = {{root / "source/engine.app", engine},
                   {root / "source/engine.plist", job},
                   {{}, legacy}};
        if (!fresh) {
            fs::create_directory(legacy);
            put(legacy / "executable", "legacy-engine");
            put(job, legacy.string());
        }
    }
};
void jobPublicationPinsTheUserDirectory() {
    MigrationFixture f;
    const auto directory = f.root / "Session", moved = f.root / "moved",
               outside = f.root / "outside";
    fs::create_directory(directory);
    fs::create_directory(outside);
    f.job = directory / "engine.plist";
    put(f.job, "old-job");
    put(outside / "engine.plist", "old-job");
    const int fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    assert(fd >= 0);
    f.entries[1] = {f.root / "source/engine.plist", f.job, fd};
    auto transaction = f.transaction();
    transaction.prepare();
    fs::rename(directory, moved);
    fs::create_directory_symlink(outside, directory);
    rejects([&] {
        transaction.promote();
    }); // Public-path sync detects the substituted parent.
    assert(get(outside / "engine.plist") == "old-job");
    assert(get(moved / "engine.plist") != "old-job");
    fs::remove(directory);
    fs::rename(moved, directory);
    transaction.rollback();
    assert(get(f.job) == "old-job");
    assert(!close(fd));
}

NSDictionary* readPlist(const fs::path& path) {
    return [NSPropertyListSerialization
        propertyListWithData:[NSData dataWithContentsOfFile:@(path.c_str())]
                     options:0
                      format:nil
                       error:nil];
}
void writePlist(const fs::path& path, NSDictionary* value) {
    NSData* bytes =
        [NSPropertyListSerialization dataWithPropertyList:value
                                                   format:NSPropertyListBinaryFormat_v1_0
                                                  options:0
                                                    error:nil];
    assert(bytes && [bytes writeToFile:@(path.c_str()) atomically:YES]);
    chmod(path.c_str(), 0600);
}
std::string json(NSDictionary* value) {
    NSData* bytes = [NSJSONSerialization dataWithJSONObject:value options:0 error:nil];
    assert(bytes);
    return {static_cast<const char*>(bytes.bytes), bytes.length};
}
size_t maintenanceInvocations = 0;
struct MaintenanceFixture : Fixture {
    fs::path app = root / "Applications/LoopZ.app";
    fs::path payload = app / "Contents/Resources/AudioComponents";
    fs::path support = root / "Library/Application Support/LoopZ";
    fs::path legacy = root / "Applications/LoopZ Engine.app";
    fs::path preferred = support / "LoopZ Engine.app";
    fs::path job = root / "home/Library/Application Support/LoopZ/Session/engine.plist";
    fs::path routes = job.parent_path().parent_path() / "routes.json";
    fs::path installedDriver = root / "HAL/LoopZAudio.driver";
    AudioMaintenanceFixture adapters;
    bool loaded{true}, stopFails{false}, stopErrorsAfterRemoval{false}, broker{true};
    bool baselineFails{false}, signatureFails{false}, capabilityFails{false}, consentLost{false},
        captureFails{false}, registered{false};
    NSString* baseline{@"not-determined"};
    NSString* currentDefault{@"physical-out"};
    bool statusFailsAfterBootstrap{false}, bootstrapFails{false};
    int defaultsRestored{}, statusQueries{};
    int stops{}, bootstraps{}, activations{}, permissionQueries{}, capabilityQueries{};
    bool applications;
    MaintenanceFixture(bool appCapture = false, bool fresh = false) : applications(appCapture) {
        fs::create_directories(payload);
        fs::create_directories(support);
        fs::create_directories(installedDriver);
        fs::create_directories(root / "LaunchDaemons");
        fs::create_directories(job.parent_path());
        fs::create_directories(root / "Users");
        for (const auto* name :
             {"loopz-routing-broker", "loopz-config", "loopz-routing-install-check",
              "routing-service.json", "com.lexasoft.loopz.routing.plist"}) {
            put(payload / name, "new-component");
            put(name == std::string("com.lexasoft.loopz.routing.plist")
                    ? root / "LaunchDaemons" / name
                    : support / name,
                "old-component");
        }
        fs::create_directories(payload / "LoopZAudio.driver");
        put(payload / "LoopZAudio.driver/binary", "new-driver");
        put(installedDriver / "binary", "old-driver");
        fs::create_directories(payload / "LoopZ Engine.app/Contents/MacOS");
        put(payload / "LoopZ Engine.app/Contents/MacOS/LoopZ Engine", "new-engine");
        chmod((payload / "LoopZ Engine.app/Contents/MacOS/LoopZ Engine").c_str(), 0755);
        if (!fresh) {
            fs::create_directories(legacy / "Contents/MacOS");
            put(legacy / "Contents/MacOS/LoopZ Engine", "old-engine");
            chmod((legacy / "Contents/MacOS/LoopZ Engine").c_str(), 0755);
        } else
            loaded = false;
        loopz::RouteDocument document;
        loopz::VirtualDeviceDefinition device;
        device.id = "fixture";
        device.name = "Fixture";
        if (applications) {
            loopz::SourceRouting source;
            source.id = "app";
            device.routing.sources.push_back(source);
            device.bindings.push_back({"app", loopz::SourceKind::Application, "fixture.app"});
        }
        document.devices.push_back(device);
        put(routes, encodeRouteDocumentJSON(document));
        if (!fresh)
            writePlist(job, definition(legacy));
        adapters.root = root;
        adapters.verify = [&](const fs::path& path, NSString* identifier) {
            if ([identifier isEqual:@(engineBundleIdentifier)] &&
                ((baselineFails && path == legacy) || (signatureFails && path != legacy)))
                throw std::runtime_error("fixture signature/requirement failure");
        };
        adapters.command = [&](const auto& args, bool) {
            return execute(args);
        };
        adapters.catalog = [&] {
            return @{
                @"routingBlockingRoutes" : @[],
                @"loopzBlockingRoutes" : @[],
                @"routingDevices" : @[],
                @"defaultInput" : @"physical-in",
                @"defaultOutput" : currentDefault,
                @"defaultSystemOutput" : currentDefault,
                @"routingPlugIn" : @{@"registered" : @(registered)}
            };
        };
        adapters.restoreDefaults = [&](NSDictionary* before) {
            ++defaultsRestored;
            currentDefault = before[@"defaultOutput"];
        };
        adapters.validateGraph = [] {};
        adapters.activate = [&] {
            ++activations;
        };
    }
    NSDictionary* definition(const fs::path& bundle) const {
        return @{
            @"Label" : @(engineJobLabel),
            @"ProgramArguments" : @[
                @((bundle / "Contents/MacOS/LoopZ Engine").c_str()), @"--routing-document",
                @(routes.c_str()), @"--editor-service", @"true", @"--block-frames", @"64"
            ],
            @"RunAtLoad" : @YES,
            @"KeepAlive" : @YES,
            @"MachServices" : @{@(engineControlService) : @YES},
            @"ProcessType" : @"Interactive",
            @"ThrottleInterval" : @10,
            @"ExitTimeOut" : @5,
            @"StandardOutPath" : @"/dev/null",
            @"StandardErrorPath" : @"/dev/null"
        };
    }
    bool newJob() const {
        return [readPlist(job)[@"ProgramArguments"][0]
            isEqual:@((preferred / "Contents/MacOS/LoopZ Engine").c_str())];
    }
    SessionCommandResult execute(const std::vector<std::string>& args) {
        assert(!args.empty());
        if (args.back() == "--runtime-capabilities") {
            ++capabilityQueries;
            return {0, capabilityFails ? "{}" : "{\"backgroundSession\":1}"};
        }
        if (args.back() == "--permission-status") {
            ++permissionQueries;
            if (baselineFails && fs::path(args[0]).string().find(legacy.string()) == 0)
                throw std::runtime_error("fixture crashing baseline query");
            return {
                0, json(@{
                    @"microphone" : consentLost &&
                            fs::path(args[0]).string().find(preferred.string()) == 0
                        ? @"not-determined"
                        : baseline
                })
            };
        }
        if (args.back() == "--routing-status") {
            ++statusQueries;
            if (statusFailsAfterBootstrap && bootstraps)
                return {1, "routing status unavailable"};
            NSData* bytes = [NSData dataWithContentsOfFile:@(routes.c_str())];
            NSDictionary* document = [NSJSONSerialization JSONObjectWithData:bytes
                                                                     options:0
                                                                       error:nil];
            return {
                0, json(@{
                    @"document" : document,
                    @"runtime" : @{
                        @"started" : @YES,
                        @"restored" : @YES,
                        @"optionalTiming" : NSNull.null,
                        @"permissions" : @{
                            @"microphone" : consentLost && newJob() ? @"not-determined" : baseline
                        },
                        @"sources" : applications ? @[ @{
                            @"kind" : @"application",
                            @"demanded" : @YES,
                            @"connected" : @(!(captureFails && newJob())),
                            @"complete" : @(!(captureFails && newJob())),
                            @"error" : captureFails && newJob()
                                ? @"capture permission not-determined"
                                : @""
                        } ]
                                                  : @[]
                    }
                })
            };
        }
        assert(args[0] == "/bin/launchctl");
        const bool session = args.size() > 2 && args[2].find("gui/") == 0;
        if (args[1] == "print") {
            if (!(session ? loaded : broker))
                return {113, "Could not find service"};
            if (!session)
                return {0, "broker"};
            NSDictionary* stored = readPlist(job);
            NSArray* arguments = stored[@"ProgramArguments"];
            std::string output = "path = " + job.string() +
                                 "\nprogram = " + [arguments[0] UTF8String] + "\narguments = {\n";
            for (NSString* argument in arguments)
                output += std::string(argument.UTF8String) + "\n";
            return {0, output + "}\n"};
        }
        if (args[1] == "bootout") {
            if (session) {
                ++stops;
                if (!stopFails)
                    loaded = false;
                if (stopErrorsAfterRemoval)
                    return {1, "bootout failed after removal"};
            } else
                broker = false;
            return {0, ""};
        }
        assert(args[1] == "bootstrap");
        if (session) {
            if (bootstrapFails)
                return {1, "bootstrap unavailable"};
            loaded = true;
            ++bootstraps;
        } else
            broker = true;
        return {0, ""};
    }
    NSDictionary* run(bool activate = false) {
        ++maintenanceInvocations;
        return maintainAudioComponents(app, activate, getuid(), adapters);
    }
};
void maintenanceEntryPointRegressions() {
    {
        MaintenanceFixture f(false, true);
        assert([f.run()[@"ok"] isEqual:@YES]);
        assert(readableEngineBundle(f.preferred) && !fs::is_symlink(f.preferred));
    }
    {
        MaintenanceFixture f;
        fs::copy(f.payload / "LoopZ Engine.app", f.preferred, fs::copy_options::recursive);
        assert([f.run()[@"ok"] isEqual:@YES] && f.newJob() && f.loaded);
        assert(!fs::exists(f.legacy));
    }
    // A grant lost by both old and new identities must not trap rollback or the
    // next Repair. Defaults and stale-context cleanup still complete.
    {
        MaintenanceFixture f;
        f.baseline = @"authorized";
        f.adapters.checkpoint = [&](const char* phase) {
            if (std::string(phase) == "promoted") {
                f.baseline = @"denied";
                f.currentDefault = @"temporary-fallback";
            }
        };
        bool lost = false;
        try {
            f.run();
        } catch (const EngineConsentLost&) {
            lost = true;
        }
        assert(lost && !f.newJob() && f.defaultsRestored > 0);
        assert([f.currentDefault isEqual:@"physical-out"]);
        assert(!fs::exists(f.support / "maintenance-session.plist"));
        f.adapters.checkpoint = {};
        assert([f.run()[@"ok"] isEqual:@YES]);
        assert(f.permissionQueries == 3); // old authorized, new denied, fresh denied baseline
    }
    {
        MaintenanceFixture f;
        f.baseline = @"authorized";
        f.adapters.checkpoint = [&](const char* phase) {
            if (std::string(phase) == "promoted") {
                f.baseline = @"denied";
                f.bootstrapFails = true;
                f.currentDefault = @"temporary-fallback";
            }
        };
        rejects([&] {
            f.run();
        });
        assert(f.defaultsRestored > 0 && [f.currentDefault isEqual:@"physical-out"]);
        assert(!fs::exists(f.support / "maintenance-session.plist") &&
               !fs::exists(f.support / "maintenance.plist"));
        f.bootstrapFails = false;
        f.adapters.checkpoint = {};
        assert([f.run()[@"ok"] isEqual:@YES] && f.loaded);
    }
    // Existing new-path jobs stay new for this account, even when another
    // account needs legacy or when legacy is recreated later.
    for (bool resumed : {false, true}) {
        MaintenanceFixture f(true);
        fs::copy(f.payload / "LoopZ Engine.app", f.preferred, fs::copy_options::recursive);
        writePlist(f.job, f.definition(f.preferred));
        f.captureFails = true; // Existing permission errors do not turn Repair into migration.
        const auto otherJob =
            f.root / "Users/other/Library/Application Support/LoopZ/Session/engine.plist";
        fs::create_directories(otherJob.parent_path());
        writePlist(otherJob, f.definition(f.legacy));
        if (resumed) {
            f.adapters.checkpoint = [](const char* phase) {
                if (std::string(phase) == "promoted")
                    throw AudioMaintenanceInterruption();
            };
            rejects([&] {
                f.run();
            });
            f.adapters.checkpoint = {};
        }
        auto result = f.run();
        assert([result[@"ok"] isEqual:@YES] && [result[@"migrationPending"] isEqual:@NO]);
        assert(f.newJob() && readableEngineBundle(f.preferred) && readableEngineBundle(f.legacy));
    }
    {
        MaintenanceFixture f(true);
        f.adapters.migrateApplicationCapture = true;
        assert([f.run()[@"ok"] isEqual:@YES]);
        assert(!fs::exists(f.legacy));
        fs::copy(f.payload / "LoopZ Engine.app", f.legacy, fs::copy_options::recursive);
        f.adapters.migrateApplicationCapture = false;
        f.captureFails = true;
        assert([f.run()[@"migrationPending"] isEqual:@NO] && f.newJob());
        assert(readableEngineBundle(f.legacy));
    }
    // Pending activation stops the replacement broker before reverse renames
    // and restarts the restored broker before returning.
    {
        MaintenanceFixture f;
        f.registered = true;
        bool observed = false;
        f.adapters.checkpoint = [&](const char* phase) {
            if (std::string(phase) == "activation-rolled-back") {
                observed = true;
                assert(!f.broker && get(f.support / "loopz-routing-broker") == "old-component");
            }
        };
        assert([f.run()[@"activationRequired"] isEqual:@YES]);
        assert(observed && f.broker);
    }
    // Gate-off pending activation cannot commit unverified routing.
    {
        MaintenanceFixture f(true);
        f.registered = true;
        f.statusFailsAfterBootstrap = true;
        rejects([&] {
            f.run();
        });
        assert(f.statusQueries > 1 && get(f.installedDriver / "binary") == "old-driver");
        assert(!f.newJob() && !fs::exists(f.support / "maintenance-session.plist"));
    }
    // A context without a journal is stale. Re-snapshot defaults, state and
    // consent instead of restoring values saved before later user changes.
    {
        MaintenanceFixture f;
        f.baseline = @"authorized";
        f.adapters.checkpoint = [](const char* phase) {
            if (std::string(phase) == "verified")
                throw std::runtime_error("verification interrupted");
            if (std::string(phase) == "rolled-back")
                throw AudioMaintenanceInterruption();
        };
        rejects([&] {
            f.run();
        });
        assert(fs::exists(f.support / "maintenance-session.plist") &&
               !fs::exists(f.support / "maintenance.plist"));
        f.currentDefault = @"later-user-choice";
        f.baseline = @"denied";
        f.adapters.checkpoint = {};
        assert([f.run()[@"ok"] isEqual:@YES]);
        assert([f.currentDefault isEqual:@"later-user-choice"]);
        assert(!fs::exists(f.support / "maintenance-session.plist"));
    }
    {
        MaintenanceFixture f(true);
        fs::remove(f.job);
        f.loaded = false;
        f.adapters.migrateApplicationCapture = true;
        auto result = f.run();
        assert([result[@"ok"] isEqual:@YES] && readableEngineBundle(f.preferred));
        assert(!fs::exists(f.legacy) && !fs::exists(f.job));
    }
    {
        MaintenanceFixture f;
        f.loaded = false;
        auto result = f.run();
        assert([result[@"ok"] isEqual:@YES] && f.loaded && f.bootstraps > 0);
    }
    {
        MaintenanceFixture f;
        f.loaded = false;
        f.adapters.checkpoint = [&](const char* phase) {
            if (std::string(phase) == "verified") {
                assert(f.loaded);
                throw std::runtime_error("failure after starting an initially stopped job");
            }
        };
        rejects([&] {
            f.run();
        });
        assert(f.stops > 0 && f.loaded && !f.newJob());
        assert(readableEngineBundle(f.legacy));
    }
    // Gate-off still updates the Engine in place and repairs driver/broker;
    // the existing user job is byte-for-byte unchanged.
    for (bool incomplete : {false, true}) {
        MaintenanceFixture f(true);
        if (incomplete)
            fs::remove(f.legacy / "Contents/MacOS/LoopZ Engine");
        const auto job = audioTreeDigest(f.job);
        auto result = f.run();
        assert([result[@"ok"] isEqual:@YES] && [result[@"migrationPending"] isEqual:@YES]);
        assert(readableEngineBundle(f.legacy) && !fs::exists(f.preferred));
        assert(get(f.installedDriver / "binary") == "new-driver");
        assert(get(f.support / "loopz-routing-broker") == "new-component");
        assert(audioTreeDigest(f.job) == job && f.stops == 1 && f.capabilityQueries == 1);
    }
    // Both migration and ordinary new-path Repair accept no prior grant.
    for (bool initialNewPath : {false, true}) {
        MaintenanceFixture f;
        if (initialNewPath) {
            fs::rename(f.legacy, f.preferred);
            writePlist(f.job, f.definition(f.preferred));
        }
        auto result = f.run();
        assert([result[@"ok"] isEqual:@YES] && readableEngineBundle(f.preferred));
        assert(f.permissionQueries == 1 && f.newJob());
    }
    // Broken signature, missing executable and crashing permission queries
    // all leave the old Engine repairable, without claiming a baseline grant.
    for (int damage = 0; damage < 3; ++damage) {
        MaintenanceFixture f;
        if (damage == 0)
            f.baselineFails = true;
        if (damage == 1)
            fs::remove(f.legacy / "Contents/MacOS/LoopZ Engine");
        if (damage == 2) {
            f.adapters.verify = [](const auto&, NSString*) {};
            f.baselineFails = true;
        }
        assert([f.run()[@"ok"] isEqual:@YES] && readableEngineBundle(f.preferred));
    }
    {
        MaintenanceFixture f;
        fs::rename(f.legacy, f.preferred);
        writePlist(f.job, f.definition(f.preferred));
        f.adapters.verify = [&](const auto& path, NSString*) {
            if (path == f.preferred)
                throw std::runtime_error("damaged installed Engine");
        };
        assert([f.run()[@"ok"] isEqual:@YES]);
        assert(get(f.preferred / "Contents/MacOS/LoopZ Engine") == "new-engine");
    }
    for (bool commandError : {false, true}) {
        MaintenanceFixture f;
        f.stopFails = !commandError;
        f.stopErrorsAfterRemoval = commandError;
        const auto legacy = audioTreeDigest(f.legacy), driver = audioTreeDigest(f.installedDriver),
                   job = audioTreeDigest(f.job);
        rejects([&] {
            f.run();
        });
        assert(f.stops == 1 && f.loaded);
        assert(audioTreeDigest(f.legacy) == legacy &&
               audioTreeDigest(f.installedDriver) == driver && audioTreeDigest(f.job) == job);
        assert(!fs::exists(f.support / "maintenance.plist") &&
               !fs::exists(f.support / "maintenance-session.plist"));
        if (commandError)
            assert(f.bootstraps == 1);
    }
    {
        MaintenanceFixture f(true);
        f.adapters.migrateApplicationCapture = true;
        f.adapters.checkpoint = [](const char* phase) {
            if (std::string(phase) == "promoted")
                throw AudioMaintenanceInterruption();
        };
        rejects([&] {
            f.run();
        });
        f.adapters.migrateApplicationCapture = false;
        f.adapters.checkpoint = {};
        auto result = f.run();
        assert([result[@"ok"] isEqual:@YES] && [result[@"migrationPending"] isEqual:@YES]);
        assert(readableEngineBundle(f.legacy) && !fs::exists(f.preferred) && !f.newJob());
        assert(get(f.installedDriver / "binary") == "new-driver");
    }
    // A stop failure on resumption must retain another run's recovery context
    // and journal. The retry subsequently recovers and completes normally.
    {
        MaintenanceFixture f;
        f.adapters.checkpoint = [](const char* phase) {
            if (std::string(phase) == "prepared")
                throw AudioMaintenanceInterruption();
        };
        rejects([&] {
            f.run();
        });
        const auto context = audioTreeDigest(f.support / "maintenance-session.plist");
        const auto journal = audioTreeDigest(f.support / "maintenance.plist");
        f.adapters.checkpoint = {};
        f.stopFails = true;
        rejects([&] {
            f.run();
        });
        assert(audioTreeDigest(f.support / "maintenance-session.plist") == context);
        assert(audioTreeDigest(f.support / "maintenance.plist") == journal);
        f.stopFails = false;
        assert([f.run()[@"ok"] isEqual:@YES]);
    }
    {
        MaintenanceFixture f;
        f.adapters.checkpoint = [](const char* phase) {
            if (std::string(phase) == "verified")
                throw std::runtime_error("verification boundary failed");
            if (std::string(phase) == "rolled-back")
                throw AudioMaintenanceInterruption();
        };
        rejects([&] {
            f.run();
        });
        assert([readPlist(f.support / "maintenance-session.plist")[@"verified"] isEqual:@NO]);
        assert(!f.newJob() && readableEngineBundle(f.legacy));
        f.adapters.checkpoint = {};
        assert([f.run()[@"ok"] isEqual:@YES] && f.newJob() && f.loaded);
    }
    {
        MaintenanceFixture f;
        f.registered = true;
        auto pending = f.run();
        assert([pending[@"activationRequired"] isEqual:@YES]);
        assert(fs::exists(f.support / "activation-required.plist") &&
               readableEngineBundle(f.legacy));
        assert(!f.newJob() && !fs::exists(f.preferred));
        assert([f.run(true)[@"ok"] isEqual:@YES] && f.activations == 1);
        assert(!fs::exists(f.support / "activation-required.plist"));
    }
    {
        MaintenanceFixture f(true);
        f.registered = true;
        assert([f.run()[@"activationRequired"] isEqual:@YES]);
        assert(get(f.installedDriver / "binary") == "new-driver");
        assert(!f.newJob() && readableEngineBundle(f.legacy));
        // The driver bytes now match: the marker must still trigger explicit
        // activation rather than depending on changedDriver becoming true again.
        assert([f.run(true)[@"ok"] isEqual:@YES] && f.activations == 1);
        assert(!fs::exists(f.support / "activation-required.plist"));
    }
    for (bool lostGrant : {false, true}) {
        MaintenanceFixture f;
        f.baseline = @"authorized";
        f.consentLost = lostGrant;
        if (lostGrant) {
            bool consentError = false;
            try {
                f.run();
            } catch (const EngineConsentLost&) {
                consentError = true;
            }
            assert(consentError && !f.newJob() && readableEngineBundle(f.legacy));
        } else
            assert([f.run()[@"ok"] isEqual:@YES]);
    }
    for (bool running : {false, true}) {
        MaintenanceFixture f(true);
        f.adapters.migrateApplicationCapture = true;
        f.loaded = running;
        f.captureFails = true;
        rejects([&] {
            f.run();
        });
        assert(readableEngineBundle(f.legacy) && !fs::exists(f.preferred));
    }
    {
        MaintenanceFixture f(true);
        f.adapters.migrateApplicationCapture = true;
        assert([f.run()[@"ok"] isEqual:@YES] && !fs::exists(f.legacy));
    }
    // Retirement conflicts retain the recreated bytes but cannot block restoring
    // the original Engine, job and driver from the real helper catch path.
    {
        MaintenanceFixture f;
        const auto originalDriver = audioTreeDigest(f.installedDriver);
        f.adapters.checkpoint = [&](const char* phase) {
            if (std::string(phase) == "retired") {
                fs::create_directory_symlink(f.root / "source", f.legacy);
                throw std::runtime_error("retirement interrupted");
            }
        };
        rejects([&] {
            f.run();
        });
        assert(readableEngineBundle(f.legacy) && !f.newJob());
        assert(audioTreeDigest(f.installedDriver) == originalDriver);
    }
    {
        MaintenanceFixture f;
        const auto otherJob =
            f.root / "Users/other/Library/Application Support/LoopZ/Session/engine.plist";
        fs::create_directories(otherJob.parent_path());
        writePlist(otherJob, f.definition(f.legacy));
        assert([f.run()[@"ok"] isEqual:@YES] && f.newJob() && f.loaded);
        assert(readableEngineBundle(f.legacy));
    }
    for (bool capability : {false, true}) {
        MaintenanceFixture f;
        f.signatureFails = !capability;
        f.capabilityFails = capability;
        rejects([&] {
            f.run();
        });
        assert(f.stops == 0 && readableEngineBundle(f.legacy) && !fs::exists(f.preferred));
    }
    {
        MaintenanceFixture f;
        fs::rename(f.routes, f.root / "original.json");
        fs::create_symlink(f.root / "original.json", f.routes);
        rejects([&] {
            f.run();
        });
        assert(f.stops == 0);
    }
    std::printf("PASS: %zu calls through maintainAudioComponents with private filesystem state\n",
                maintenanceInvocations);
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
        maintenanceEntryPointRegressions();
        jobPublicationPinsTheUserDirectory();
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
