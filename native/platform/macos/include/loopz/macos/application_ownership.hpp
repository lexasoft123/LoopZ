#pragma once

#include <loopz/application_processes.hpp>
#include <memory>
#include <optional>

namespace loopz::macos {
// Capture-free control snapshot. libproc/AppKit and optional system SPI stay
// here, outside the portable ownership policy and every audio callback.
class ApplicationOwnership {
  public:
    explicit ApplicationOwnership(const std::string& application);
    ~ApplicationOwnership();
    const std::vector<ApplicationProcessRoot>& roots() const noexcept;
    std::optional<AudioProcessAttribution> inspect(uint64_t object, int32_t pid,
                                                   std::string application);
    bool coherent(std::span<const AudioProcessAttribution> selected) const;
    static bool responsibilityAvailable() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz::macos
