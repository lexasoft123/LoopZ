#include "engine_location.hpp"
#include "engine_migration.hpp"
#include <sys/xattr.h>
#include <loopz/macos/audio_host_identity.hpp>
#include "audio_maintenance.hpp"
#include "audio_maintenance_testing.hpp"
#include "maintenance_files.hpp"
#include "maintenance_host_policy.hpp"
#include "session_host.hpp"
#include "catalog_channel.hpp"
#include <loopz/macos/route_store.hpp>
#import <Security/Security.h>
#include <CoreAudio/CoreAudio.h>
#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <pwd.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <future>
#include <chrono>
#include <cerrno>
#include <optional>
#include <set>
#include <thread>

namespace loopz::macos {
namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
thread_local fs::path support = "/Library/Application Support/LoopZ";
thread_local fs::path driver = "/Library/Audio/Plug-Ins/HAL/LoopZAudio.driver";
thread_local fs::path preferred = installedEngineTarget;
thread_local fs::path engine = installedEngineTarget;
thread_local fs::path daemon = "/Library/LaunchDaemons/com.lexasoft.loopz.routing.plist";
thread_local fs::path legacy = legacyEnginePath;
thread_local fs::path accounts = "/Users";
thread_local AudioMaintenanceFixture* fixture = nullptr;
uid_t installOwner() {
    return fixture ? geteuid() : 0;
}
gid_t installGroup() {
    return fixture ? getegid() : 0;
}
void checkpoint(const char* phase) {
    if (fixture && fixture->checkpoint)
        fixture->checkpoint(phase);
}
constexpr auto brokerLabel = "system/com.lexasoft.loopz.routing";
constexpr auto sessionName = engineJobLabel;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
NSString* text(const fs::path& path) {
    return @(path.c_str());
}
NSString* pin(NSString* identifier) {
    return [NSString stringWithFormat:@"anchor apple generic and identifier \"%@\" and "
                                      @"certificate leaf[subject.OU] = \"USJ7H3X44X\" and "
                                      @"certificate leaf[field.1.2.840.113635.100.6.1.13] exists",
                                      identifier];
}
void verify(const fs::path& path, NSString* identifier, bool apple = false) {
    if (fixture) {
        fixture->verify(path, identifier);
        return;
    }
    SecStaticCodeRef code = nullptr;
    SecRequirementRef requirement = nullptr;
    NSString* expected =
        apple ? [@"anchor apple and identifier \"" stringByAppendingFormat:@"%@\"", identifier]
              : pin(identifier);
    auto status = SecRequirementCreateWithString((__bridge CFStringRef)expected, kSecCSDefaultFlags,
                                                 &requirement);
    if (!status)
        status = SecStaticCodeCreateWithPath((__bridge CFURLRef)[NSURL fileURLWithPath:text(path)],
                                             kSecCSDefaultFlags, &code);
    if (!status)
        status = SecStaticCodeCheckValidity(
            code, kSecCSStrictValidate | kSecCSCheckAllArchitectures, requirement);
    if (code)
        CFRelease(code);
    if (requirement)
        CFRelease(requirement);
    require(status == errSecSuccess, "An audio component could not be verified.");
}
void verifyEngineStage(const fs::path& bundle) {
    verify(bundle, @(engineBundleIdentifier));
    std::vector<fs::path> paths{bundle};
    for (const auto& item : fs::recursive_directory_iterator(bundle))
        paths.push_back(item.path());
    for (const auto& path : paths) {
        errno = 0;
        const auto size =
            getxattr(path.c_str(), "com.apple.quarantine", nullptr, 0, 0, XATTR_NOFOLLOW);
        require(size < 0 && errno == ENOATTR,
                "The staged Engine has quarantine metadata or could not be inspected.");
    }
}
void protectedDirectory(const fs::path& path) {
    if (fixture)
        return;
    if (path == "/")
        return;
    protectedDirectory(path.parent_path());
    struct stat info{};
    require(!lstat(path.c_str(), &info) && S_ISDIR(info.st_mode) && info.st_uid == 0 &&
                !(info.st_mode & 0002) &&
                (!(info.st_mode & 0020) || (path == "/Applications" && info.st_gid == 80)),
            "Audio maintenance requires protected system directories.");
}

void validateInstalledEntry(const fs::path& root, bool userOwned, uid_t uid) {
    if (!fs::exists(fs::symlink_status(root)))
        return;
    std::vector<fs::path> paths{root};
    if (fs::is_directory(fs::symlink_status(root)))
        for (const auto& item : fs::recursive_directory_iterator(root)) {
            require(paths.size() < 4096, "Installed audio component has too many files.");
            paths.push_back(item.path());
        }
    for (const auto& path : paths) {
        struct stat info{};
        require(!lstat(path.c_str(), &info) && (S_ISREG(info.st_mode) || S_ISDIR(info.st_mode)) &&
                    (info.st_uid == installOwner() || (userOwned && info.st_uid == uid)) &&
                    !(info.st_mode & 0022) && (S_ISDIR(info.st_mode) || info.st_nlink == 1),
                "An installed audio component is not safely owned. No files were replaced.");
    }
}
NSDictionary* dictionary(const std::string& bytes) {
    NSData* data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
    id result = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    require([result isKindOfClass:NSDictionary.class],
            "The audio verification response is invalid.");
    return result;
}
NSData* boundedData(const fs::path& path, size_t maximum) {
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    require(fd >= 0, "The saved mix could not be opened safely.");
    struct Descriptor {
        int value;
        ~Descriptor() {
            close(value);
        }
    } descriptor{fd};
    struct stat info{};
    require(!fstat(fd, &info) && S_ISREG(info.st_mode) && info.st_nlink == 1 && info.st_size > 0 &&
                static_cast<uint64_t>(info.st_size) <= maximum,
            "The saved mix is invalid or too large.");
    NSMutableData* data = [NSMutableData dataWithLength:static_cast<NSUInteger>(info.st_size)];
    size_t done = 0;
    while (done < data.length) {
        const auto count =
            read(fd, static_cast<char*>(data.mutableBytes) + done, data.length - done);
        if (count < 0 && errno == EINTR)
            continue;
        require(count > 0, "The saved mix changed while being read.");
        done += static_cast<size_t>(count);
    }
    return data;
}
NSDictionary* plist(const fs::path& path, off_t maximumBytes = 65536) {
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    require(fd >= 0, "The saved audio session could not be opened.");
    struct stat info{};
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_size < 1 ||
        info.st_size > maximumBytes) {
        close(fd);
        throw std::runtime_error("Invalid saved audio session file.");
    }
    NSMutableData* data = [NSMutableData dataWithLength:static_cast<NSUInteger>(info.st_size)];
    size_t done = 0;
    while (done < data.length) {
        const auto count =
            read(fd, static_cast<char*>(data.mutableBytes) + done, data.length - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            close(fd);
            throw std::runtime_error("The saved audio session changed.");
        }
        done += static_cast<size_t>(count);
    }
    close(fd);
    id result = [NSPropertyListSerialization propertyListWithData:data
                                                          options:NSPropertyListImmutable
                                                           format:nil
                                                            error:nil];
    require([result isKindOfClass:NSDictionary.class], "Invalid saved audio session.");
    return result;
}
void saveSessionContext(NSDictionary* record) {
    // Runtime/catalog JSON includes optional NSNull values, which a property
    // list cannot represent. Preserve those snapshots as checked JSON blobs.
    NSMutableDictionary* stored = [record mutableCopy];
    for (NSString* key in @[ @"state", @"catalog", @"consent" ]) {
        if (!record[key])
            continue;
        NSData* bytes = [NSJSONSerialization dataWithJSONObject:record[key] options:0 error:nil];
        require(bytes && bytes.length, "Audio recovery snapshot could not be serialized.");
        stored[key] = bytes;
    }
    NSData* data = [NSPropertyListSerialization dataWithPropertyList:stored
                                                              format:NSPropertyListBinaryFormat_v1_0
                                                             options:0
                                                               error:nil];
    require(data && data.length && data.length <= 32 * 1024 * 1024,
            "Audio recovery context could not be serialized.");
    const auto temporary = support / (".session-" + std::string(NSUUID.UUID.UUIDString.UTF8String));
    const int fd =
        open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    require(fd >= 0, "Audio recovery context could not be created.");
    const auto count = write(fd, data.bytes, data.length);
    const bool saved = count == static_cast<ssize_t>(data.length) && !fsync(fd);
    close(fd);
    require(saved, "Audio recovery context could not be saved.");
    fs::rename(temporary, support / "maintenance-session.plist");
    syncAudioTree(support / "maintenance-session.plist");
}
SessionCommandResult command(std::vector<std::string> args, bool checked = true,
                             std::chrono::milliseconds timeout = 10s) {
    if (fixture) {
        auto result = fixture->command(args, false);
        if (checked && result.exitCode)
            throw std::runtime_error("Fixture command failed: " + args.front());
        return result;
    }
    SessionCommandResult result;
    try {
        result = sessionCommand(args, timeout, nullptr, SessionCommandOutput::Combined,
                                32 * 1024 * 1024);
    } catch (const std::runtime_error& error) {
        throw std::runtime_error("Audio maintenance: " +
                                 fs::path(args.front()).filename().string() + ": " + error.what());
    }
    if (checked && result.exitCode != 0)
        throw std::runtime_error(
            "Audio maintenance: " + fs::path(args.front()).filename().string() + " failed (" +
            std::to_string(result.exitCode) + "): " + result.output.substr(0, 512));
    return result;
}
struct Process {
    pid_t pid{};
    uid_t uid{};
    uint64_t seconds{}, microseconds{};
    std::string path;
    bool operator==(const Process&) const = default;
};
Process identity(pid_t pid) {
    proc_bsdinfo info{};
    char path[PROC_PIDPATHINFO_MAXSIZE]{};
    require(pid > 1 &&
                proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == sizeof(info) &&
                info.pbi_pid == static_cast<uint32_t>(pid) && info.pbi_start_tvsec &&
                proc_pidpath(pid, path, sizeof(path)) > 0,
            "An audio process identity could not be verified.");
    return {pid, info.pbi_uid, info.pbi_start_tvsec, info.pbi_start_tvusec, path};
}
std::vector<pid_t> pids() {
    const int bytes = proc_listallpids(nullptr, 0);
    require(bytes > 0 && bytes <= 1000000, "Audio process enumeration failed.");
    std::vector<pid_t> result(static_cast<size_t>(bytes) + 128);
    const int count =
        proc_listallpids(result.data(), static_cast<int>(result.size() * sizeof(pid_t)));
    require(count > 0 && static_cast<size_t>(count) < result.size(),
            "Audio process enumeration changed.");
    result.resize(static_cast<size_t>(count));
    return result;
}
Process coreAudio() {
    if (fixture)
        return {1, geteuid(), 1, 0, "fixture-core-audio"};
    std::optional<Process> found;
    for (const auto pid : pids()) {
        char path[PROC_PIDPATHINFO_MAXSIZE]{};
        if (proc_pidpath(pid, path, sizeof(path)) <= 0 ||
            std::string_view(path) != "/usr/sbin/coreaudiod")
            continue;
        const auto candidate = identity(pid);
        const auto* user = getpwnam("_coreaudiod");
        if (!user || candidate.uid != user->pw_uid)
            continue;
        require(!found, "Core Audio identity is ambiguous.");
        found = candidate;
    }
    require(found.has_value(), "Core Audio is not available. Try audio repair after signing in.");
    return *found;
}
struct DriverHost {
    Process process;
    AudioMappedImage image;
    bool operator==(const DriverHost&) const = default;
};
std::optional<DriverHost> isolatedHost() {
    if (fixture)
        return {};
    std::optional<DriverHost> found;
    for (const auto pid : pids()) {
        char path[PROC_PIDPATHINFO_MAXSIZE]{};
        if (proc_pidpath(pid, path, sizeof(path)) <= 0 || !isolatedAudioHostPath(path))
            continue;
        const auto before = identity(pid);
        const auto* user = getpwnam("_coreaudiod");
        if (!user || before.uid != user->pw_uid)
            continue;
        const auto listing =
            command({"/usr/sbin/lsof", "-nP", "-a", "-p", std::to_string(pid), "-F0pfDin"}, false);
        if (listing.exitCode)
            continue;
        const auto image = loopZMappedImage(listing.output, pid);
        if (!image)
            continue;
        require(!found, "More than one isolated LoopZ audio host is present.");
        verify(before.path, text(fs::path(before.path).filename()), true);
        require(identity(pid) == before, "The isolated audio driver changed during inspection.");
        found = DriverHost{before, *image};
    }
    return found;
}
void reload(const std::optional<DriverHost>& host) {
    if (!host)
        return;
    const auto verified = isolatedHost();
    require(verified && *verified == *host && identity(host->process.pid) == host->process,
            "The LoopZ driver process changed; audio update was stopped safely.");
    require(kill(host->process.pid, SIGKILL) == 0, "The LoopZ driver could not be reloaded.");
}
NSDictionary* normalizedIntent(NSDictionary* document) {
    NSMutableDictionary* result = [document mutableCopy];
    NSMutableArray* devices = [NSMutableArray array];
    for (NSDictionary* device in document[@"devices"]) {
        NSMutableDictionary* copy = [device mutableCopy];
        [copy removeObjectForKey:@"formatRequestReceipt"];
        [devices addObject:copy];
    }
    result[@"devices"] = devices;
    return result;
}
bool hardwareIntentRestored(NSDictionary* current, NSDictionary* previous) {
    NSDictionary* before = previous[@"runtime"][@"hardware"];
    NSDictionary* after = current[@"runtime"][@"hardware"];
    if (![before isKindOfClass:NSDictionary.class])
        return true;
    if (![after isKindOfClass:NSDictionary.class])
        return false;
    if (before[@"routingBufferFrames"] &&
        ![after[@"routingBufferFrames"] isEqual:before[@"routingBufferFrames"]])
        return false;
    const auto preferences = [](NSDictionary* hardware) {
        NSMutableDictionary* result = [NSMutableDictionary dictionary];
        for (NSDictionary* entry in hardware[@"bufferPreferences"])
            if ([entry[@"uid"] isKindOfClass:NSString.class] &&
                [entry[@"frames"] isKindOfClass:NSNumber.class])
                result[entry[@"uid"]] = entry[@"frames"];
        return result;
    };
    return [preferences(after) isEqual:preferences(before)];
}
bool otherAccountUsesLegacy(const fs::path& currentHome) {
    try {
        if (!fs::exists(accounts))
            return true;
        for (const auto& account : fs::directory_iterator(accounts)) {
            if (account.path() == currentHome)
                continue;
            const auto job =
                account.path() / "Library/Application Support/LoopZ/Session/engine.plist";
            try {
                if (!fs::exists(fs::symlink_status(job)))
                    continue;
                NSDictionary* stored = plist(job);
                NSArray* args = stored[@"ProgramArguments"];
                if (![args isKindOfClass:NSArray.class] || !args.count ||
                    [args[0] isEqual:text(legacy / "Contents/MacOS/LoopZ Engine")])
                    return true;
            } catch (...) {
                return true;
            } // Unknown account state retains rollback compatibility.
        }
        return false;
    } catch (...) {
        return true;
    }
}
struct Session {
    uid_t uid;
    fs::path home, job;
    std::string domain, label;
    NSDictionary* expected{nil};
    NSDictionary* expectedJob{nil};
    bool loaded{false}, paused{false};
    NSDictionary* consent{nil};
    bool applicationCapture{false};
    std::string savedRoutes;
    Session(uid_t user, fs::path folder)
        : uid(user), home(std::move(folder)),
          job(home / "Library/Application Support/LoopZ/Session/engine.plist"),
          domain("gui/" + std::to_string(uid)), label(domain + "/" + sessionName) {}
    SessionCommandResult userCommand(std::vector<std::string> args, bool checked = true,
                                     std::chrono::milliseconds timeout = 10s) {
        if (fixture)
            return command(std::move(args), checked, timeout);
        std::vector<std::string> prefix{"/bin/launchctl",
                                        "asuser",
                                        std::to_string(uid),
                                        "/usr/bin/sudo",
                                        "-n",
                                        "-H",
                                        "-u",
                                        "#" + std::to_string(uid)};
        prefix.insert(prefix.end(), args.begin(), args.end());
        try {
            return command(std::move(prefix), checked, timeout);
        } catch (const std::runtime_error& error) {
            throw std::runtime_error(fs::path(args.front()).filename().string() + ": " +
                                     error.what());
        }
    }
    bool isLoaded() {
        auto result = command({"/bin/launchctl", "print", label}, false);
        if (!result.exitCode)
            return true;
        require((result.exitCode == 3 || result.exitCode == 113) &&
                    result.output.find("Could not find service") != std::string::npos,
                "LoopZ's background session could not be inspected.");
        return false;
    }
    void inspect() {
        savedRoutes = audioTreeDigest(home / "Library/Application Support/LoopZ/routes.json");
        loaded = isLoaded();
        if (!loaded && !fs::exists(job))
            return;
        struct stat jobInfo{};
        require(!lstat(job.c_str(), &jobInfo) && jobInfo.st_uid == uid && !(jobInfo.st_mode & 0077),
                "The saved background session has unsafe permissions.");
        expectedJob = plist(job);
        const auto routes = home / "Library/Application Support/LoopZ/routes.json";
        NSDictionary* canonical = @{
            @"Label" : @(sessionName),
            @"ProgramArguments" : @[
                text(preferred / "Contents/MacOS/LoopZ Engine"), @"--routing-document",
                text(routes), @"--editor-service", @"true", @"--block-frames", @"64"
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
        // Read either known path, but never rewrite/migrate it implicitly.
        NSArray* storedArguments = expectedJob[@"ProgramArguments"];
        fs::path jobEngine = preferred;
        if ([storedArguments isKindOfClass:NSArray.class] && storedArguments.count &&
            [storedArguments[0] isEqual:text(legacy / "Contents/MacOS/LoopZ Engine")]) {
            jobEngine = legacy;
            NSMutableDictionary* alternate = [canonical mutableCopy];
            NSMutableArray* arguments = [canonical[@"ProgramArguments"] mutableCopy];
            arguments[0] = text(jobEngine / "Contents/MacOS/LoopZ Engine");
            alternate[@"ProgramArguments"] = arguments;
            canonical = alternate;
        }
        require([expectedJob isEqual:canonical],
                "A different background session uses LoopZ's name.");
        if (!loaded)
            return;
        const auto result = command({"/bin/launchctl", "print", label});
        NSString* output = [[NSString alloc] initWithBytes:result.output.data()
                                                    length:result.output.size()
                                                  encoding:NSUTF8StringEncoding];
        NSMutableArray* lines = [NSMutableArray array];
        for (NSString* line in [output componentsSeparatedByString:@"\n"])
            [lines addObject:[line stringByTrimmingCharactersInSet:NSCharacterSet
                                                                       .whitespaceCharacterSet]];
        require(
            [lines containsObject:[@"path = " stringByAppendingString:text(job)]] &&
                [lines
                    containsObject:[@"program = "
                                       stringByAppendingString:text(
                                                                   jobEngine /
                                                                   "Contents/MacOS/LoopZ Engine")]],
            "A different background session uses LoopZ's name.");
        const auto first = [lines indexOfObject:@"arguments = {"];
        NSArray* arguments = canonical[@"ProgramArguments"];
        require(first != NSNotFound && first + arguments.count + 1 < lines.count,
                "The loaded background session has invalid arguments.");
        for (NSUInteger i = 0; i < arguments.count; ++i)
            require([lines[first + i + 1] isEqual:arguments[i]],
                    "A different background session uses LoopZ's name.");
        require([lines[first + arguments.count + 1] isEqual:@"}"],
                "The loaded background session has unexpected arguments.");
        const auto status = userCommand(
            {"/Applications/LoopZ.app/Contents/MacOS/LoopZ", "--routing-status"}, false);
        if (!status.exitCode)
            expected = dictionary(status.output);
    }
    NSDictionary* migratedJob() const {
        if (!expectedJob)
            return nil;
        NSMutableDictionary* result = [expectedJob mutableCopy];
        NSMutableArray* arguments = [expectedJob[@"ProgramArguments"] mutableCopy];
        arguments[0] = text(engine / "Contents/MacOS/LoopZ Engine");
        result[@"ProgramArguments"] = arguments;
        return result;
    }
    NSDictionary* permissionStatus(const fs::path& bundle) {
        verify(bundle, @(engineBundleIdentifier));
        return dictionary(
            userCommand({(bundle / "Contents/MacOS/LoopZ Engine").string(), "--permission-status"})
                .output);
    }
    void pause() {
        if (!loaded && !expectedJob)
            return;
        if (!isLoaded()) {
            paused = true;
            return;
        }
        loaded = true;
        paused = true; // Once bootout is issued, restoration is required even on timeout/error.
        command({"/bin/launchctl", "bootout", label});
        const auto end = std::chrono::steady_clock::now() + (fixture ? 10ms : 8s);
        while (isLoaded()) {
            require(std::chrono::steady_clock::now() < end, "The LoopZ Engine did not stop.");
            std::this_thread::sleep_for(50ms);
        }
        paused = true;
    }
    void restore(bool verifyState = true) {
        require(audioTreeDigest(home / "Library/Application Support/LoopZ/routes.json") ==
                    savedRoutes,
                "The saved mix changed during audio maintenance. Retry Repair audio.");
        if (!loaded && !paused && !expectedJob) {
            return;
        }
        require([plist(job) isEqual:expectedJob],
                "The saved background session changed during audio maintenance.");
        if (!isLoaded())
            userCommand({"/bin/launchctl", "bootstrap", domain, job.string()});
        loaded = true;
        if (!verifyState || !expected) {
            paused = false;
            return;
        }
        const auto end = std::chrono::steady_clock::now() + (fixture ? 20ms : 25s);
        while (std::chrono::steady_clock::now() < end) {
            try {
                const auto result = userCommand(
                    {"/Applications/LoopZ.app/Contents/MacOS/LoopZ", "--routing-status"}, false,
                    3s);
                if (!result.exitCode) {
                    NSDictionary* state = dictionary(result.output);
                    if (consent)
                        requireMigrationConsent(consent, state[@"runtime"][@"permissions"]);
                    if (applicationCapture)
                        requireApplicationCaptureConsent(state);
                    if ([normalizedIntent(state[@"document"])
                            isEqual:normalizedIntent(expected[@"document"])] &&
                        [state[@"runtime"][@"started"] isEqual:@YES] &&
                        hardwareIntentRestored(state, expected) &&
                        (!applicationCapture || applicationCaptureRestored(state)) &&
                        (![expected[@"runtime"][@"restored"] isEqual:@YES] ||
                         [state[@"runtime"][@"restored"] isEqual:@YES])) {
                        paused = false;
                        return;
                    }
                }
            } catch (const EngineConsentLost&) {
                throw;
            } catch (...) {
            }
            std::this_thread::sleep_for(fixture ? 1ms : 100ms);
        }
        throw std::runtime_error(
            "The saved audio graph is still reconnecting. Use Repair audio to retry.");
    }
};
void requireIdle(NSDictionary* catalog, NSSet* savedDevices = nil) {
    for (NSString* key in @[ @"routingBlockingRoutes", @"loopzBlockingRoutes" ])
        require([catalog[key] isKindOfClass:NSArray.class] && [catalog[key] count] == 0,
                "Quit apps using LoopZ as an audio device, then try again. Your saved mix is "
                "preserved.");
    for (NSString* key in @[ @"defaultInput", @"defaultOutput", @"defaultSystemOutput" ])
        require([catalog[key] isKindOfClass:NSString.class] &&
                    (![catalog[key] hasPrefix:@"com.lexasoft.loopz."] ||
                     [savedDevices containsObject:catalog[key]]),
                "Choose a physical system audio device before updating LoopZ audio.");
    require([catalog[@"routingDevices"] isKindOfClass:NSArray.class],
            "The audio device catalog could not be checked.");
    for (NSDictionary* device in catalog[@"routingDevices"])
        require([device[@"running"] isEqual:@NO],
                "Quit apps currently using LoopZ, then try again.");
}
void brokerStop() {
    const auto status = command({"/bin/launchctl", "print", brokerLabel}, false);
    if (!status.exitCode)
        command({"/bin/launchctl", "bootout", brokerLabel});
    else
        require(status.exitCode == 3 || status.exitCode == 113,
                "The LoopZ audio service could not be checked.");
}
void brokerStart() {
    const auto result = command({"/bin/launchctl", "print", brokerLabel}, false);
    if (result.exitCode)
        command({"/bin/launchctl", "bootstrap", "system", daemon.string()});
}
class AudioCatalog {
  public:
    explicit AudioCatalog(fs::path payload)
        : checker_(std::move(payload) / "loopz-routing-install-check") {}
    NSDictionary* read() {
        if (fixture)
            return fixture->catalog();
        if (!worker_)
            worker_ = std::make_unique<CatalogChannel>(
                std::vector<std::string>{checker_.string(), "--snapshot-server"});
        const auto sequence = ++sequence_;
        NSDictionary* reply = nil;
        try {
            reply = dictionary(worker_->exchange("snapshot " + std::to_string(sequence) + "\n"));
        } catch (...) {
            worker_.reset();
            throw;
        }
        require([reply[@"request"] isKindOfClass:NSNumber.class] &&
                    [reply[@"request"] unsignedLongLongValue] == sequence && reply.count == 2,
                "The audio catalog checker returned an invalid acknowledgement.");
        if ([reply[@"error"] isKindOfClass:NSString.class])
            throw std::runtime_error("The audio catalog did not settle: " +
                                     std::string([reply[@"error"] UTF8String]));
        require([reply[@"catalog"] isKindOfClass:NSDictionary.class],
                "The audio catalog checker returned an invalid snapshot.");
        return reply[@"catalog"];
    }

  private:
    fs::path checker_;
    uint64_t sequence_{0};
    std::unique_ptr<CatalogChannel> worker_;
};

struct DriverNotDiscovered : std::runtime_error {
    DriverNotDiscovered() : std::runtime_error("macOS has not discovered the new audio driver.") {}
};
void recordPendingActivation(uid_t uid) {
    const auto marker = support / "activation-required.plist";
    if (fs::exists(marker))
        validateInstalledEntry(marker, false, uid);
    NSData* bytes =
        [NSPropertyListSerialization dataWithPropertyList:@{
            @"version" : @1
        }
                                                   format:NSPropertyListBinaryFormat_v1_0
                                                  options:0
                                                    error:nil];
    require([bytes writeToFile:text(marker) atomically:YES],
            "The pending audio activation could not be saved.");
    require(!chmod(marker.c_str(), 0644), "The pending audio activation could not be protected.");
    syncAudioTree(marker);
}
Process activateAudioService(const Process& previous) {
    if (fixture) {
        fixture->activate();
        return previous;
    }
    verify(previous.path, @"com.apple.audio.coreaudiod", true);
    require(coreAudio() == previous && identity(previous.pid) == previous,
            "The audio service changed before activation. Try again.");
    require(kill(previous.pid, SIGKILL) == 0, "macOS did not allow audio activation.");
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            const auto current = coreAudio();
            if (!(current == previous))
                return current;
        } catch (...) {
        }
        std::this_thread::sleep_for(100ms);
    }
    throw std::runtime_error(
        "The audio service is still restarting. Wait, then choose Repair audio.");
}
void restoreAudioDefaults(NSDictionary* before, AudioCatalog& catalog, bool physicalOnly = false) {
    if (fixture) {
        if (fixture->restoreDefaults)
            fixture->restoreDefaults(before);
        NSDictionary* after = catalog.read();
        for (NSString* key in @[ @"defaultInput", @"defaultOutput", @"defaultSystemOutput" ])
            require([before[key] isEqual:after[key]], "Fixture audio defaults changed.");
        return; // A fixture can never reach the hardware setter, including on failure.
    }
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (true) {
        try {
            NSDictionary* after = catalog.read();
            const std::array<std::pair<NSString*, AudioObjectPropertySelector>, 3> selections{
                {{@"defaultInput", kAudioHardwarePropertyDefaultInputDevice},
                 {@"defaultOutput", kAudioHardwarePropertyDefaultOutputDevice},
                 {@"defaultSystemOutput", kAudioHardwarePropertyDefaultSystemOutputDevice}}};
            for (const auto& [key, selector] : selections) {
                if (physicalOnly && [before[key] hasPrefix:@"com.lexasoft.loopz.device."])
                    continue;
                if ([after[key] isEqual:before[key]])
                    continue;
                require([before[key] isKindOfClass:NSString.class] && [before[key] length],
                        "The original system audio selection is not available yet.");
                CFStringRef uid = (__bridge CFStringRef)before[key];
                AudioObjectID device = 0;
                UInt32 size = sizeof(device);
                AudioObjectPropertyAddress lookup{kAudioHardwarePropertyTranslateUIDToDevice,
                                                  kAudioObjectPropertyScopeGlobal,
                                                  kAudioObjectPropertyElementMain};
                require(!AudioObjectGetPropertyData(kAudioObjectSystemObject, &lookup, sizeof(uid),
                                                    &uid, &size, &device) &&
                            device,
                        "The original audio device is still reconnecting.");
                AudioObjectPropertyAddress selection{selector, kAudioObjectPropertyScopeGlobal,
                                                     kAudioObjectPropertyElementMain};
                require(!AudioObjectSetPropertyData(kAudioObjectSystemObject, &selection, 0,
                                                    nullptr, sizeof(device), &device),
                        "The original audio selection could not be restored.");
            }
            after = catalog.read();
            for (const auto& [key, selector] : selections) {
                (void)selector;
                if (physicalOnly && [before[key] hasPrefix:@"com.lexasoft.loopz.device."])
                    continue;
                require([after[key] isEqual:before[key]],
                        "The original audio selection is still reconnecting.");
            }
            return;
        } catch (...) {
            if (std::chrono::steady_clock::now() >= deadline)
                throw;
            std::this_thread::sleep_for(100ms);
        }
    }
}
NSDictionary* temporaryGraph(Session& session, const fs::path& payload, const fs::path& directory,
                             bool publish, AudioCatalog& catalog) {
    if (fixture) {
        fixture->validateGraph();
        return @{};
    }
    const auto file = directory / "validation.json";
    const auto json = audioValidationDocument(publish);
    NSData* bytes = [NSData dataWithBytes:json.data() length:json.size()];
    require([bytes writeToFile:text(file) atomically:YES],
            "The temporary audio check could not be prepared.");
    require(!chmod(file.c_str(), 0644), "The temporary audio check could not be opened.");
    const auto stopFile = directory / "validation.stop";
    fs::remove(stopFile);
    auto child = std::async(std::launch::async, [&] {
        return session.userCommand(
            {(payload / "LoopZ Engine.app/Contents/MacOS/LoopZ Engine").string(),
             "--routing-document", file.string(), "--installation-validation", "true", "--seconds",
             "25", "--validation-stop-file", stopFile.string()},
            true, 35s);
    });
    struct ValidationStop {
        fs::path path;
        bool request() const {
            const int fd =
                open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd < 0)
                return errno == EEXIST;
            return close(fd) == 0;
        }
        ~ValidationStop() {
            request();
        }
    } stop{stopFile}; // Signal before the future waits, including on a failed catalog check.
    const auto model = decodeRouteDocumentJSON(json);
    const auto uid =
        publish ? "com.lexasoft.loopz.device." + model.devices.front().id : std::string{};
    NSDictionary* visibleDevice = nil;
    bool catalogVerified = false;
    do {
        NSDictionary* state = catalog.read();
        NSArray* devices = state[@"routingDevices"];
        if ([devices isKindOfClass:NSArray.class]) {
            if (!publish && devices.count == 0)
                catalogVerified = true;
            if (publish)
                for (NSDictionary* device in devices) {
                    if ([device[@"uid"] isEqual:@(uid.c_str())] &&
                        [device[@"alive"] isEqual:@YES] && [device[@"inputChannels"] isEqual:@2] &&
                        [device[@"outputChannels"] isEqual:@0] &&
                        [device[@"sampleRate"] isEqual:@48000]) {
                        visibleDevice = device;
                        catalogVerified = true;
                    }
                }
        }
        if (catalogVerified)
            break;
        std::this_thread::sleep_for(100ms);
    } while (child.wait_for(0ms) != std::future_status::ready);
    require(stop.request(), "The temporary audio check could not be stopped.");
    const auto result = child.get();
    bool configured = false;
    NSDictionary* last = nil;
    NSString* output = [[NSString alloc] initWithBytes:result.output.data()
                                                length:result.output.size()
                                              encoding:NSUTF8StringEncoding];
    for (NSString* line in [output componentsSeparatedByString:@"\n"]) {
        if (!line.length)
            continue;
        NSData* data = [line dataUsingEncoding:NSUTF8StringEncoding];
        id state = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
        if ([state isKindOfClass:NSDictionary.class])
            last = state;
        if ([state isKindOfClass:NSDictionary.class] && [state[@"phase"] isEqual:@"configured"] &&
            [state[@"devices"] isKindOfClass:NSArray.class] &&
            [state[@"devices"] count] == (publish ? 1 : 0))
            configured = true;
    }
    if (!configured && [last[@"phase"] isEqual:@"waitingForDriver"])
        throw DriverNotDiscovered();
    if (!configured)
        throw std::runtime_error(
            "The audio driver did not acknowledge its validation graph: " +
            std::string(last ? [last description].UTF8String : "no verification response"));
    require(catalogVerified, "The validation device was not visible in the public audio catalog.");
    return visibleDevice;
}
void loadDriver() {
    if (fixture)
        return;
    CFStringRef bundle = CFSTR("com.lexasoft.loopz.driver");
    AudioObjectID object = 0;
    UInt32 size = sizeof(object);
    AudioObjectPropertyAddress address{kAudioHardwarePropertyTranslateBundleIDToPlugIn,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, sizeof(bundle), &bundle, &size,
                               &object);
}
std::vector<AudioInstallEntry> entries(const fs::path& payload) {
    std::vector<AudioInstallEntry> result;
    for (const auto* name : {"loopz-routing-broker", "loopz-config", "loopz-routing-install-check",
                             "routing-service.json"})
        result.push_back({payload / name, support / name});
    result.push_back({payload / "com.lexasoft.loopz.routing.plist", daemon});
    result.push_back({payload / "LoopZAudio.driver", driver});
    result.push_back({payload / "LoopZ Engine.app", engine});
    return result;
}
} // namespace

std::string audioValidationDocument(bool publish) {
    RouteDocument document;
    if (publish) {
        VirtualDeviceDefinition device;
        device.id = NSUUID.UUID.UUIDString.lowercaseString.UTF8String;
        device.name = "LoopZ setup validation";
        document.devices.push_back(std::move(device));
    }
    return encodeRouteDocumentJSON(document);
}

void verifyAudioPayload(const fs::path& app) {
    if (fixture) {
        verify(app, @"com.lexasoft.loopz.editor");
        return;
    }
    (void)audioTreeDigest(app); // Bounded, no links or special files.
    verify(app, @"com.lexasoft.loopz.editor");
    const auto payload = app / "Contents/Resources/AudioComponents";
    verify(payload / "LoopZAudio.driver", @"com.lexasoft.loopz.driver");
    verify(payload / "LoopZ Engine.app", @(engineBundleIdentifier));
    verify(payload / "loopz-routing-broker", @"com.lexasoft.loopz.routing.broker");
    verify(payload / "loopz-config", @"com.lexasoft.loopz.config");
    verify(payload / "loopz-routing-install-check", @"com.lexasoft.loopz.routing.install-check");
    verify(app / "Contents/Library/Helpers/LoopZSetup", @"com.lexasoft.loopz.setup");
    NSDictionary* expectedDaemon = @{
        @"AssociatedBundleIdentifiers" : @[ @"com.lexasoft.loopz.editor" ],
        @"Label" : @"com.lexasoft.loopz.routing",
        @"ProgramArguments" : @[
            text(support / "loopz-routing-broker"), @"--engine-requirement",
            pin(@(engineBundleIdentifier))
        ],
        @"MachServices" : @{@"com.lexasoft.loopz.routing.bridge" : @YES},
        @"KeepAlive" : @YES,
        @"RunAtLoad" : @YES,
        @"ProcessType" : @"Background",
        @"ThrottleInterval" : @3
    };
    require([plist(payload / "com.lexasoft.loopz.routing.plist") isEqual:expectedDaemon],
            "The bundled audio service configuration is invalid.");
    NSDictionary* expectedMaintenance = @{
        @"AssociatedBundleIdentifiers" : @[ @"com.lexasoft.loopz.editor" ],
        @"Label" : @"com.lexasoft.loopz.setup",
        @"BundleProgram" : @"Contents/Library/Helpers/LoopZSetup",
        @"ProgramArguments" : @[ @"LoopZSetup", @"--service" ],
        @"MachServices" : @{@"com.lexasoft.loopz.setup" : @YES},
        @"ProcessType" : @"Background"
    };
    require([plist(app / "Contents/Library/LaunchDaemons/com.lexasoft.loopz.setup.plist")
                isEqual:expectedMaintenance],
            "The bundled maintenance service configuration is invalid.");
    NSDictionary* expectedPin =
        @{@"protocol" : @2,
          @"brokerRequirement" : pin(@"com.lexasoft.loopz.routing.broker")};
    for (const auto& file :
         {payload / "routing-service.json",
          payload / "LoopZAudio.driver/Contents/Resources/routing-service.json"}) {
        NSData* data = [NSData dataWithContentsOfFile:text(file)];
        id value = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
        require([value isEqual:expectedPin], "The bundled driver trust policy is invalid.");
    }
}

NSDictionary* maintainAudioComponents(const fs::path& app, bool allowAudioActivation,
                                      uid_t expectedUser) {
    engine = preferred;
    require(fixture || (geteuid() == 0 && app == "/Applications/LoopZ.app"),
            "Audio maintenance needs administrator authorization.");
    for (const auto& path : {driver.parent_path(), daemon.parent_path(), support.parent_path()})
        protectedDirectory(path);
    require(fixture || !fs::exists("/Library/Audio/Plug-Ins/HAL/LoopZRoutingAudio.driver"),
            "This separate routing driver layout cannot be maintained in-app. Contact support "
            "before changing audio components.");
    if (!fs::exists(support)) {
        require(fs::create_directory(support), "The audio service directory could not be created.");
        chmod(support.c_str(), 0755);
    }
    protectedDirectory(support);
    const int lock =
        open((support / ".install.lock").c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0644);
    struct stat lockInfo{};
    require(lock >= 0 && !fstat(lock, &lockInfo) && S_ISREG(lockInfo.st_mode) &&
                lockInfo.st_uid == installOwner() && lockInfo.st_nlink == 1 &&
                !(lockInfo.st_mode & 0022),
            "Audio maintenance lock is not trusted.");
    if (flock(lock, LOCK_EX | LOCK_NB)) {
        close(lock);
        throw std::runtime_error("Another LoopZ update is running. Try again shortly.");
    }
    struct Lock {
        int fd;
        ~Lock() {
            close(fd);
        }
    } lease{lock};
    struct stat console{};
    if (fixture)
        console.st_uid = getuid();
    require(fixture || (!stat("/dev/console", &console) && console.st_uid >= 501),
            "Sign in to your Mac before repairing audio.");
    require(!expectedUser || console.st_uid == expectedUser,
            "The active user changed. Retry audio maintenance.");
    const auto requireSameUser = [&] {
        if (fixture)
            return;
        struct stat active{};
        require(!stat("/dev/console", &active) && active.st_uid == console.st_uid,
                "The active user changed. Retry audio maintenance.");
    };
    const auto* user = getpwuid(console.st_uid);
    require(user && user->pw_dir, "The signed-in account could not be found.");
    Session session(console.st_uid, fixture ? fixture->root / "home" : fs::path(user->pw_dir));
    struct JobDirectory {
        int fd{-1};
        ~JobDirectory() {
            if (fd >= 0)
                close(fd);
        }
    } jobDirectory;
    if (fs::exists(fs::symlink_status(session.job.parent_path()))) {
        jobDirectory.fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        require(jobDirectory.fd >= 0, "The saved background session directory is unsafe.");
        for (const auto& part : session.job.parent_path().relative_path()) {
            const int next = openat(jobDirectory.fd, part.c_str(),
                                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            struct stat info{};
            const bool safe = next >= 0 && !fstat(next, &info) && S_ISDIR(info.st_mode) &&
                              (info.st_uid == 0 || info.st_uid == console.st_uid) &&
                              (!(info.st_mode & 0022) || (fixture && (info.st_mode & S_ISVTX)));
            if (!safe) {
                if (next >= 0)
                    close(next);
                throw std::runtime_error("The saved background session directory is unsafe.");
            }
            close(jobDirectory.fd);
            jobDirectory.fd = next;
        }
    }
    const auto originalCore = coreAudio();
    auto expectedCore = originalCore;
    bool audioServiceActivated = false;
    const auto snapshot = support / (".payload-" + std::string(NSUUID.UUID.UUIDString.UTF8String));
    require(fs::create_directory(snapshot), "The audio maintenance payload could not be prepared.");
    chmod(snapshot.c_str(), 0700);
    struct Snapshot {
        fs::path path;
        ~Snapshot() {
            std::error_code e;
            fs::remove_all(path, e);
        }
    } snapshotLease{snapshot};
    fs::copy(app, snapshot / "LoopZ.app",
             fs::copy_options::recursive | fs::copy_options::copy_symlinks);
    protectAudioTree(snapshot / "LoopZ.app", installOwner(), installGroup());
    verifyAudioPayload(snapshot / "LoopZ.app");
    const auto payload = snapshot / "LoopZ.app/Contents/Resources/AudioComponents";
    chmod(snapshot.c_str(), 0711); // Signed, root-owned binaries may run as the console account.
    auto plan = entries(payload);
    bool prepared = false, filesPromoted = false, recovered = false, rollbackCompleted = false;
    AudioCatalog catalog(payload);
    NSDictionary* originalCatalog = catalog.read();
    const auto context = support / "maintenance-session.plist";
    if (fs::exists(context) && !fs::exists(support / "maintenance.plist")) {
        validateInstalledEntry(context, false, console.st_uid);
        fs::remove(
            context); // No transaction remains: snapshot this account's current state afresh.
    }
    if (!fs::exists(context))
        session.inspect();
    auto host = isolatedHost();
    const bool inheritedContext = fs::exists(context);
    bool createdContext = false;
    bool legacyRecovery = false;
    bool pendingMigration = false, rewriteJob = true, retireLegacy = true;
    if (fs::exists(context)) {
        validateInstalledEntry(context, false, console.st_uid);
        NSMutableDictionary* pending = [plist(context, 32 * 1024 * 1024) mutableCopy];
        for (NSString* key in @[ @"state", @"catalog", @"consent" ]) {
            if (![pending[key] isKindOfClass:NSData.class])
                continue;
            id value = [NSJSONSerialization JSONObjectWithData:pending[key] options:0 error:nil];
            require([value isKindOfClass:NSDictionary.class], "Invalid audio recovery snapshot.");
            pending[key] = value;
        }
        require(([pending[@"version"] isEqual:@1] || [pending[@"version"] isEqual:@2]) &&
                    [pending[@"uid"] isEqual:@(console.st_uid)] &&
                    [pending[@"loaded"] isKindOfClass:NSNumber.class],
                "Sign in to the account that started audio maintenance before repairing it.");
        if ([pending[@"legacyTarget"] isEqual:@YES])
            engine = legacy;
        if (pending[@"rewriteJob"])
            rewriteJob = [pending[@"rewriteJob"] boolValue];
        if (pending[@"retireLegacy"])
            retireLegacy = [pending[@"retireLegacy"] boolValue];
        legacyRecovery = [pending[@"version"] isEqual:@1];
        if (legacyRecovery) {
            // Slice 1 journals used the legacy Engine target and did not replace
            // user jobs. Recover that fixed plan before starting the new plan.
            session.inspect();
            session.loaded = session.loaded || [pending[@"loaded"] boolValue];
        } else {
            if ([pending[@"catalog"] isKindOfClass:NSDictionary.class])
                originalCatalog = pending[@"catalog"];
            session.loaded = [pending[@"loaded"] boolValue];
            session.expectedJob = pending[@"job"];
            session.expected = pending[@"state"];
            session.consent = pending[@"consent"];
            if ([pending[@"verified"] isEqual:@YES] && !fs::exists(support / "maintenance.plist") &&
                session.expectedJob) {
                session.expectedJob = session.migratedJob();
            }
            require([pending[@"routes"] isKindOfClass:NSString.class],
                    "Invalid audio recovery context.");
            session.savedRoutes = [pending[@"routes"] UTF8String];
        }
        // A loaded replacement must still be our exact known job before bootout.
        if (session.isLoaded()) {
            Session observed(console.st_uid, session.home);
            observed.inspect();
        }
    }
    NSMutableSet* savedDevices = [NSMutableSet set];
    if (session.loaded && session.expected)
        for (NSDictionary* device in session.expected[@"document"][@"devices"])
            if ([device[@"id"] isKindOfClass:NSString.class])
                [savedDevices addObject:[@"com.lexasoft.loopz.device."
                                            stringByAppendingString:device[@"id"]]];
    requireIdle(originalCatalog, savedDevices);
    const auto routesPath = session.home / "Library/Application Support/LoopZ/routes.json";
    NSDictionary* document = nil;
    if (fs::exists(routesPath)) {
        NSData* data = boundedData(routesPath, 8 * 1024 * 1024);
        document = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
        require([document isKindOfClass:NSDictionary.class],
                "The saved mix needs attention before audio maintenance.");
        // Validate the complete format before applying migration policy.
        decodeRouteDocumentJSON(std::string(static_cast<const char*>(data.bytes), data.length));
    }
    session.applicationCapture = applicationCaptureDocument(document);
    pendingMigration = accountMigrationPending(document, session.expectedJob, legacy,
                                               fixture ? fixture->migrateApplicationCapture
                                                       : kMigrateApplicationCaptureGraphs);
    bool captureMigration = session.applicationCapture && session.expectedJob &&
                            accountUsesLegacy(session.expectedJob, legacy) && !pendingMigration;
    if (!inheritedContext) {
        if (pendingMigration) {
            engine = legacy;
            rewriteJob = false;
            retireLegacy = false;
        } else
            retireLegacy = accountUsesLegacy(session.expectedJob, legacy) &&
                           !otherAccountUsesLegacy(session.home);
    }
    // Gate-off maintenance still replaces a damaged legacy Engine and repairs
    // the driver/broker, without rewriting the user job or retiring its path.
    auto engineEntry = std::find_if(plan.begin(), plan.end(), [&](const auto& entry) {
        return entry.source.filename() == "LoopZ Engine.app";
    });
    require(engineEntry != plan.end(), "The Engine maintenance entry is missing.");
    engineEntry->target = engine;
    const size_t engineIndex = static_cast<size_t>(std::distance(plan.begin(), engineEntry));
    for (const auto& entry : plan)
        validateInstalledEntry(entry.target, entry.target == legacy, console.st_uid);
    if (session.expectedJob && !session.expected && document)
        session.expected = @{@"document" : document, @"runtime" : @{}};
    const auto baselineEngine =
        session.expectedJob ? fs::path([session.expectedJob[@"ProgramArguments"][0] UTF8String])
                                  .parent_path()
                                  .parent_path()
                                  .parent_path()
                            : readEnginePath(engine, legacy);
    if (!session.consent && readableEngineBundle(baselineEngine)) {
        try {
            session.consent = session.permissionStatus(baselineEngine);
        } catch (...) {
            session.consent = nil;
        } // Damaged Engines must remain repairable.
    }
    // asuser from a daemon may not establish the same TCC attribution. Only a
    // positively authorized baseline can establish a grant that must survive.
    if (![session.consent[@"microphone"] isEqual:@"authorized"])
        session.consent = nil;
    if (session.expectedJob && rewriteJob) {
        const auto source = snapshot / "engine.plist";
        NSData* data =
            [NSPropertyListSerialization dataWithPropertyList:session.migratedJob()
                                                       format:NSPropertyListBinaryFormat_v1_0
                                                      options:0
                                                        error:nil];
        require([data writeToFile:text(source) atomically:YES] && !chmod(source.c_str(), 0600),
                "The background job could not be staged.");
        require(jobDirectory.fd >= 0, "The saved background session directory is unavailable.");
        plan.push_back({source, session.job, jobDirectory.fd});
    }
    // Retirement is last and occurs only after routing and consent verification.
    validateInstalledEntry(legacy, true, console.st_uid);
    if (retireLegacy)
        plan.push_back({{}, legacy});
    AudioInstallTransaction transaction(support, plan);
    NSMutableDictionary* record = [@{
        @"version" : @2,
        @"uid" : @(console.st_uid),
        @"loaded" : @(session.loaded),
        @"routes" : @(session.savedRoutes.c_str())
    } mutableCopy];
    record[@"legacyTarget"] = @(engine == legacy);
    record[@"rewriteJob"] = @(rewriteJob);
    record[@"retireLegacy"] = @(retireLegacy);
    record[@"catalog"] = originalCatalog;
    if (session.expectedJob)
        record[@"job"] = session.expectedJob;
    if (session.expected)
        record[@"state"] = session.expected;
    if (session.consent)
        record[@"consent"] = session.consent;
    if (!fs::exists(context) || (legacyRecovery && !fs::exists(support / "maintenance.plist"))) {
        saveSessionContext(record);
        createdContext = !inheritedContext;
    }
    try {
        requireSameUser();
        const auto prepare = [&] {
            transaction.prepare([&](const fs::path& staging) {
                const auto staged = staging / std::to_string(engineIndex);
                verifyEngineStage(staged);
                const auto executableCopy = snapshot / "capability-Engine.app";
                if (fs::exists(executableCopy))
                    fs::remove_all(executableCopy);
                fs::copy(staged, executableCopy, fs::copy_options::recursive);
                verifyEngineStage(executableCopy);
                require(audioTreeDigest(executableCopy) == audioTreeDigest(staged),
                        "The staged capability copy changed.");
                const auto capabilities = dictionary(
                    session
                        .userCommand({(executableCopy / "Contents/MacOS/LoopZ Engine").string(),
                                      "--runtime-capabilities"})
                        .output);
                require([capabilities[@"backgroundSession"] isEqual:@1],
                        "The staged Engine cannot run background routing.");
            });
            prepared = true;
            checkpoint("prepared");
        };
        if (!fs::exists(support / "maintenance.plist"))
            prepare();
        session.pause();
        const bool completed =
            !prepared && [plist(support / "maintenance.plist")[@"phase"] isEqual:@"committed"];
        if (!prepared) {
            if (legacyRecovery) {
                auto oldPlan = entries(payload);
                oldPlan.back().target = legacy;
                AudioInstallTransaction oldTransaction(support, oldPlan);
                recovered = oldTransaction.recover();
                saveSessionContext(record);
            } else
                recovered = transaction.recover();
        }
        if (completed && !legacyRecovery && session.expectedJob) {
            session.expectedJob = session.migratedJob();
            record[@"job"] = session.expectedJob;
            pendingMigration = accountMigrationPending(document, session.expectedJob, legacy,
                                                       fixture ? fixture->migrateApplicationCapture
                                                               : kMigrateApplicationCaptureGraphs);
            captureMigration = applicationCaptureDocument(document) && session.expectedJob &&
                               accountUsesLegacy(session.expectedJob, legacy) && !pendingMigration;
            saveSessionContext(record);
        }
        if (session.expectedJob)
            require([plist(session.job) isEqual:session.expectedJob],
                    "The background job did not recover its original definition.");
        if (recovered && pendingMigration && engine != legacy &&
            (!session.expectedJob || [session.expectedJob[@"ProgramArguments"][0]
                                         isEqual:text(legacy / "Contents/MacOS/LoopZ Engine")])) {
            // Finish recovery using its recorded plan, then apply today's gate
            // to a fresh in-place repair plan. A gate flip never resumes a move.
            engine = legacy;
            rewriteJob = false;
            retireLegacy = false;
            plan = entries(payload);
            transaction = AudioInstallTransaction(support, plan);
            record[@"legacyTarget"] = @YES;
            record[@"rewriteJob"] = @NO;
            record[@"retireLegacy"] = @NO;
            record[@"verified"] = @NO;
            saveSessionContext(record);
        }
        if (recovered) {
            brokerStop();
            brokerStart();
            reload(isolatedHost());
        }
        // Idle public endpoints can remain published while their service is
        // repaired; the validation graph replaces them after service recovery.
        // Never depend on the broken service to withdraw its own devices.
        requireIdle(catalog.read(), savedDevices);
        const bool hadDriver = fs::exists(driver);
        const bool changedDriver =
            audioTreeDigest(driver) != audioTreeDigest(payload / "LoopZAudio.driver");
        if (!prepared)
            prepare();
        brokerStop();
        requireSameUser();
        filesPromoted = true; // Promotion can fail after replacing only part of the plan.
        transaction.promote();
        for (const auto& entry : plan)
            if (!entry.source.empty() && entry.target != session.job)
                protectAudioTree(entry.target, installOwner(), installGroup());
        if (session.expectedJob && rewriteJob) {
            const int fd =
                openat(jobDirectory.fd, "engine.plist", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            struct stat info{};
            const bool owned = fd >= 0 && !fstat(fd, &info) && S_ISREG(info.st_mode) &&
                               info.st_nlink == 1 && !fchown(fd, console.st_uid, user->pw_gid) &&
                               !fchmod(fd, 0600) && !fsync(fd);
            if (fd >= 0)
                close(fd);
            require(owned, "The background job could not be published.");
            session.expectedJob = session.migratedJob();
        }
        checkpoint("promoted");
        // Query from the new identity before bootstrapping any capture graph.
        if (session.consent)
            requireMigrationConsent(session.consent, session.permissionStatus(engine));
        brokerStart();
        if (changedDriver && host)
            reload(host);
        require(coreAudio() == originalCore, "Core Audio changed during audio maintenance.");
        const auto pendingActivation = [&]() -> NSDictionary* {
            recordPendingActivation(console.st_uid);
            const auto after = catalog.read();
            for (NSString* key in @[ @"defaultInput", @"defaultOutput", @"defaultSystemOutput" ])
                require([after[key] isEqual:originalCatalog[key]],
                        "System audio selections changed during maintenance.");
            require(coreAudio() == originalCore, "Core Audio changed during audio maintenance.");
            if (retireLegacy && legacyEnginePresent(legacy)) {
                record[@"verified"] = @NO;
                saveSessionContext(record);
                brokerStop();
                session.pause();
                transaction.rollback();
                checkpoint("activation-rolled-back");
                prepared = false;
                session.expectedJob = record[@"job"];
                session.consent = nil;
                session.applicationCapture = false;
                if (fs::exists(daemon))
                    brokerStart();
            } else {
                session.applicationCapture = captureMigration && engine != legacy;
                require(!session.applicationCapture || session.loaded,
                        "Application capture was not running; the legacy Engine was retained.");
                session.restore();
                restoreAudioDefaults(originalCatalog, catalog);
                record[@"verified"] = @YES;
                saveSessionContext(record);
                transaction.retire();
                transaction.commit();
                prepared = false;
            }
            session.restore(false);
            if (!fs::exists(support / "maintenance.plist"))
                fs::remove(context);
            return @{
                @"ok" : @YES,
                @"activationRequired" : @YES,
                @"validationPending" : @YES,
                @"savedGraphPreserved" : @YES,
                @"coreAudioPreserved" : @YES,
                @"audioDefaultsPreserved" : @YES
            };
        };
        const auto activate = [&] {
            require(allowAudioActivation && !audioServiceActivated,
                    "Audio activation needs your explicit choice in Settings.");
            requireSameUser();
            expectedCore = activateAudioService(expectedCore);
            audioServiceActivated = true;
            restoreAudioDefaults(originalCatalog, catalog, true);
        };
        const bool activationPending = fs::exists(support / "activation-required.plist");
        if (((changedDriver && hadDriver) || activationPending) && !host &&
            [originalCatalog[@"routingPlugIn"][@"registered"] isEqual:@YES]) {
            if (!allowAudioActivation)
                return pendingActivation();
            activate();
        }
        NSDictionary* validatedDevice = nil;
        auto discoveryDeadline = std::chrono::steady_clock::now() + 30s;
        while (true) {
            loadDriver();
            try {
                validatedDevice = temporaryGraph(session, payload, snapshot, true, catalog);
                break;
            } catch (const DriverNotDiscovered&) {
                const auto state = catalog.read();
                const bool missing = !host && [state[@"routingPlugIn"][@"registered"] isEqual:@NO];
                if (missing && allowAudioActivation && !audioServiceActivated) {
                    activate();
                    discoveryDeadline = std::chrono::steady_clock::now() + 20s;
                    continue;
                }
                if (std::chrono::steady_clock::now() >= discoveryDeadline) {
                    if (missing && !allowAudioActivation)
                        return pendingActivation();
                    throw;
                }
                std::this_thread::sleep_for(250ms);
            }
        }
        temporaryGraph(session, payload, snapshot, false, catalog);
        requireSameUser();
        session.applicationCapture = captureMigration && engine != legacy;
        require(!session.applicationCapture || session.loaded,
                "Application capture was not running; the legacy Engine was retained.");
        if (pendingMigration)
            session.applicationCapture = false;
        session.restore();
        // Restore saved virtual defaults only after their original graph is back.
        // macOS may temporarily choose another input while validation runs.
        restoreAudioDefaults(originalCatalog, catalog);
        require(coreAudio() == expectedCore, "The audio service changed during audio validation.");
        require(!retireLegacy || !otherAccountUsesLegacy(session.home),
                "Another account still uses the legacy Engine; the previous installation was "
                "restored.");
        record[@"verified"] = @YES;
        saveSessionContext(record);
        checkpoint("verified");
        require(!retireLegacy || !otherAccountUsesLegacy(session.home),
                "Another account still uses the legacy Engine; the previous installation was "
                "restored.");
        transaction.retire();
        checkpoint("retired");
        transaction.commit();
        prepared = false;
        fs::remove(support / "restart-required");
        fs::remove(support / "activation-required.plist");
        fs::remove(context);
        return @{
            @"ok" : @YES,
            @"migrationPending" : @(pendingMigration),
            @"mode" : @"native-audio-maintenance",
            @"recoveredInterruptedUpdate" : @(recovered),
            @"driverUpdated" : @(changedDriver),
            @"savedGraphPreserved" : @YES,
            @"coreAudioPreserved" : @(!audioServiceActivated),
            @"audioServiceActivated" : @(audioServiceActivated),
            @"publicDeviceVerified" : @YES,
            @"publicDevice" : validatedDevice ?: @{},
            @"captureOpened" : @NO,
            @"physicalMonitorsOpened" : @NO,
            @"audioDefaultsPreserved" : @YES
        };
    } catch (const AudioMaintenanceInterruption&) {
        throw;
    } catch (...) {
        const auto error = std::current_exception();
        try {
            if (prepared) {
                // Clear completion durably before any reverse rename. A crash
                // after rollback must never interpret the old job as committed.
                record[@"verified"] = @NO;
                [record removeObjectForKey:@"consent"];
                session.consent = nil;
                session.applicationCapture = false;
                saveSessionContext(record);
                if (filesPromoted) {
                    brokerStop();
                    session.pause();
                }
                transaction.rollback();
                rollbackCompleted = true;
                checkpoint("rolled-back");
                session.expectedJob = record[@"job"];
                prepared = false;
                if (filesPromoted && fs::exists(daemon))
                    brokerStart();
                if (filesPromoted)
                    reload(isolatedHost());
            }
            // Even a failed bootout may have removed the job; always restore
            // after it was issued, including before any files were promoted.
            std::exception_ptr restoreError;
            if (session.paused || filesPromoted) {
                session.consent = nil;
                session.applicationCapture = false;
                try {
                    session.restore(false);
                } catch (...) {
                    restoreError = std::current_exception();
                }
                try {
                    restoreAudioDefaults(originalCatalog, catalog);
                } catch (...) {
                    if (!restoreError)
                        restoreError = std::current_exception();
                }
            }
            if (!fs::exists(support / "maintenance.plist") &&
                (createdContext || rollbackCompleted || recovered))
                fs::remove(context);
            if (restoreError)
                std::rethrow_exception(restoreError);
        } catch (const EngineConsentLost&) {
            throw;
        } catch (...) {
            if (!fs::exists(support / "maintenance.plist"))
                throw std::runtime_error("Audio maintenance could not finish.");
            throw std::runtime_error("Audio repair could not finish. Recovery files were retained; "
                                     "choose Repair audio again.");
        }
        std::rethrow_exception(error);
    }
}
NSDictionary* maintainAudioComponents(const fs::path& app, bool activate, uid_t user,
                                      AudioMaintenanceFixture& injected) {
    require(!fixture && injected.root.is_absolute() &&
                fs::symlink_status(injected.root).type() == fs::file_type::directory,
            "Invalid maintenance fixture.");
    const auto temporaryRoot = fs::canonical(fs::temp_directory_path());
    const auto fixtureRoot = fs::canonical(injected.root);
    const auto relative = fixtureRoot.lexically_relative(temporaryRoot);
    require(fixtureRoot != temporaryRoot && !relative.empty() && *relative.begin() != "..",
            "Maintenance fixtures require a private temporary directory.");
    const auto savedSupport = support, savedDriver = driver, savedEngine = engine,
               savedPreferred = preferred, savedDaemon = daemon, savedLegacy = legacy,
               savedAccounts = accounts;
    struct Reset {
        std::function<void()> reset;
        ~Reset() {
            reset();
        }
    } reset{[&] {
        support = savedSupport;
        driver = savedDriver;
        engine = savedEngine;
        preferred = savedPreferred;
        daemon = savedDaemon;
        legacy = savedLegacy;
        accounts = savedAccounts;
        fixture = nullptr;
    }};
    fixture = &injected;
    support = injected.root / "Library/Application Support/LoopZ";
    preferred = support / "LoopZ Engine.app";
    engine = preferred;
    driver = injected.root / "HAL/LoopZAudio.driver";
    daemon = injected.root / "LaunchDaemons/com.lexasoft.loopz.routing.plist";
    legacy = injected.root / "Applications/LoopZ Engine.app";
    accounts = injected.root / "Users";
    return maintainAudioComponents(app, activate, user);
}
} // namespace loopz::macos
