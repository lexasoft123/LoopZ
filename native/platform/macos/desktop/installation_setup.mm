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
        result.components[0] = checkedCode(
            paths.engine, true, "com.lexasoft.loopz.engine", false,
            "Install LoopZ Engine alongside the LoopZ app to start routing.",
            "LoopZ Engine could not be verified. Reinstall the matching LoopZ apps.", code);
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
            result.components[1] = {InstallationState::Invalid,
                                    "More than one LoopZ audio driver is present. Repair the audio "
                                    "components before installing another copy."};
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
InstallationProbe productionInstallationProbe() {
    auto probe = makeInstallationProbe({"/Applications/LoopZ Engine.app",
                                        "/Library/Audio/Plug-Ins/HAL/LoopZAudio.driver",
                                        "/Library/Audio/Plug-Ins/HAL/LoopZRoutingAudio.driver",
                                        "/Library/Application Support/LoopZ/loopz-routing-broker",
                                        "/Library/LaunchDaemons/com.lexasoft.loopz.routing.plist"},
                                       signedCode, daemonDefinition);
    return [probe = std::move(probe)] {
        auto result = probe();
        result.activationRequired =
            fs::exists("/Library/Application Support/LoopZ/restart-required") ||
            fs::exists("/Library/Application Support/LoopZ/activation-required.plist");
        const fs::path app = NSBundle.mainBundle.bundlePath.UTF8String ?: "";
        const auto payload = app / "Contents/Resources/AudioComponents";
        result.maintenanceAvailable =
            app == "/Applications/LoopZ.app" &&
            fs::is_regular_file(app / "Contents/Library/Helpers/LoopZSetup") &&
            fs::is_directory(payload);
        if (result.maintenanceAvailable) {
            for (const auto& pair : std::array<std::pair<fs::path, fs::path>, 3>{
                     {{payload / "LoopZ Engine.app", "/Applications/LoopZ Engine.app"},
                      {payload / "LoopZAudio.driver",
                       "/Library/Audio/Plug-Ins/HAL/LoopZAudio.driver"},
                      {payload / "loopz-routing-broker",
                       "/Library/Application Support/LoopZ/loopz-routing-broker"}}}) {
                NSData* bundled = codeDigest(pair.first);
                NSData* installed = codeDigest(pair.second);
                if (bundled && installed && ![bundled isEqual:installed])
                    result.updateAvailable = true;
            }
        }
        if (fs::exists("/Library/Application Support/LoopZ/maintenance.plist") ||
            fs::exists("/Library/Application Support/LoopZ/maintenance-session.plist"))
            result.components[2] = {
                InstallationState::Invalid,
                "An interrupted audio update needs recovery. Choose Repair audio in Settings."};
        return result;
    };
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
        @"activationRequired" : @(snapshot.activationRequired)
    };
}
} // namespace loopz::macos
