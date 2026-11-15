#pragma once

#include <filesystem>
#include <string>
#include <functional>
#include <vector>
#include <sys/types.h>

namespace loopz::macos {
struct AudioInstallEntry {
    // An empty source retires the target only after verification, via retire().
    std::filesystem::path source, target;
    // Optional caller-owned directory descriptor for publishing into a user home.
    int targetDirectory{-1};
};
// Only the privileged helper supplies production paths. Journals contain entry
// indices and digests, never caller-selected destinations or commands.
class AudioInstallTransaction {
  public:
    AudioInstallTransaction(std::filesystem::path directory,
                            std::vector<AudioInstallEntry> entries);
    void prepare(const std::function<void(const std::filesystem::path&)>& verifyStage = {});
    void promote();
    void retire();
    void rollback();
    void commit();
    bool recover();
    const std::filesystem::path& workspace() const noexcept;

  private:
    std::filesystem::path directory_, work_;
    std::vector<AudioInstallEntry> entries_;
    std::vector<std::string> oldHashes_, newHashes_;
    size_t attempted_{0};
    void save(const char* phase);
    void clean();
    void moveFromTarget(size_t index, const std::filesystem::path& backup);
    void moveToTarget(const std::filesystem::path& source, size_t index);
    void removeTarget(size_t index);
};
std::string audioTreeDigest(const std::filesystem::path& path);
void protectAudioTree(const std::filesystem::path& path, uid_t owner, gid_t group);
void syncAudioTree(const std::filesystem::path& path);
} // namespace loopz::macos
