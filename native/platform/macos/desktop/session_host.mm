#include "session_host.hpp"
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <fcntl.h>
#include <libproc.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <stdexcept>

namespace loopz::macos {
namespace {
class AudioMaintenanceLease {
    int fd_{-1};

  public:
    explicit AudioMaintenanceLease(bool production) {
        if (!production)
            return;
        constexpr auto lock = "/Library/Application Support/LoopZ/.install.lock";
        fd_ = open(lock, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd_ < 0 && errno != ENOENT)
            throw std::runtime_error("LoopZ could not check audio maintenance.");
        struct stat info{};
        const bool valid = fd_ < 0 || (!fstat(fd_, &info) && S_ISREG(info.st_mode) &&
                                       info.st_uid == 0 && !(info.st_mode & 0022) &&
                                       info.st_nlink == 1 && !flock(fd_, LOCK_SH | LOCK_NB));
        struct stat journal{};
        const bool recovery =
            !lstat("/Library/Application Support/LoopZ/maintenance.plist", &journal) ||
            !lstat("/Library/Application Support/LoopZ/maintenance-session.plist", &journal);
        if (!valid || recovery) {
            if (fd_ >= 0)
                close(fd_);
            fd_ = -1;
            throw std::runtime_error(
                "Audio maintenance is running or needs recovery. Use Repair audio in Settings.");
        }
    }
    ~AudioMaintenanceLease() {
        if (fd_ >= 0)
            close(fd_);
    }
};
class File {
  public:
    explicit File(int value) : value_(value) {
        if (value < 0)
            throw std::runtime_error("LoopZ could not open its private session files.");
    }
    ~File() {
        close(value_);
    }
    operator int() const {
        return value_;
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;

  private:
    int value_;
};
struct TemporaryJob {
    int directory;
    std::string name;
    ~TemporaryJob() {
        unlinkat(directory, name.c_str(), 0);
    }
};
NSString* text(const std::string& value) {
    auto* result = [[NSString alloc] initWithBytes:value.data()
                                            length:value.size()
                                          encoding:NSUTF8StringEncoding];
    if (!result)
        throw std::invalid_argument("Invalid background session path");
    return result;
}
void privateFile(int file, bool directory) {
    struct stat info{};
    if (fstat(file, &info) || info.st_uid != getuid() || (info.st_mode & 0077) ||
        (directory ? !S_ISDIR(info.st_mode) : (!S_ISREG(info.st_mode) || info.st_nlink != 1)))
        throw std::runtime_error("LoopZ couldn’t open its saved audio setup. Try reopening LoopZ. "
                                 "If this continues, contact support.");
}
File privateDirectory(const std::filesystem::path& path) {
    if (mkdir(path.c_str(), 0700) && errno != EEXIST)
        throw std::runtime_error("LoopZ could not create its session directory.");
    File result(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    // Older builds and other LoopZ settings writers may create this account's
    // directory with ordinary 0755 permissions. Tighten our own opened directory
    // in place; never follow a link or change another account's ownership.
    struct stat info{};
    if (fstat(result, &info) || !S_ISDIR(info.st_mode) || info.st_uid != getuid() ||
        ((info.st_mode & 0077) && fchmod(result, info.st_mode & 0700)))
        throw std::runtime_error("LoopZ couldn’t open its saved audio setup. Try reopening LoopZ. "
                                 "If this continues, contact support.");
    privateFile(result, true);
    // No copying: the returned prvalue constructs its descriptor owner in place.
    return File(fcntl(result, F_DUPFD_CLOEXEC, 0));
}
void staticIdentity(const std::filesystem::path& path, const std::string& pin) {
    SecStaticCodeRef code = nullptr;
    SecRequirementRef requirement = nullptr;
    auto status = SecRequirementCreateWithString((__bridge CFStringRef)text(pin),
                                                 kSecCSDefaultFlags, &requirement);
    if (status == errSecSuccess)
        status = SecStaticCodeCreateWithPath(
            (__bridge CFURLRef)[NSURL fileURLWithPath:text(path.string())], kSecCSDefaultFlags,
            &code);
    if (status == errSecSuccess)
        status = SecStaticCodeCheckValidity(
            code, kSecCSStrictValidate | kSecCSCheckAllArchitectures, requirement);
    if (code)
        CFRelease(code);
    if (requirement)
        CFRelease(requirement);
    if (status != errSecSuccess)
        throw std::runtime_error(
            "Install a matching, signed LoopZ Engine before starting background routing.");
}
bool processIdentity(pid_t pid, const std::filesystem::path& executable, const std::string& pin) {
    std::array<char, PROC_PIDPATHINFO_MAXSIZE> path{};
    if (proc_pidpath(pid, path.data(), path.size()) <= 0 || path.data() != executable.string())
        return false;
    SecCodeRef code = nullptr;
    SecRequirementRef requirement = nullptr;
    auto status = SecRequirementCreateWithString((__bridge CFStringRef)text(pin),
                                                 kSecCSDefaultFlags, &requirement);
    if (status == errSecSuccess)
        status = SecCodeCopyGuestWithAttributes(
            nullptr,
            (__bridge CFDictionaryRef)
                @{(__bridge NSString*)kSecGuestAttributePid : @(pid)},
            kSecCSDefaultFlags, &code);
    if (status == errSecSuccess)
        status = SecCodeCheckValidity(code, kSecCSStrictValidate, requirement);
    if (code)
        CFRelease(code);
    if (requirement)
        CFRelease(requirement);
    return status == errSecSuccess;
}
class Host final : public SessionHost {
    const SessionPaths paths_;
    const std::string pin_;
    const std::string domain_{"gui/" + std::to_string(getuid())};
    std::string target() const {
        return domain_ + "/" + paths_.label;
    }
    std::filesystem::path executable() const {
        return paths_.engine / "Contents/MacOS/LoopZ Engine";
    }
    NSArray* arguments() const {
        // The background profile uses the quantum verified with concurrent
        // 64-frame virtual clients. It preserves source/lookback metadata.
        return @[
            text(executable().string()), @"--routing-document", text(paths_.document.string()),
            @"--editor-service", @"true", @"--block-frames", @"64"
        ];
    }
    NSDictionary* definition() const {
        return @{
            @"Label" : text(paths_.label),
            @"ProgramArguments" : arguments(),
            @"RunAtLoad" : @YES,
            @"KeepAlive" : @YES,
            @"ThrottleInterval" : @10,
            @"ProcessType" : @"Interactive",
            @"ExitTimeOut" : @5,
            @"MachServices" : @{text(paths_.service) : @YES},
            @"StandardOutPath" : @"/dev/null",
            @"StandardErrorPath" : @"/dev/null"
        };
    }
    void validateDefinition(int directory) const {
        File file(openat(directory, "engine.plist", O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        privateFile(file, false);
        struct stat info{};
        if (fstat(file, &info) || info.st_size < 1 || info.st_size > 65536)
            throw std::runtime_error("The background job definition is invalid.");
        NSMutableData* data = [NSMutableData dataWithLength:static_cast<NSUInteger>(info.st_size)];
        size_t done = 0;
        while (done < data.length) {
            const auto count =
                read(file, static_cast<char*>(data.mutableBytes) + done, data.length - done);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                throw std::runtime_error("The background job could not be read.");
            done += static_cast<size_t>(count);
        }
        id stored = [NSPropertyListSerialization propertyListWithData:data
                                                              options:NSPropertyListImmutable
                                                               format:nil
                                                                error:nil];
        if (![stored isEqual:definition()])
            throw std::runtime_error("Another configuration occupies LoopZ’s background job file.");
    }

  public:
    Host(SessionPaths paths, std::string pin) : paths_(std::move(paths)), pin_(std::move(pin)) {
        if (!getuid() || paths_.label.empty() || paths_.service.empty() ||
            !paths_.engine.is_absolute() || !paths_.document.is_absolute() ||
            !paths_.directory.is_absolute())
            throw std::invalid_argument(
                "Background routing requires a user session and absolute paths");
        for (const auto& path : {paths_.engine.string(), paths_.document.string(),
                                 paths_.directory.string(), paths_.label, paths_.service})
            if ([text(path) rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location !=
                NSNotFound)
                throw std::invalid_argument(
                    "Background session paths cannot contain control characters");
        (void)definition();
    }
    SessionJob inspect() override {
        AudioMaintenanceLease maintenance(paths_.engine == "/Applications/LoopZ Engine.app");
        const auto result = sessionCommand({"/bin/launchctl", "print", target()});
        if (result.exitCode != 0) {
            if ((result.exitCode == 113 || result.exitCode == 3) &&
                result.output.find("Could not find service") != std::string::npos)
                return SessionJob::Missing;
            throw std::runtime_error("LoopZ could not inspect its background session.");
        }
        // launchctl has no structured print format. Compare complete fields,
        // not substring labels/PIDs, and fail closed if its format changes.
        NSString* output = text(result.output);
        NSMutableArray* lines = [NSMutableArray array];
        for (NSString* line in [output componentsSeparatedByString:@"\n"])
            [lines addObject:[line stringByTrimmingCharactersInSet:NSCharacterSet
                                                                       .whitespaceCharacterSet]];
        if (![lines containsObject:[@"path = " stringByAppendingString:text((paths_.directory /
                                                                             "engine.plist")
                                                                                .string())]] ||
            ![lines
                containsObject:[@"program = " stringByAppendingString:text(executable().string())]])
            return SessionJob::Foreign;
        NSUInteger first = [lines indexOfObject:@"arguments = {"];
        auto* expected = arguments();
        if (first == NSNotFound || first + expected.count + 1 >= lines.count)
            return SessionJob::Foreign;
        for (NSUInteger index = 0; index < expected.count; ++index)
            if (![lines[first + index + 1] isEqual:expected[index]])
                return SessionJob::Foreign;
        if (![lines[first + expected.count + 1] isEqual:@"}"])
            return SessionJob::Foreign;
        bool keepAlive = false;
        for (NSString* line in lines)
            if ([line hasPrefix:@"properties = "] &&
                [[[line substringFromIndex:13] componentsSeparatedByString:@" | "]
                    containsObject:@"keepalive"])
                keepAlive = true;
        if (!keepAlive)
            return SessionJob::Foreign;
        const File directory(
            open(paths_.directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        privateFile(directory, true);
        validateDefinition(directory);
        for (NSString* line in lines)
            if ([line hasPrefix:@"pid = "]) {
                const auto pid = [[line substringFromIndex:6] intValue];
                if (pid <= 0 || !processIdentity(pid, executable(), pin_))
                    return SessionJob::Foreign;
            }
        return SessionJob::Managed;
    }
    void validateEngine() override {
        AudioMaintenanceLease maintenance(paths_.engine == "/Applications/LoopZ Engine.app");
        staticIdentity(paths_.engine, pin_);
        const auto result = sessionCommand({executable().string(), "--runtime-capabilities"});
        NSData* data = [text(result.output) dataUsingEncoding:NSUTF8StringEncoding];
        id capabilities = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
        if (result.exitCode || ![capabilities isKindOfClass:NSDictionary.class] ||
            ![capabilities[@"backgroundSession"] isEqual:@1])
            throw std::runtime_error("Update LoopZ Engine to enable background routing.");
    }
    void launch() override {
        AudioMaintenanceLease maintenance(paths_.engine == "/Applications/LoopZ Engine.app");
        // The session file is deliberately outside Library/LaunchAgents.
        // Login registration is a separate, explicitly chosen SMAppService.
        auto parent = privateDirectory(paths_.directory.parent_path());
        (void)parent;
        auto directory = privateDirectory(paths_.directory);
        File lock(
            openat(directory, "session.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600));
        privateFile(lock, false);
        if (flock(lock, LOCK_EX | LOCK_NB))
            throw std::runtime_error("Another LoopZ window is starting the audio engine.");
        const auto current = inspect();
        if (current == SessionJob::Managed)
            return;
        if (current != SessionJob::Missing)
            throw std::runtime_error("Another engine occupies LoopZ’s background session.");
        const auto data =
            [NSPropertyListSerialization dataWithPropertyList:definition()
                                                       format:NSPropertyListXMLFormat_v1_0
                                                      options:0
                                                        error:nil];
        if (!data)
            throw std::runtime_error("The background job could not be prepared.");
        struct stat existing{};
        if (fstatat(directory, "engine.plist", &existing, AT_SYMLINK_NOFOLLOW) && errno == ENOENT) {
            TemporaryJob temporary{
                directory, "engine-" + std::string(NSUUID.UUID.UUIDString.UTF8String) + ".tmp"};
            File file(openat(directory, temporary.name.c_str(),
                             O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
            size_t done = 0;
            while (done < data.length) {
                const auto count =
                    write(file, static_cast<const char*>(data.bytes) + done, data.length - done);
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0) {
                    throw std::runtime_error("The background job could not be saved.");
                }
                done += static_cast<size_t>(count);
            }
            if (fsync(file))
                throw std::runtime_error("The background job could not be synchronized.");
            if (linkat(directory, temporary.name.c_str(), directory, "engine.plist", 0) &&
                errno != EEXIST)
                throw std::runtime_error("The background job could not be published.");
            // Drop the temporary hard link before validating the single-link
            // published file. The directory lock serializes our publishers.
            unlinkat(directory, temporary.name.c_str(), 0);
            if (fsync(directory))
                throw std::runtime_error("The background job could not be synchronized.");
        }
        validateDefinition(directory);
        const auto result = sessionCommand(
            {"/bin/launchctl", "bootstrap", domain_, (paths_.directory / "engine.plist").string()});
        if (result.exitCode && inspect() != SessionJob::Managed)
            throw std::runtime_error("The audio engine is still starting; LoopZ will retry.");
    }
};
} // namespace
std::unique_ptr<SessionHost> makeSessionHost(SessionPaths paths, const std::string& requirement) {
    return std::make_unique<Host>(std::move(paths), requirement);
}
std::unique_ptr<SessionHost> productionSessionHost() {
    struct passwd* user = getpwuid(getuid());
    if (!user || !user->pw_dir)
        throw std::runtime_error("The current user’s home directory is unavailable.");
    const auto support = std::filesystem::path(user->pw_dir) / "Library/Application Support/LoopZ";
    return makeSessionHost({"/Applications/LoopZ Engine.app", support / "routes.json",
                            support / "Session", "com.lexasoft.loopz.session",
                            "com.lexasoft.loopz.editor.control"},
                           "identifier \"com.lexasoft.loopz.engine\" and anchor apple generic and "
                           "certificate 1[field.1.2.840.113635.100.6.2.6] exists and certificate "
                           "leaf[field.1.2.840.113635.100.6.1.13] exists and certificate "
                           "leaf[subject.OU] = \"USJ7H3X44X\"");
}
} // namespace loopz::macos
