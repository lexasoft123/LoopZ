#include "engine_location.hpp"
#include "installation_setup.hpp"
#import <Security/Security.h>
#include <sys/stat.h>
#include <algorithm>
#include <stdexcept>

namespace loopz::macos {
namespace {
namespace fs = std::filesystem;
NSString* text(const std::string& value) {
    return [[NSString alloc] initWithBytes:value.data()
                                    length:value.size()
                                  encoding:NSUTF8StringEncoding]
               ?: @"";
}
std::string requirement(const std::string& identifier) {
    return "anchor apple generic and identifier \"" + identifier +
           "\" and certificate leaf[subject.OU] = \"USJ7H3X44X\" and "
           "certificate leaf[field.1.2.840.113635.100.6.1.13] exists";
}
bool protectedEntry(const fs::path& path) {
    struct stat info{};
    return !lstat(path.c_str(), &info) && info.st_uid == 0 && !(info.st_mode & 0022) &&
           (S_ISDIR(info.st_mode) || S_ISREG(info.st_mode));
}
bool protectedTree(const fs::path& path) {
    for (auto ancestor = path; !ancestor.empty(); ancestor = ancestor.parent_path()) {
        if (!protectedEntry(ancestor))
            return false;
        if (ancestor == ancestor.root_path())
            break;
    }
    if (fs::is_directory(path)) {
        size_t count = 0;
        for (const auto& item : fs::recursive_directory_iterator(path))
            if (++count > 1024 || !protectedEntry(item.path()))
                return false;
    }
    return true;
}
bool signedCode(const fs::path& path, const std::string& identifier, bool protectedFiles) {
    if (protectedFiles && !protectedTree(path))
        return false;
    SecStaticCodeRef code = nullptr;
    SecRequirementRef pin = nullptr;
    auto status = SecRequirementCreateWithString(
        (__bridge CFStringRef)text(requirement(identifier)), kSecCSDefaultFlags, &pin);
    if (!status)
        status = SecStaticCodeCreateWithPath(
            (__bridge CFURLRef)[NSURL fileURLWithPath:text(path.string())], kSecCSDefaultFlags,
            &code);
    if (!status)
        status = SecStaticCodeCheckValidity(
            code, kSecCSStrictValidate | kSecCSCheckAllArchitectures, pin);
    if (pin)
        CFRelease(pin);
    if (code)
        CFRelease(code);
    return status == errSecSuccess;
}
NSData* codeDigest(const fs::path& path) {
    SecStaticCodeRef code = nullptr;
    CFDictionaryRef information = nullptr;
    const auto status = SecStaticCodeCreateWithPath(
        (__bridge CFURLRef)[NSURL fileURLWithPath:text(path.string())], kSecCSDefaultFlags, &code);
    if (!status)
        SecCodeCopySigningInformation(code, kSecCSSigningInformation, &information);
    NSDictionary* values = CFBridgingRelease(information);
    if (code)
        CFRelease(code);
    id hash = values[(__bridge NSString*)kSecCodeInfoUnique];
    return [hash isKindOfClass:NSData.class] ? hash : nil;
}
bool daemonDefinition(const fs::path& path) {
    if (!protectedTree(path) || fs::file_size(path) > 65536)
        return false;
    NSData* data = [NSData dataWithContentsOfURL:[NSURL fileURLWithPath:text(path.string())]];
    if (!data)
        return false;
    id value = [NSPropertyListSerialization propertyListWithData:data
                                                         options:NSPropertyListImmutable
                                                          format:nil
                                                           error:nil];
    NSDictionary* expected = @{
        @"AssociatedBundleIdentifiers" : @[ @"com.lexasoft.loopz.editor" ],
        @"Label" : @"com.lexasoft.loopz.routing",
        @"ProgramArguments" : @[
            @"/Library/Application Support/LoopZ/loopz-routing-broker", @"--engine-requirement",
            text(requirement("com.lexasoft.loopz.engine"))
        ],
        @"MachServices" : @{@"com.lexasoft.loopz.routing.bridge" : @YES},
        @"KeepAlive" : @YES,
        @"RunAtLoad" : @YES,
        @"ProcessType" : @"Background",
        @"ThrottleInterval" : @3
    };
    NSMutableDictionary* legacy = [expected mutableCopy];
    [legacy removeObjectForKey:@"AssociatedBundleIdentifiers"];
    return [value isKindOfClass:NSDictionary.class] &&
           ([value isEqual:expected] || [value isEqual:legacy]);
}
InstallationComponent entry(const fs::path& path, bool directory, const char* missing,
                            const char* invalid) {
    std::error_code error;
    const auto info = fs::symlink_status(path, error);
    if (info.type() == fs::file_type::not_found)
        return {InstallationState::Missing, missing};
    if (error)
        return {InstallationState::Unknown,
                "LoopZ could not check its audio components. Retry the setup check."};
    if (fs::is_symlink(info) || (directory ? !fs::is_directory(info) : !fs::is_regular_file(info)))
        return {InstallationState::Invalid, invalid};
    return {InstallationState::Installed, {}};
}
InstallationComponent checkedCode(const fs::path& path, bool directory,
                                  const std::string& identifier, bool protectedFiles,
                                  const char* missing, const char* invalid,
                                  const InstallationCodeCheck& verify) {
    try {
        auto result = entry(path, directory, missing, invalid);
        if (result.state == InstallationState::Installed &&
            !verify(path, identifier, protectedFiles))
            return {InstallationState::Invalid, invalid};
        return result;
    } catch (...) {
        return {InstallationState::Unknown,
                "LoopZ could not verify its audio components. Retry the setup check."};
    }
}
bool legacyOnlyDriver(const fs::path& bundle) {
    const auto path = bundle / "Contents/Info.plist";
    if (entry(path, false, "", "").state != InstallationState::Installed ||
        fs::file_size(path) > 65536)
        return false;
    NSData* data = [NSData dataWithContentsOfURL:[NSURL fileURLWithPath:text(path.string())]];
    id info = data ? [NSPropertyListSerialization propertyListWithData:data
                                                               options:NSPropertyListImmutable
                                                                format:nil
                                                                 error:nil]
                   : nil;
    return [info isKindOfClass:NSDictionary.class] &&
           [info[@"AudioServerPlugIn_MachServices"] isEqual:@[ @"com.lexasoft.loopz.bridge" ]];
}
NSString* stateName(InstallationState state) {
    switch (state) {
    case InstallationState::Unchecked:
        return @"unchecked";
    case InstallationState::Installed:
        return @"installed";
    case InstallationState::Missing:
        return @"missing";
    case InstallationState::Invalid:
        return @"invalid";
    case InstallationState::Unknown:
        return @"unknown";
    }
    return @"unknown";
}
} // namespace
InstallationProbe makeInstallationProbe(InstallationPaths paths, InstallationCodeCheck code,
                                        InstallationDaemonCheck daemon) {
    if (!code || !daemon)
        throw std::invalid_argument("Installation verification operations are required");
    for (const auto& path :
         {paths.engine, paths.legacyDriver, paths.routingDriver, paths.broker, paths.daemon})
        if (!path.is_absolute())
            throw std::invalid_argument("Installation checks require absolute paths");
    return [paths = std::move(paths), code = std::move(code), daemon = std::move(daemon)] {
        InstallationSnapshot result;
        result.components[0] = checkedCode(paths.engine, true, "com.lexasoft.loopz.engine", false,
                                           "LoopZ Engine is missing or damaged. Use Install audio "
                                           "components or Repair audio in Settings.",
                                           "LoopZ Engine is missing or damaged. Use Install audio "
                                           "components or Repair audio in Settings.",
                                           code);
        auto legacy = entry(paths.legacyDriver, true, "", "");
        const auto routing = entry(paths.routingDriver, true, "", "");
        // Initial routing installation can preserve an independently active
        // legacy-only driver. It does not compete for the routing service.
        if (legacy.state == InstallationState::Installed) {
            try {
                if (legacyOnlyDriver(paths.legacyDriver))
                    legacy = {InstallationState::Missing, {}};
            } catch (...) {
                legacy = {InstallationState::Unknown, {}};
            }
        }
        if (legacy.state == InstallationState::Unknown ||
            routing.state == InstallationState::Unknown) {
            result.components[1] = {
                InstallationState::Unknown,
                "LoopZ could not check its audio driver. Retry the setup check."};
        } else if (legacy.state != InstallationState::Missing &&
                   routing.state != InstallationState::Missing) {
            result.components[1] = {
                InstallationState::Invalid,
                "More than one LoopZ audio driver is present. Contact support for help "
                "removing the older installation and preserving your audio setup."};
        } else {
            const bool useLegacy = legacy.state != InstallationState::Missing;
            result.components[1] = checkedCode(
                useLegacy ? paths.legacyDriver : paths.routingDriver, true,
                useLegacy ? "com.lexasoft.loopz.driver" : "com.lexasoft.loopz.driver.routing", true,
                "Install LoopZ’s audio components to make virtual devices available to other apps.",
                "The LoopZ audio driver could not be verified. Repair the audio components.", code);
        }
        result.components[2] = checkedCode(
            paths.broker, false, "com.lexasoft.loopz.routing.broker", true,
            "Install LoopZ’s audio service to connect the engine and virtual devices.",
            "The LoopZ audio service could not be verified. Repair the audio components.", code);
        if (result.components[2].state == InstallationState::Installed) {
            try {
                auto job =
                    entry(paths.daemon, false,
                          "LoopZ’s audio service setup is incomplete. Repair the audio components.",
                          "The LoopZ audio service configuration needs repair.");
                if (job.state == InstallationState::Installed && !daemon(paths.daemon))
                    job = {InstallationState::Invalid,
                           "The LoopZ audio service configuration needs repair."};
                if (job.state != InstallationState::Installed)
                    result.components[2] = std::move(job);
            } catch (...) {
                result.components[2] = {InstallationState::Unknown,
                                        "LoopZ could not check its audio service configuration. "
                                        "Retry the setup check."};
            }
        }
        return result;
    };
}
InstallationProbe makeProductionInstallationProbe(ProductionInstallationPaths paths,
                                                  InstallationCodeCheck code,
                                                  InstallationDaemonCheck daemon,
                                                  InstallationDigest digest) {
    return [paths, code, daemon, digest] {
        auto components = paths.components;
        components.engine = readEnginePath(paths.preferredEngine, components.engine);
        auto result = makeInstallationProbe(components, code, daemon)();
        if (!readableEngineBundle(paths.preferredEngine) &&
            fs::symlink_status(paths.preferredEngine).type() != fs::file_type::not_found &&
            result.components[0].state != InstallationState::Installed)
            result.components[0].state = InstallationState::Invalid;
        result.activationRequired = fs::exists(paths.support / "restart-required") ||
                                    fs::exists(paths.support / "activation-required.plist");
        const auto payload = paths.app / "Contents/Resources/AudioComponents";
        if (paths.app != paths.expectedApp)
            result.maintenanceUnavailableReason = "wrong-location";
        else if (!fs::is_regular_file(paths.app / "Contents/Library/Helpers/LoopZSetup"))
            result.maintenanceUnavailableReason = "missing-helper";
        else if (!fs::is_directory(payload))
            result.maintenanceUnavailableReason = "missing-payload";
        // Keep reason codes/messages in sync with AudioSetup.jsx unavailableMessages.
        // A healthy separate routing slot remains operational; only maintenance is unavailable.
        if (fs::symlink_status(components.routingDriver).type() != fs::file_type::not_found)
            result.maintenanceUnavailableReason = "separate-driver";
        result.maintenanceAvailable = result.maintenanceUnavailableReason.empty();
        if (result.components[0].state == InstallationState::Missing ||
            result.components[0].state == InstallationState::Invalid)
            result.components[0].detail =
                result.maintenanceAvailable ? "LoopZ Engine is missing or damaged. Use Install "
                                              "audio components or Repair audio in Settings."
                : result.maintenanceUnavailableReason == "wrong-location"
                    ? "Move LoopZ to Applications to maintain audio components."
                : result.maintenanceUnavailableReason == "separate-driver"
                    ? "This separate routing driver layout cannot be maintained in-app. Contact "
                      "support before changing audio components."
                    : "This copy of LoopZ is incomplete. Download the full LoopZ release and "
                      "replace this copy.";
        if (result.maintenanceAvailable) {
            for (const auto& pair : std::array<std::pair<fs::path, fs::path>, 3>{
                     {{payload / "LoopZ Engine.app", components.engine},
                      {payload / "LoopZAudio.driver", components.legacyDriver},
                      {payload / "loopz-routing-broker", components.broker}}}) {
                const auto bundled = digest(pair.first), installed = digest(pair.second);
                if (!bundled.empty() && !installed.empty() && bundled != installed)
                    result.updateAvailable = true;
            }
        }
        if (fs::exists(paths.support / "maintenance.plist") ||
            fs::exists(paths.support / "maintenance-session.plist"))
            result.components[2] = {
                InstallationState::Invalid,
                "An interrupted audio update needs recovery. Choose Repair audio in Settings."};
        return result;
    };
}
InstallationProbe productionInstallationProbe() {
    return makeProductionInstallationProbe(
        {{legacyEnginePath, "/Library/Audio/Plug-Ins/HAL/LoopZAudio.driver",
          "/Library/Audio/Plug-Ins/HAL/LoopZRoutingAudio.driver",
          "/Library/Application Support/LoopZ/loopz-routing-broker",
          "/Library/LaunchDaemons/com.lexasoft.loopz.routing.plist"},
         NSBundle.mainBundle.bundlePath.UTF8String ?: "",
         "/Applications/LoopZ.app",
         "/Library/Application Support/LoopZ",
         newEnginePath},
        signedCode, daemonDefinition, [](const fs::path& path) -> std::string {
            NSData* value = codeDigest(path);
            return value ? std::string(static_cast<const char*>(value.bytes), value.length) : "";
        });
}
NSDictionary* installationJSON(const InstallationSnapshot& snapshot) {
    NSMutableDictionary* components = [NSMutableDictionary dictionary];
    NSArray* names = @[ @"engine", @"driver", @"service" ];
    bool checked = true;
    for (size_t index = 0; index < snapshot.components.size(); ++index) {
        const auto& item = snapshot.components[index];
        checked = checked && item.state != InstallationState::Unchecked;
        components[names[index]] =
            @{@"state" : stateName(item.state), @"detail" : text(item.detail)};
    }
    return @{
        @"checked" : @(checked),
        @"components" : components,
        @"updateAvailable" : @(snapshot.updateAvailable),
        @"maintenanceAvailable" : @(snapshot.maintenanceAvailable),
        @"maintenanceUnavailableReason" : text(snapshot.maintenanceUnavailableReason),
        @"activationRequired" : @(snapshot.activationRequired)
    };
}
} // namespace loopz::macos
