// One-shot first-install helper. No Python, network, arbitrary commands, driver
// upgrades, default-device changes, or Core Audio restart. Never runs at login.
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <sys/stat.h>
#include <mach-o/dyld.h>
#include <vector>
#include <string_view>
#include <sys/wait.h>
#include <spawn.h>
#include <unistd.h>
#include <filesystem>
#include <stdexcept>
#include <cstdio>
extern char** environ;
namespace {
namespace fs = std::filesystem;
constexpr const char* team = "USJ7H3X44X";
void require(bool condition, const char* detail) {
    if (!condition)
        throw std::runtime_error(detail);
}
NSString* text(const fs::path& path) {
    return @(path.c_str());
}
NSString* requirement(NSString* identifier) {
    return [NSString
        stringWithFormat:
            @"anchor apple generic and identifier \"%@\" and certificate leaf[subject.OU] = \"%s\" "
            @"and certificate leaf[field.1.2.840.113635.100.6.1.13] exists",
            identifier, team];
}
void verify(const fs::path& path, NSString* identifier) {
    SecStaticCodeRef code = nullptr;
    SecRequirementRef pin = nullptr;
    OSStatus status = SecRequirementCreateWithString((__bridge CFStringRef)requirement(identifier),
                                                     kSecCSDefaultFlags, &pin);
    if (!status)
        status = SecStaticCodeCreateWithPath((__bridge CFURLRef)[NSURL fileURLWithPath:text(path)],
                                             kSecCSDefaultFlags, &code);
    if (!status)
        status = SecStaticCodeCheckValidity(
            code, kSecCSStrictValidate | kSecCSCheckAllArchitectures, pin);
    if (pin)
        CFRelease(pin);
    if (code)
        CFRelease(code);
    require(status == errSecSuccess, "An audio component could not be verified.");
}
void bounded(const fs::path& root) {
    require(!fs::is_symlink(root), "Linked setup payload is not supported.");
    size_t count = 0;
    uintmax_t bytes = 0;
    for (const auto& item : fs::recursive_directory_iterator(root)) {
        require(++count <= 4096 && !item.is_symlink(), "Invalid setup payload.");
        if (item.is_regular_file())
            bytes += item.file_size();
        else
            require(item.is_directory(), "Invalid setup entry.");
        require(bytes <= 128 * 1024 * 1024, "Setup payload is too large.");
    }
}
void protectedDirectory(const fs::path& path) {
    if (path == "/")
        return;
    protectedDirectory(path.parent_path());
    struct stat info{};
    require(!lstat(path.c_str(), &info) && S_ISDIR(info.st_mode) && info.st_uid == 0 &&
                !(info.st_mode & 0002) &&
                (!(info.st_mode & 0020) || (path == "/Applications" && info.st_gid == 80)),
            "Audio setup requires protected system directories.");
}
void copyEntry(const fs::path& source, const fs::path& target) {
    NSError* error = nil;
    require([NSFileManager.defaultManager copyItemAtPath:text(source)
                                                  toPath:text(target)
                                                   error:&error],
            "An audio component could not be copied.");
}
void own(const fs::path& path, uid_t uid, gid_t gid) {
    require(!lchown(path.c_str(), uid, gid), "Component ownership could not be set.");
    if (fs::is_directory(path))
        for (const auto& entry : fs::recursive_directory_iterator(path))
            require(!lchown(entry.path().c_str(), uid, gid),
                    "Component ownership could not be set.");
}
void launchBroker() {
    pid_t pid = 0;
    const char* args[] = {"/bin/launchctl", "bootstrap", "system",
                          "/Library/LaunchDaemons/com.lexasoft.loopz.routing.plist", nullptr};
    require(!posix_spawn(&pid, args[0], nullptr, nullptr, const_cast<char**>(args), environ),
            "The audio service could not be started.");
    int status = 0;
    require(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "The audio service could not be started. Setup files were retained for repair.");
}
} // namespace
int main(int argc, const char* argv[]) {
    @autoreleasepool {
        try {
            const bool verifyOnly = argc == 2 && std::string_view(argv[1]) == "--verify-payload";
            require(verifyOnly || (argc == 1 && geteuid() == 0),
                    "Audio setup needs administrator authorization.");
            // Resolve from the actual executable, never a caller-supplied source path.
            uint32_t length = 0;
            _NSGetExecutablePath(nullptr, &length);
            std::vector<char> executable(length);
            require(!_NSGetExecutablePath(executable.data(), &length),
                    "The setup executable could not be located.");
            fs::path helper = fs::canonical(executable.data());
            fs::path app = helper.parent_path().parent_path().parent_path().parent_path();
            require(app.extension() == ".app", "Run audio setup from the LoopZ app.");
            require(verifyOnly || app == "/Applications/LoopZ.app",
                    "Move LoopZ to Applications before installing audio components.");
            const auto source = app / "Contents/Resources/AudioComponents";
            bounded(app);
            verify(app, @"com.lexasoft.loopz.editor");
            verify(source / "LoopZAudio.driver", @"com.lexasoft.loopz.driver");
            verify(source / "LoopZ Engine.app", @"com.lexasoft.loopz.engine");
            verify(source / "loopz-routing-broker", @"com.lexasoft.loopz.routing.broker");
            verify(source / "loopz-config", @"com.lexasoft.loopz.config");
            verify(source / "loopz-routing-install-check",
                   @"com.lexasoft.loopz.routing.install-check");
            if (verifyOnly) {
                std::puts("Audio payload verified.");
                return 0;
            }
            const fs::path support = "/Library/Application Support/LoopZ";
            const fs::path driver = "/Library/Audio/Plug-Ins/HAL/LoopZAudio.driver";
            const fs::path daemon = "/Library/LaunchDaemons/com.lexasoft.loopz.routing.plist";
            const fs::path engine = "/Applications/LoopZ Engine.app";
            // First install only: never overwrite a live driver or another Engine.
            require(!fs::exists(driver) &&
                        !fs::exists("/Library/Audio/Plug-Ins/HAL/LoopZRoutingAudio.driver") &&
                        !fs::exists(daemon) && !fs::exists(engine) && !fs::exists(support),
                    "Audio components already exist. Use the existing installation or its repair "
                    "workflow.");
            for (const auto& path : {driver.parent_path(), daemon.parent_path(),
                                     engine.parent_path(), support.parent_path()})
                protectedDirectory(path);
            struct stat console{};
            require(!stat("/dev/console", &console) && console.st_uid >= 501,
                    "Sign in to your Mac before installing audio components.");
            require(fs::create_directory(support), "Another audio setup is already running.");
            chmod(support.c_str(), 0755);
            const fs::path staging =
                support / (".setup-" + std::string(NSUUID.UUID.UUIDString.UTF8String));
            fs::create_directory(staging);
            chmod(staging.c_str(), 0700);
            // Snapshot into root-owned storage and verify again before publishing.
            try {
                copyEntry(app, staging / "LoopZ.app");
                own(staging / "LoopZ.app", 0, 0);
                verify(staging / "LoopZ.app", @"com.lexasoft.loopz.editor");
            } catch (...) {
                std::error_code error;
                fs::remove_all(support, error);
                throw;
            }
            const auto payload = staging / "LoopZ.app/Contents/Resources/AudioComponents";
            try {
                verify(payload / "LoopZAudio.driver", @"com.lexasoft.loopz.driver");
                verify(payload / "LoopZ Engine.app", @"com.lexasoft.loopz.engine");
                verify(payload / "loopz-routing-broker", @"com.lexasoft.loopz.routing.broker");
                verify(payload / "loopz-config", @"com.lexasoft.loopz.config");
                verify(payload / "loopz-routing-install-check",
                       @"com.lexasoft.loopz.routing.install-check");
            } catch (...) {
                std::error_code error;
                fs::remove_all(support, error);
                throw;
            }
            std::vector<fs::path> installed;
            try {
                for (const auto& name : {"loopz-routing-broker", "loopz-config",
                                         "loopz-routing-install-check", "routing-service.json"}) {
                    copyEntry(payload / name, support / name);
                    installed.push_back(support / name);
                }
                copyEntry(payload / "com.lexasoft.loopz.routing.plist", daemon);
                installed.push_back(daemon);
                copyEntry(payload / "LoopZAudio.driver", driver);
                installed.push_back(driver);
                copyEntry(payload / "LoopZ Engine.app", engine);
                installed.push_back(engine);
                own(engine, console.st_uid, console.st_gid);
                verify(driver, @"com.lexasoft.loopz.driver");
                verify(engine, @"com.lexasoft.loopz.engine");
            } catch (...) {
                for (auto it = installed.rbegin(); it != installed.rend(); ++it) {
                    std::error_code error;
                    fs::remove_all(*it, error);
                }
                std::error_code error;
                fs::remove_all(support, error);
                throw;
            }
            fs::remove_all(staging);
            launchBroker();
            std::puts("Audio components installed. Return to LoopZ to connect audio.");
            return 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 1;
        }
    }
}
