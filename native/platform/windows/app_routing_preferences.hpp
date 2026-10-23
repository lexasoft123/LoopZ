#pragma once
#include "app_audio_policy.hpp"
#include "app_routing_process.hpp"
#include <loopz/app_routing_lease.hpp>

namespace loopz::windows {
// MTA control thread only. The caller must authenticate the intended virtual
// target before lease.apply(), serialize ownership and separately verify stream
// movement. Every private policy call checks the pinned process before/after it.
class ProcessRoutingPreferences final : public AppRoutingPreferences {
  public:
    ProcessRoutingPreferences(AppAudioPolicy& policy, uint32_t pid,
                              const std::filesystem::path& selectedImage,
                              std::string_view expectedApplication = {});
    const RoutingProcessIdentity& processIdentity() const noexcept;
    std::string application() const override;
    std::array<std::string, 3> read() override;
    void write(unsigned role, std::string_view value) override;

  private:
    AppAudioPolicy& policy_;
    AppRoutingProcess process_;
};
} // namespace loopz::windows
