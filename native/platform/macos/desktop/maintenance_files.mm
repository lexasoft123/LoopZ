#include "maintenance_files.hpp"
#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <dirent.h>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm>
#include <stdexcept>

namespace loopz::macos {
namespace {
namespace fs = std::filesystem;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
NSString* text(const fs::path& path) {
    return @(path.c_str());
}
void syncFile(const fs::path& path, bool directory) {
    const int fd =
        open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | (directory ? O_DIRECTORY : 0));
    require(fd >= 0, "Audio maintenance could not open a prepared file.");
    const int result = fsync(fd);
    close(fd);
    require(!result, "Audio maintenance could not save its recovery files.");
}
bool present(const fs::path& path) {
    std::error_code error;
    auto status = fs::symlink_status(path, error);
    require(!error || error == std::errc::no_such_file_or_directory,
            "Audio component could not be inspected.");
    return status.type() != fs::file_type::not_found;
}
NSString* hashText(const std::string& hash) {
    return @(hash.c_str());
}
std::string readHash(id value) {
    require([value isKindOfClass:NSString.class], "Invalid maintenance recovery digest.");
    std::string result([value UTF8String]);
    require(result.empty() || (result.size() == 64 &&
                               result.find_first_not_of("0123456789abcdef") == std::string::npos),
            "Invalid maintenance recovery digest.");
    return result;
}
} // namespace

std::string audioTreeDigest(const fs::path& path) {
    if (!present(path))
        return {};
    std::vector<fs::path> paths{path};
    if (fs::is_directory(fs::symlink_status(path)))
        for (const auto& item : fs::recursive_directory_iterator(path)) {
            require(paths.size() < 4096, "Audio component has too many files.");
            paths.push_back(item.path());
        }
    std::sort(paths.begin(), paths.end());
    NSMutableData* manifest = [NSMutableData data];
    uintmax_t total = 0;
    for (const auto& file : paths) {
        const auto status = fs::symlink_status(file);
        require(fs::is_regular_file(status) || fs::is_directory(status),
                "Linked or special audio component files are not supported.");
        struct stat info{};
        require(!lstat(file.c_str(), &info) && (S_ISDIR(info.st_mode) || info.st_nlink == 1),
                "Hard-linked audio files are not supported.");
        const auto relative =
            file == path ? std::string(".") : file.lexically_relative(path).string();
        [manifest appendBytes:relative.c_str() length:relative.size() + 1];
        const uint32_t mode = info.st_mode & 0777;
        [manifest appendBytes:&mode length:sizeof(mode)];
        if (fs::is_regular_file(status)) {
            const auto size = fs::file_size(file);
            total += size;
            require(total <= 128 * 1024 * 1024, "Audio component is too large.");
            const int fd = open(file.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
            require(fd >= 0, "Audio component could not be opened safely.");
            struct Descriptor {
                int fd;
                ~Descriptor() {
                    close(fd);
                }
            } descriptor{fd};
            struct stat opened{};
            require(!fstat(fd, &opened) && S_ISREG(opened.st_mode) && opened.st_nlink == 1 &&
                        opened.st_ino == info.st_ino && opened.st_dev == info.st_dev &&
                        opened.st_size >= 0 && static_cast<uintmax_t>(opened.st_size) == size,
                    "Audio component changed while being opened.");
            NSMutableData* data = [NSMutableData dataWithLength:static_cast<NSUInteger>(size)];
            size_t done = 0;
            while (done < size) {
                const auto count =
                    read(fd, static_cast<char*>(data.mutableBytes) + done, size - done);
                if (count < 0 && errno == EINTR)
                    continue;
                require(count > 0, "Audio component changed while being read.");
                done += static_cast<size_t>(count);
            }
            struct stat after{};
            require(!fstat(fd, &after) && after.st_size == opened.st_size &&
                        after.st_mtimespec.tv_sec == opened.st_mtimespec.tv_sec &&
                        after.st_mtimespec.tv_nsec == opened.st_mtimespec.tv_nsec &&
                        after.st_ctimespec.tv_sec == opened.st_ctimespec.tv_sec &&
                        after.st_ctimespec.tv_nsec == opened.st_ctimespec.tv_nsec,
                    "Audio component changed while being read.");
            unsigned char digest[CC_SHA256_DIGEST_LENGTH];
            CC_SHA256(data.bytes, static_cast<CC_LONG>(data.length), digest);
            [manifest appendBytes:digest length:sizeof(digest)];
        }
    }
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(manifest.bytes, static_cast<CC_LONG>(manifest.length), digest);
    std::string result;
    constexpr char digits[] = "0123456789abcdef";
    for (auto byte : digest) {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

void syncAudioTree(const fs::path& path) {
    if (fs::is_directory(path)) {
        std::vector<fs::path> directories{path};
        for (const auto& item : fs::recursive_directory_iterator(path))
            if (item.is_directory())
                directories.push_back(item.path());
            else
                syncFile(item.path(), false);
        for (auto i = directories.rbegin(); i != directories.rend(); ++i)
            syncFile(*i, true);
    } else
        syncFile(path, false);
    syncFile(path.parent_path(), true);
}

AudioInstallTransaction::AudioInstallTransaction(fs::path directory,
                                                 std::vector<AudioInstallEntry> entries)
    : directory_(std::move(directory)), entries_(std::move(entries)) {
    require(directory_.is_absolute() && !entries_.empty(), "Invalid audio maintenance plan.");
}
const fs::path& AudioInstallTransaction::workspace() const noexcept {
    return work_;
}
void AudioInstallTransaction::save(const char* phase) {
    NSMutableArray* old = [NSMutableArray array];
    NSMutableArray* fresh = [NSMutableArray array];
    for (const auto& hash : oldHashes_)
        [old addObject:hashText(hash)];
    for (const auto& hash : newHashes_)
        [fresh addObject:hashText(hash)];
    NSDictionary* record = @{
        @"version" : @1,
        @"workspace" : text(work_.filename()),
        @"phase" : @(phase),
        @"attempted" : @(attempted_),
        @"old" : old,
        @"new" : fresh
    };
    NSData* data = [NSPropertyListSerialization dataWithPropertyList:record
                                                              format:NSPropertyListBinaryFormat_v1_0
                                                             options:0
                                                               error:nil];
    const auto temporary =
        directory_ / (".maintenance-journal-" + std::string(NSUUID.UUID.UUIDString.UTF8String));
    const int fd =
        open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    require(fd >= 0, "Audio maintenance recovery journal could not be created.");
    const auto written = write(fd, data.bytes, data.length);
    const bool saved = written == static_cast<ssize_t>(data.length) && !fsync(fd);
    close(fd);
    if (!saved) {
        fs::remove(temporary);
        throw std::runtime_error("Audio recovery journal could not be saved.");
    }
    fs::rename(temporary, directory_ / "maintenance.plist");
    syncFile(directory_, true);
}
void AudioInstallTransaction::prepare() {
    require(!present(directory_ / "maintenance.plist"),
            "Recover the interrupted audio update first.");
    oldHashes_.clear();
    newHashes_.clear();
    attempted_ = 0;
    work_ = directory_ / (".maintenance-" + std::string(NSUUID.UUID.UUIDString.UTF8String));
    require(fs::create_directory(work_), "Audio maintenance staging is unavailable.");
    fs::permissions(work_, fs::perms::owner_all);
    fs::create_directory(work_ / "new");
    fs::create_directory(work_ / "old");
    try {
        for (size_t i = 0; i < entries_.size(); ++i) {
            oldHashes_.push_back(audioTreeDigest(entries_[i].target));
            newHashes_.push_back(audioTreeDigest(entries_[i].source));
            require(!newHashes_.back().empty(), "A bundled audio component is missing.");
            const auto target = work_ / "new" / std::to_string(i);
            fs::copy(entries_[i].source, target, fs::copy_options::recursive);
            require(audioTreeDigest(target) == newHashes_.back(), "Audio staging did not verify.");
            syncAudioTree(target);
        }
        save("prepared");
    } catch (...) {
        if (!present(directory_ / "maintenance.plist"))
            fs::remove_all(work_);
        throw;
    }
}
void AudioInstallTransaction::promote() {
    for (size_t i = 0; i < entries_.size(); ++i) {
        require(audioTreeDigest(entries_[i].target) == oldHashes_[i],
                "An installed audio component changed during maintenance.");
        if (oldHashes_[i] == newHashes_[i])
            continue;
        attempted_ = i + 1;
        save("changing"); // Durable before either rename, including the crash window between them.
        if (!oldHashes_[i].empty())
            fs::rename(entries_[i].target, work_ / "old" / std::to_string(i));
        fs::rename(work_ / "new" / std::to_string(i), entries_[i].target);
        syncFile(entries_[i].target.parent_path(), true);
        syncFile(work_ / "old", true);
    }
}
void AudioInstallTransaction::rollback() {
    for (size_t i = attempted_; i-- > 0;) {
        const auto backup = work_ / "old" / std::to_string(i);
        const auto current = audioTreeDigest(entries_[i].target);
        if (present(backup)) {
            require(audioTreeDigest(backup) == oldHashes_[i], "An audio recovery backup changed.");
            require(current.empty() || current == newHashes_[i],
                    "A changed audio component was preserved for manual review.");
            if (!current.empty())
                fs::remove_all(entries_[i].target);
            fs::rename(backup, entries_[i].target);
        } else if (oldHashes_[i].empty() && current == newHashes_[i]) {
            fs::remove_all(entries_[i].target);
        } else {
            require(current == oldHashes_[i], "An audio recovery component is unavailable.");
        }
        syncFile(entries_[i].target.parent_path(), true);
    }
    save("rolled-back");
    clean();
}
void AudioInstallTransaction::commit() {
    save("committed");
    clean();
}
void AudioInstallTransaction::clean() {
    fs::remove(directory_ / "maintenance.plist");
    syncFile(directory_, true);
    fs::remove_all(work_);
    syncFile(directory_, true);
}
bool AudioInstallTransaction::recover() {
    const auto journal = directory_ / "maintenance.plist";
    if (!present(journal))
        return false;
    require(fs::is_regular_file(fs::symlink_status(journal)) && fs::file_size(journal) <= 65536,
            "Invalid audio maintenance recovery journal.");
    struct stat journalInfo{};
    require(!lstat(journal.c_str(), &journalInfo) && journalInfo.st_uid == geteuid() &&
                journalInfo.st_nlink == 1 && !(journalInfo.st_mode & 0022),
            "Audio recovery journal ownership is invalid.");
    NSData* bytes = [NSData dataWithContentsOfFile:text(journal)];
    id record = [NSPropertyListSerialization propertyListWithData:bytes
                                                          options:0
                                                           format:nil
                                                            error:nil];
    require([record isKindOfClass:NSDictionary.class] && [record[@"version"] isEqual:@1] &&
                [record[@"old"] isKindOfClass:NSArray.class] &&
                [record[@"new"] isKindOfClass:NSArray.class] &&
                [record[@"old"] count] == entries_.size() &&
                [record[@"new"] count] == entries_.size(),
            "Invalid audio maintenance recovery journal.");
    id leaf = record[@"workspace"];
    require([leaf isKindOfClass:NSString.class] && [leaf hasPrefix:@".maintenance-"] &&
                [[NSUUID alloc] initWithUUIDString:[leaf substringFromIndex:13]] != nil,
            "Invalid audio maintenance recovery location.");
    work_ = directory_ / [leaf UTF8String];
    require(fs::is_directory(fs::symlink_status(work_)), "Audio recovery files are unavailable.");
    struct stat workspaceInfo{};
    require(!lstat(work_.c_str(), &workspaceInfo) && workspaceInfo.st_uid == geteuid() &&
                (workspaceInfo.st_mode & 0777) == 0700,
            "Audio recovery directory ownership is invalid.");
    id attempted = record[@"attempted"];
    require([attempted isKindOfClass:NSNumber.class] && [attempted doubleValue] >= 0 &&
                [attempted doubleValue] <= entries_.size() &&
                [attempted doubleValue] == [attempted unsignedLongLongValue],
            "Invalid audio maintenance recovery progress.");
    attempted_ = [attempted unsignedLongLongValue];
    require([@[ @"prepared", @"changing", @"rolled-back", @"committed" ]
                containsObject:record[@"phase"]],
            "Invalid audio maintenance recovery phase.");
    for (id hash in record[@"old"])
        oldHashes_.push_back(readHash(hash));
    for (id hash in record[@"new"])
        newHashes_.push_back(readHash(hash));
    if ([record[@"phase"] isEqual:@"committed"]) {
        for (size_t i = 0; i < entries_.size(); ++i)
            require(audioTreeDigest(entries_[i].target) == newHashes_[i],
                    "The completed audio update changed before recovery.");
        clean();
    } else
        rollback();
    return true;
}
void protectAudioTree(const fs::path& root, uid_t owner, gid_t group) {
    // Walk pinned descriptors so a user-owned Engine cannot redirect a root
    // ownership change through a replaced directory or symbolic link.
    size_t count = 0;
    const auto visit = [&](const auto& self, int descriptor) -> void {
        struct stat info{};
        require(++count <= 4096 && !fstat(descriptor, &info) &&
                    (S_ISREG(info.st_mode) || S_ISDIR(info.st_mode)) &&
                    (S_ISDIR(info.st_mode) || info.st_nlink == 1),
                "Invalid audio payload entry.");
        if (S_ISDIR(info.st_mode)) {
            DIR* directory = fdopendir(dup(descriptor));
            require(directory != nullptr, "Audio component directory could not be inspected.");
            struct Directory {
                DIR* value;
                ~Directory() {
                    closedir(value);
                }
            } listing{directory};
            while (true) {
                errno = 0;
                const auto* entry = readdir(directory);
                if (!entry) {
                    require(errno == 0, "Audio component directory could not be inspected.");
                    break;
                }
                if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, ".."))
                    continue;
                const int child = openat(descriptor, entry->d_name,
                                         O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
                require(child >= 0, "Invalid audio payload entry.");
                struct Child {
                    int value;
                    ~Child() {
                        close(value);
                    }
                } opened{child};
                self(self, child);
            }
        }
        require(
            !fchown(descriptor, owner, group) &&
                !fchmod(descriptor, S_ISDIR(info.st_mode) || (info.st_mode & 0111) ? 0755 : 0644),
            "Audio component ownership could not be set.");
    };
    const int descriptor = open(root.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    require(descriptor >= 0, "Audio component could not be opened.");
    struct Root {
        int value;
        ~Root() {
            close(value);
        }
    } opened{descriptor};
    visit(visit, descriptor);
}
} // namespace loopz::macos
