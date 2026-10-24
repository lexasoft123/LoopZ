#include <loopz/macos/audio_host_identity.hpp>
#include "audio_maintenance.hpp"
#include "maintenance_files.hpp"
#include "maintenance_host_policy.hpp"
#include "session_host.hpp"
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
#include <optional>
#include <set>
#include <thread>

namespace loopz::macos {
namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
const fs::path support = "/Library/Application Support/LoopZ";
const fs::path driver = "/Library/Audio/Plug-Ins/HAL/LoopZAudio.driver";
const fs::path engine = "/Applications/LoopZ Engine.app";
const fs::path daemon = "/Library/LaunchDaemons/com.lexasoft.loopz.routing.plist";
constexpr auto brokerLabel = "system/com.lexasoft.loopz.routing";
constexpr auto sessionName = "com.lexasoft.loopz.session";
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
void protectedDirectory(const fs::path& path) {
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
                    (info.st_uid == 0 || (userOwned && info.st_uid == uid)) &&
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
NSDictionary* plist(const fs::path& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    require(fd >= 0, "The saved audio session could not be opened.");
    struct stat info{};
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_size < 1 ||
        info.st_size > 65536) {
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
void saveSessionContext(uid_t uid, bool loaded) {
    NSDictionary* record = @{@"version" : @1, @"uid" : @(uid), @"loaded" : @(loaded)};
    NSData* data = [NSPropertyListSerialization dataWithPropertyList:record
                                                              format:NSPropertyListBinaryFormat_v1_0
                                                             options:0
                                                               error:nil];
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
struct Session {
    uid_t uid;
    fs::path home, job;
    std::string domain, label;
    NSDictionary* expected{nil};
    NSDictionary* expectedJob{nil};
    bool loaded{false}, paused{false};
    std::string savedRoutes;
    Session(uid_t user, fs::path folder)
        : uid(user), home(std::move(folder)),
          job(home / "Library/Application Support/LoopZ/Session/engine.plist"),
          domain("gui/" + std::to_string(uid)), label(domain + "/" + sessionName) {}
    SessionCommandResult userCommand(std::vector<std::string> args, bool checked = true,
                                     std::chrono::milliseconds timeout = 10s) {
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
                text(engine / "Contents/MacOS/LoopZ Engine"), @"--routing-document", text(routes),
                @"--editor-service", @"true", @"--block-frames", @"64"
            ],
            @"RunAtLoad" : @YES,
            @"KeepAlive" : @YES,
            @"MachServices" : @{@"com.lexasoft.loopz.editor.control" : @YES},
            @"ProcessType" : @"Interactive",
            @"ThrottleInterval" : @10,
            @"ExitTimeOut" : @5,
            @"StandardOutPath" : @"/dev/null",
            @"StandardErrorPath" : @"/dev/null"
        };
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
                                                                   engine /
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
    void pause() {
        if (!loaded)
            return;
        paused = true;
        if (!isLoaded())
            return;
        command({"/bin/launchctl", "bootout", label});
        const auto end = std::chrono::steady_clock::now() + 8s;
        while (isLoaded()) {
            require(std::chrono::steady_clock::now() < end, "The LoopZ Engine did not stop.");
            std::this_thread::sleep_for(50ms);
        }
    }
    void restore(bool verifyState = true) {
        require(audioTreeDigest(home / "Library/Application Support/LoopZ/routes.json") ==
                    savedRoutes,
                "The saved mix changed during audio maintenance. Retry Repair audio.");
        if (!loaded && !paused)
            return;
        require([plist(job) isEqual:expectedJob],
                "The saved background session changed during audio maintenance.");
        if (!isLoaded())
            userCommand({"/bin/launchctl", "bootstrap", domain, job.string()});
        if (!verifyState || !expected) {
            paused = false;
            return;
        }
        const auto end = std::chrono::steady_clock::now() + 25s;
        while (std::chrono::steady_clock::now() < end) {
            try {
                const auto result = userCommand(
                    {"/Applications/LoopZ.app/Contents/MacOS/LoopZ", "--routing-status"}, false,
                    3s);
                if (!result.exitCode) {
                    NSDictionary* state = dictionary(result.output);
                    if ([normalizedIntent(state[@"document"])
                            isEqual:normalizedIntent(expected[@"document"])] &&
                        [state[@"runtime"][@"started"] isEqual:@YES] &&
                        hardwareIntentRestored(state, expected) &&
                        (![expected[@"runtime"][@"restored"] isEqual:@YES] ||
                         [state[@"runtime"][@"restored"] isEqual:@YES])) {
                        paused = false;
                        return;
                    }
                }
            } catch (...) {
            }
            std::this_thread::sleep_for(100ms);
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
NSDictionary* catalog(const fs::path& payload) {
    return dictionary(
        command({(payload / "loopz-routing-install-check").string(), "--snapshot"}).output);
}

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
void restoreAudioDefaults(NSDictionary* before, const fs::path& payload,
                          bool physicalOnly = false) {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (true) {
        try {
            NSDictionary* after = catalog(payload);
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
            after = catalog(payload);
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
                             bool publish) {
    const auto file = directory / "validation.json";
    const auto json = audioValidationDocument(publish);
    NSData* bytes = [NSData dataWithBytes:json.data() length:json.size()];
    require([bytes writeToFile:text(file) atomically:YES],
            "The temporary audio check could not be prepared.");
    require(!chmod(file.c_str(), 0644), "The temporary audio check could not be opened.");
    auto child = std::async(std::launch::async, [&] {
        return session.userCommand(
            {(payload / "LoopZ Engine.app/Contents/MacOS/LoopZ Engine").string(),
             "--routing-document", file.string(), "--installation-validation", "true", "--seconds",
             "5"},
            true, 12s);
    });
    const auto model = decodeRouteDocumentJSON(json);
    const auto uid =
        publish ? "com.lexasoft.loopz.device." + model.devices.front().id : std::string{};
    NSDictionary* visibleDevice = nil;
    bool catalogVerified = false;
    do {
        NSDictionary* state = catalog(payload);
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
    (void)audioTreeDigest(app); // Bounded, no links or special files.
    verify(app, @"com.lexasoft.loopz.editor");
    const auto payload = app / "Contents/Resources/AudioComponents";
    verify(payload / "LoopZAudio.driver", @"com.lexasoft.loopz.driver");
    verify(payload / "LoopZ Engine.app", @"com.lexasoft.loopz.engine");
    verify(payload / "loopz-routing-broker", @"com.lexasoft.loopz.routing.broker");
    verify(payload / "loopz-config", @"com.lexasoft.loopz.config");
    verify(payload / "loopz-routing-install-check", @"com.lexasoft.loopz.routing.install-check");
    verify(app / "Contents/Library/Helpers/LoopZSetup", @"com.lexasoft.loopz.setup");
    NSDictionary* expectedDaemon = @{
        @"AssociatedBundleIdentifiers" : @[ @"com.lexasoft.loopz.editor" ],
        @"Label" : @"com.lexasoft.loopz.routing",
        @"ProgramArguments" : @[
            text(support / "loopz-routing-broker"), @"--engine-requirement",
            pin(@"com.lexasoft.loopz.engine")
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
    require(geteuid() == 0 && app == "/Applications/LoopZ.app",
            "Audio maintenance needs administrator authorization.");
    for (const auto& path :
         {driver.parent_path(), daemon.parent_path(), engine.parent_path(), support.parent_path()})
        protectedDirectory(path);
    require(!fs::exists("/Library/Audio/Plug-Ins/HAL/LoopZRoutingAudio.driver"),
            "An older separate LoopZ driver is present. Remove that installation before updating.");
    if (!fs::exists(support)) {
        require(fs::create_directory(support), "The audio service directory could not be created.");
        chmod(support.c_str(), 0755);
    }
    protectedDirectory(support);
    const int lock =
        open((support / ".install.lock").c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0644);
    struct stat lockInfo{};
    require(lock >= 0 && !fstat(lock, &lockInfo) && S_ISREG(lockInfo.st_mode) &&
                lockInfo.st_uid == 0 && lockInfo.st_nlink == 1 && !(lockInfo.st_mode & 0022),
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
    require(!stat("/dev/console", &console) && console.st_uid >= 501,
            "Sign in to your Mac before repairing audio.");
    require(!expectedUser || console.st_uid == expectedUser,
            "The active user changed. Retry audio maintenance.");
    const auto requireSameUser = [&] {
        struct stat active{};
        require(!stat("/dev/console", &active) && active.st_uid == console.st_uid,
                "The active user changed. Retry audio maintenance.");
    };
    const auto* user = getpwuid(console.st_uid);
    require(user && user->pw_dir, "The signed-in account could not be found.");
    Session session(console.st_uid, user->pw_dir);
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
    protectAudioTree(snapshot / "LoopZ.app", 0, 0);
    verifyAudioPayload(snapshot / "LoopZ.app");
    const auto payload = snapshot / "LoopZ.app/Contents/Resources/AudioComponents";
    chmod(snapshot.c_str(), 0711); // Signed, root-owned binaries may run as the console account.
    const auto plan = entries(payload);
    for (const auto& entry : plan)
        validateInstalledEntry(entry.target, entry.target == engine, console.st_uid);
    AudioInstallTransaction transaction(support, plan);
    bool prepared = false, filesPromoted = false;
    const auto originalCatalog = catalog(payload);
    session.inspect();
    NSMutableSet* savedDevices = [NSMutableSet set];
    if (session.loaded && session.expected)
        for (NSDictionary* device in session.expected[@"document"][@"devices"])
            if ([device[@"id"] isKindOfClass:NSString.class])
                [savedDevices addObject:[@"com.lexasoft.loopz.device."
                                            stringByAppendingString:device[@"id"]]];
    requireIdle(originalCatalog, savedDevices);
    auto host = isolatedHost();
    const auto context = support / "maintenance-session.plist";
    if (fs::exists(context)) {
        validateInstalledEntry(context, false, console.st_uid);
        NSDictionary* pending = plist(context);
        require([pending[@"version"] isEqual:@1] && [pending[@"uid"] isEqual:@(console.st_uid)] &&
                    [pending[@"loaded"] isKindOfClass:NSNumber.class] &&
                    CFGetTypeID((__bridge CFTypeRef)pending[@"loaded"]) == CFBooleanGetTypeID(),
                "Sign in to the account that started audio maintenance before repairing it.");
        session.loaded = session.loaded || [pending[@"loaded"] boolValue];
        require(!session.loaded || session.expectedJob,
                "The saved routing session needs attention before recovery.");
    }
    saveSessionContext(console.st_uid, session.loaded);
    const bool hadPendingActivation = fs::exists(support / "activation-required.plist");
    try {
        requireSameUser();
        session.pause();
        const bool recovered = transaction.recover();
        if (recovered) {
            brokerStop();
            brokerStart();
            reload(isolatedHost());
        }
        // Idle public endpoints can remain published while their service is
        // repaired; the validation graph replaces them after service recovery.
        // Never depend on the broken service to withdraw its own devices.
        requireIdle(catalog(payload), savedDevices);
        const bool hadDriver = fs::exists(driver);
        const bool changedDriver =
            audioTreeDigest(driver) != audioTreeDigest(payload / "LoopZAudio.driver");
        transaction.prepare();
        prepared = true;
        brokerStop();
        requireSameUser();
        transaction.promote();
        filesPromoted = true;
        for (const auto& entry : plan)
            protectAudioTree(entry.target, entry.target == engine ? console.st_uid : 0,
                             entry.target == engine ? console.st_gid : 0);
        brokerStart();
        if (changedDriver && host)
            reload(host);
        require(coreAudio() == originalCore, "Core Audio changed during audio maintenance.");
        const auto pendingActivation = [&]() -> NSDictionary* {
            recordPendingActivation(console.st_uid);
            const auto after = catalog(payload);
            for (NSString* key in @[ @"defaultInput", @"defaultOutput", @"defaultSystemOutput" ])
                require([after[key] isEqual:originalCatalog[key]],
                        "System audio selections changed during maintenance.");
            require(coreAudio() == originalCore, "Core Audio changed during audio maintenance.");
            transaction.commit();
            prepared = false;
            session.restore(false);
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
            restoreAudioDefaults(originalCatalog, payload, true);
        };
        if (changedDriver && hadDriver && !host &&
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
                validatedDevice = temporaryGraph(session, payload, snapshot, true);
                break;
            } catch (const DriverNotDiscovered&) {
                const auto state = catalog(payload);
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
        temporaryGraph(session, payload, snapshot, false);
        requireSameUser();
        session.restore();
        // Restore saved virtual defaults only after their original graph is back.
        // macOS may temporarily choose another input while validation runs.
        restoreAudioDefaults(originalCatalog, payload);
        require(coreAudio() == expectedCore, "The audio service changed during audio validation.");
        transaction.commit();
        prepared = false;
        fs::remove(support / "restart-required");
        fs::remove(support / "activation-required.plist");
        fs::remove(context);
        return @{
            @"ok" : @YES,
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
    } catch (...) {
        const auto error = std::current_exception();
        try {
            if (prepared) {
                brokerStop();
                transaction.rollback();
                if (!hadPendingActivation)
                    fs::remove(support / "activation-required.plist");
                prepared = false;
                if (fs::exists(daemon))
                    brokerStart();
                if (filesPromoted)
                    reload(isolatedHost());
            }
            session.restore();
            restoreAudioDefaults(originalCatalog, payload);
            fs::remove(context);
        } catch (...) {
            throw std::runtime_error("Audio repair could not finish. Recovery files were retained; "
                                     "choose Repair audio again.");
        }
        std::rethrow_exception(error);
    }
}
} // namespace loopz::macos
