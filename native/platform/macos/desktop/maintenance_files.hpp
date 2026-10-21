#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <sys/types.h>

namespace loopz::macos {
struct AudioInstallEntry {
    std::filesystem::path source, target;
};
// Only the privileged helper supplies production paths. Journals contain entry
// indices and digests, never caller-selected destinations or commands.
class AudioInstallTransaction {
  public:
    AudioInstallTransaction(std::filesystem::path directory,
                            std::vector<AudioInstallEntry> entries);
    void prepare();
    void promote();
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
};
std::string audioTreeDigest(const std::filesystem::path& path);
void protectAudioTree(const std::filesystem::path& path, uid_t owner, gid_t group);
void syncAudioTree(const std::filesystem::path& path);
} // namespace loopz::macos
