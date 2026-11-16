#include "../../native/platform/macos/desktop/installation_setup.hpp"
#include "../../native/platform/macos/desktop/engine_job.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using namespace loopz::macos;
namespace fs = std::filesystem;
struct Fixture {
    const fs::path root{std::string(NSTemporaryDirectory().UTF8String) + "loopz-setup-" +
                        NSUUID.UUID.UUIDString.UTF8String};
    InstallationPaths paths{root / "Engine.app", root / "Legacy.driver", root / "Routing.driver",
                            root / "broker", root / "daemon.plist"};
    std::vector<std::pair<std::string, bool>> checks;
    bool codeValid{true}, daemonValid{true}, daemonThrows{false};
    Fixture() {
        fs::create_directory(root);
    }
    ~Fixture() {
        fs::remove_all(root);
    }
    InstallationProbe probe() {
        return makeInstallationProbe(
            paths,
            [this](const auto&, const auto& id, bool protectedFiles) {
                checks.emplace_back(id, protectedFiles);
                return codeValid;
            },
            [this](const auto&) {
                if (daemonThrows)
                    throw std::runtime_error("fixture");
                return daemonValid;
            });
    }
    void complete() {
        fs::create_directories(paths.engine / "Contents/MacOS");
        std::ofstream(paths.engine / "Contents/MacOS/LoopZ Engine") << "fixture";
        fs::permissions(paths.engine / "Contents/MacOS/LoopZ Engine", fs::perms::owner_exec,
                        fs::perm_options::add);
        fs::create_directory(paths.legacyDriver);
        std::ofstream(paths.broker) << "fixture";
        std::ofstream(paths.daemon) << "fixture";
    }
};
void missingAndUninspected() {
    assert([installationJSON({})[@"checked"] isEqual:@NO]);
    Fixture fixture;
    const auto result = fixture.probe()();
    for (const auto& component : result.components) {
        assert(component.state == InstallationState::Missing && !component.detail.empty());
    }
    assert(fixture.checks.empty());
    assert([installationJSON(result)[@"checked"] isEqual:@YES]);
}
void validSlotsAndIdentityRoles() {
    Fixture fixture;
    fixture.complete();
    auto result = fixture.probe()();
    for (const auto& component : result.components)
        assert(component.state == InstallationState::Installed);
    assert((fixture.checks == std::vector<std::pair<std::string, bool>>{
                                  {"com.lexasoft.loopz.engine", false},
                                  {"com.lexasoft.loopz.driver", true},
                                  {"com.lexasoft.loopz.routing.broker", true}}));
    fs::rename(fixture.paths.legacyDriver, fixture.paths.routingDriver);
    fixture.checks.clear();
    result = fixture.probe()();
    assert(result.components[1].state == InstallationState::Installed);
    fs::create_directory(fixture.paths.legacyDriver);
    fixture.checks.clear();
    result = fixture.probe()();
    assert(result.components[1].state == InstallationState::Invalid);
    assert(result.components[1].detail.find("More than one") != std::string::npos);
    assert(fixture.checks.size() == 2);
}
void invalidOrMissingCannotAdvertiseInstalled() {
    Fixture fixture;
    fixture.complete();
    fixture.codeValid = false;
    auto result = fixture.probe()();
    for (const auto& component : result.components)
        assert(component.state == InstallationState::Invalid);
    fixture.codeValid = true;
    fs::remove(fixture.paths.daemon);
    result = fixture.probe()();
    assert(result.components[2].state == InstallationState::Missing);
    std::ofstream(fixture.paths.daemon) << "fixture";
    fixture.daemonValid = false;
    result = fixture.probe()();
    assert(result.components[2].state == InstallationState::Invalid);
    fixture.daemonThrows = true;
    result = fixture.probe()();
    assert(result.components[0].state == InstallationState::Installed);
    assert(result.components[1].state == InstallationState::Installed);
    assert(result.components[2].state == InstallationState::Unknown);
}
void wrongTypesAndLinksAreRejected() {
    Fixture fixture;
    std::ofstream(fixture.paths.engine) << "not a bundle";
    fs::create_symlink(fixture.root / "absent-driver", fixture.paths.legacyDriver);
    fs::create_directory(fixture.paths.broker);
    auto result = fixture.probe()();
    for (const auto& component : result.components)
        assert(component.state == InstallationState::Invalid);
    assert(fixture.checks.empty());
    auto paths = fixture.paths;
    paths.engine = "relative";
    bool rejected = false;
    try {
        (void)makeInstallationProbe(
            paths,
            [](const auto&, const auto&, bool) {
                return true;
            },
            [](const auto&) {
                return true;
            });
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}
void legacyAudioDoesNotCountAsRoutingInstallationOrConflict() {
    Fixture fixture;
    fixture.complete();
    fs::create_directory(fixture.paths.legacyDriver / "Contents");
    auto* data = [NSPropertyListSerialization dataWithPropertyList:@{
        @"AudioServerPlugIn_MachServices" : @[ @"com.lexasoft.loopz.bridge" ]
    }
                                                            format:NSPropertyListXMLFormat_v1_0
                                                           options:0
                                                             error:nil];
    assert([data writeToFile:@((fixture.paths.legacyDriver / "Contents/Info.plist").c_str())
                  atomically:YES]);
    auto result = fixture.probe()();
    assert(result.components[1].state == InstallationState::Missing);
    fs::create_directory(fixture.paths.routingDriver);
    result = fixture.probe()();
    assert(result.components[1].state == InstallationState::Installed);
    assert(fs::is_directory(fixture.paths.legacyDriver));
}
void knownOldJobIsNotForeign() {
    const auto currentProgram = newEnginePath / "Contents/MacOS/LoopZ Engine";
    const auto legacyProgram = legacyEnginePath / "Contents/MacOS/LoopZ Engine";
    NSDictionary* current = @{
        @"Label" : @"fixture",
        @"KeepAlive" : @YES,
        @"ProgramArguments" : @[ @(currentProgram.c_str()), @"--editor-service", @"true" ]
    };
    NSMutableDictionary* old = [current mutableCopy];
    old[@"ProgramArguments"] = @[ @(legacyProgram.c_str()), @"--editor-service", @"true" ];
    assert(classifyEngineJob(current, current) == EngineJobDefinition::Current);
    assert(classifyEngineJob(old, current) == EngineJobDefinition::KnownOld);
    old[@"KeepAlive"] = @NO;
    assert(classifyEngineJob(old, current) == EngineJobDefinition::Foreign);
    old[@"KeepAlive"] = @YES;
    old[@"ProgramArguments"] = @[ @"/tmp/Engine", @"--editor-service", @"true" ];
    assert(classifyEngineJob(old, current) == EngineJobDefinition::Foreign);
}
void productionRecoveryStates() {
    Fixture f;
    f.complete();
    const auto app = f.root / "LoopZ.app", support = f.root / "support";
    const auto helper = app / "Contents/Library/Helpers/LoopZSetup";
    const auto payload = app / "Contents/Resources/AudioComponents";
    ProductionInstallationPaths paths{f.paths, app, app, support, f.root / "new/Engine.app"};
    bool different = false;
    auto probe = [&] {
        return makeProductionInstallationProbe(
            paths,
            [](const auto&, const auto&, bool) {
                return true;
            },
            [](const auto&) {
                return true;
            },
            [&](const auto& path) {
                return different && path == payload / "LoopZ Engine.app" ? "new" : "same";
            })();
    };
    paths.app = f.root / "elsewhere.app";
    assert(probe().maintenanceUnavailableReason == "wrong-location");
    paths.app = app;
    assert(probe().maintenanceUnavailableReason == "missing-helper");
    fs::create_directories(helper.parent_path());
    std::ofstream(helper) << "fixture";
    assert(probe().maintenanceUnavailableReason == "missing-payload");
    assert([installationJSON(probe())[@"maintenanceUnavailableReason"] isEqual:@"missing-payload"]);
    fs::create_directories(payload);
    auto result = probe();
    assert(result.maintenanceAvailable && !result.updateAvailable && !result.activationRequired);
    different = true;
    assert(probe().updateAvailable);
    different = false;
    fs::create_directory(support);
    for (const char* marker : {"restart-required", "activation-required.plist"}) {
        std::ofstream(support / marker) << "fixture";
        assert(probe().activationRequired);
        fs::remove(support / marker);
    }
    for (const char* marker : {"maintenance.plist", "maintenance-session.plist"}) {
        std::ofstream(support / marker) << "fixture";
        assert(probe().components[2].state == InstallationState::Invalid);
        assert(probe().components[2].detail.find("interrupted") != std::string::npos);
        fs::remove(support / marker);
    }
    assert(readEnginePath(paths.preferredEngine, paths.components.engine) == f.paths.engine);
    fs::create_directories(paths.preferredEngine.parent_path());
    for (int kind = 0; kind < 3; ++kind) {
        if (kind == 0)
            fs::create_directory(paths.preferredEngine);
        if (kind == 1)
            std::ofstream(paths.preferredEngine) << "stray";
        if (kind == 2)
            fs::create_directory_symlink(f.paths.engine, paths.preferredEngine);
        assert(readEnginePath(paths.preferredEngine, paths.components.engine) == f.paths.engine);
        assert(probe().components[0].state == InstallationState::Installed);
        fs::remove_all(paths.preferredEngine);
    }
    fs::create_directories(paths.preferredEngine / "Contents/MacOS");
    std::ofstream(paths.preferredEngine / "Contents/MacOS/LoopZ Engine") << "fixture";
    fs::permissions(paths.preferredEngine / "Contents/MacOS/LoopZ Engine", fs::perms::owner_exec,
                    fs::perm_options::add);
    assert(readEnginePath(paths.preferredEngine, paths.components.engine) == paths.preferredEngine);
    fs::remove_all(f.paths.engine);
    assert(probe().components[0].state == InstallationState::Installed);
    fs::remove_all(paths.preferredEngine);
    fs::create_directory(paths.preferredEngine);
    assert(probe().components[0].state == InstallationState::Invalid);
    fs::remove_all(paths.preferredEngine);
    result = probe();
    assert(result.components[0].state == InstallationState::Missing);
    assert(result.components[0].detail.find("Repair audio") != std::string::npos);
    fs::remove_all(payload);
    assert(probe().components[0].detail.find("incomplete") != std::string::npos);
    fs::create_directory(f.paths.routingDriver);
    result = probe();
    assert(!result.maintenanceAvailable &&
           result.maintenanceUnavailableReason == "separate-driver");
    assert(result.components[1].state == InstallationState::Invalid);
    assert(result.components[1].detail.find("More than one") != std::string::npos);
    fs::remove_all(f.paths.legacyDriver);
    fs::create_directories(f.paths.engine / "Contents/MacOS");
    std::ofstream(f.paths.engine / "Contents/MacOS/LoopZ Engine") << "fixture";
    fs::create_directory(payload);
    different = true;
    result = probe();
    for (const auto& component : result.components)
        assert(component.state == InstallationState::Installed);
    assert(!result.updateAvailable && !result.maintenanceAvailable);
    assert(result.maintenanceUnavailableReason == "separate-driver");
}
} // namespace
int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc == 2 && std::string_view(argv[1]) == "--installed") {
            const auto value = productionInstallationProbe()();
            auto* data = [NSJSONSerialization dataWithJSONObject:installationJSON(value)
                                                         options:0
                                                           error:nil];
            assert(data);
            std::fwrite(data.bytes, 1, data.length, stdout);
            std::putchar('\n');
            return 0;
        }
        knownOldJobIsNotForeign();
        productionRecoveryStates();
        missingAndUninspected();
        validSlotsAndIdentityRoles();
        invalidOrMissingCannotAdvertiseInstalled();
        wrongTypesAndLinksAreRejected();
        legacyAudioDoesNotCountAsRoutingInstallationOrConflict();
        std::puts("Installation inspection: missing, signed roles, duplicate slots, invalid "
                  "configuration, unknown errors and links passed");
    }
}
