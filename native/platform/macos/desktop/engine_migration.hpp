#pragma once

#include "engine_location.hpp"
#import <Foundation/Foundation.h>
#include <stdexcept>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>

namespace loopz::macos {
inline bool applicationCaptureDocument(NSDictionary* document) {
    for (NSDictionary* device in document[@"devices"])
        for (NSDictionary* binding in device[@"bindings"])
            if ([binding[@"kind"] isEqual:@"application"])
                return true;
    return false;
}
inline NSDictionary* checkedAccountEngineJob(const std::filesystem::path& path) {
    if (!std::filesystem::exists(std::filesystem::symlink_status(path)))
        return nil;
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        throw std::runtime_error("The account's Engine job could not be read safely.");
    struct Descriptor {
        int value;
        ~Descriptor() {
            close(value);
        }
    } descriptor{fd};
    struct stat info{};
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_size < 1 ||
        info.st_size > 65536)
        throw std::runtime_error("The account's Engine job is invalid.");
    NSMutableData* data = [NSMutableData dataWithLength:static_cast<NSUInteger>(info.st_size)];
    size_t done = 0;
    while (done < data.length) {
        const auto count =
            read(fd, static_cast<char*>(data.mutableBytes) + done, data.length - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            throw std::runtime_error("The account's Engine job changed.");
        done += static_cast<size_t>(count);
    }
    id job = [NSPropertyListSerialization propertyListWithData:data options:0 format:nil error:nil];
    if (![job isKindOfClass:NSDictionary.class])
        throw std::runtime_error("The account's Engine job is invalid.");
    return job;
}
// Path selection is advisory during app launch. A transient or malformed job
// must reach the host's ordinary validation/status flow, never escape a factory.
inline NSDictionary* accountEngineJob(const std::filesystem::path& path,
                                      bool* invalid = nullptr) noexcept {
    if (invalid)
        *invalid = false;
    try {
        return checkedAccountEngineJob(path);
    } catch (...) {
        if (invalid)
            *invalid = true;
        return nil;
    }
}
inline bool accountJobUses(NSDictionary* job, const std::filesystem::path& bundle) {
    NSArray* args = job[@"ProgramArguments"];
    return [args isKindOfClass:NSArray.class] && args.count &&
           [args[0] isEqual:@((bundle / "Contents/MacOS/LoopZ Engine").c_str())];
}
inline bool accountUsesLegacy(NSDictionary* job,
                              const std::filesystem::path& legacy = legacyEnginePath) {
    return job ? accountJobUses(job, legacy) : legacyEnginePresent(legacy);
}
inline bool accountMigrationPending(NSDictionary* document, NSDictionary* job,
                                    const std::filesystem::path& legacy = legacyEnginePath,
                                    bool enabled = kMigrateApplicationCaptureGraphs) {
    return engineMigrationPending(applicationCaptureDocument(document),
                                  accountUsesLegacy(job, legacy), enabled);
}
inline std::filesystem::path
sessionEnginePath(NSDictionary* document, const std::filesystem::path& preferred = newEnginePath,
                  const std::filesystem::path& legacy = legacyEnginePath,
                  bool enabled = kMigrateApplicationCaptureGraphs, NSDictionary* job = nil) {
    if (accountJobUses(job, preferred))
        return preferred;
    if (accountMigrationPending(document, job, legacy, enabled))
        return legacy;
    return readEnginePath(preferred, legacy);
}
// This checks microphone consent only. System-audio consent has no supported
// read-only query here and is guarded by kMigrateApplicationCaptureGraphs.
struct EngineConsentLost : std::runtime_error {
    using std::runtime_error::runtime_error;
};
inline void requireMigrationConsent(NSDictionary* before, NSDictionary* after) {
    NSString* previous = before[@"microphone"];
    NSString* current = after[@"microphone"];
    if ([previous isEqual:@"authorized"] && ![current isEqual:@"authorized"])
        throw EngineConsentLost("Engine consent changed during audio maintenance. The previous "
                                "installation was restored.");
}
inline void requireApplicationCaptureConsent(NSDictionary* state) {
    for (NSDictionary* source in state[@"runtime"][@"sources"]) {
        if (![source[@"kind"] isEqual:@"application"])
            continue;
        NSString* error = [source[@"error"] isKindOfClass:NSString.class]
                              ? [source[@"error"] lowercaseString]
                              : @"";
        if ([error containsString:@"permission"] || [error containsString:@"not-determined"] ||
            [error containsString:@"denied"])
            throw EngineConsentLost("Application capture permission could not be restored. The "
                                    "previous installation was restored.");
    }
}
inline bool applicationCaptureRestored(NSDictionary* state) {
    bool observed = false;
    for (NSDictionary* source in state[@"runtime"][@"sources"])
        if ([source[@"kind"] isEqual:@"application"]) {
            observed = true;
            if ([source[@"demanded"] isEqual:@YES] &&
                (![source[@"connected"] isEqual:@YES] || ![source[@"complete"] isEqual:@YES]))
                return false;
        }
    return observed;
}
} // namespace loopz::macos
