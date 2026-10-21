#include "../../native/platform/macos/desktop/installation_setup.hpp"
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
        fs::create_directory(paths.engine);
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
    assert(fixture.checks[1].first == "com.lexasoft.loopz.driver.routing");
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
        missingAndUninspected();
        validSlotsAndIdentityRoles();
        invalidOrMissingCannotAdvertiseInstalled();
        wrongTypesAndLinksAreRejected();
        legacyAudioDoesNotCountAsRoutingInstallationOrConflict();
        std::puts("Installation inspection: missing, signed roles, duplicate slots, invalid "
                  "configuration, unknown errors and links passed");
    }
}
