#pragma once
#include <loopz/document_json.hpp>
#include <loopz/app_routing_journal.hpp>
#include <filesystem>
#include <memory>

namespace loopz::windows {
std::filesystem::path userDirectory();
std::string utf8(std::wstring_view value);
std::wstring wide(std::string_view value);
class ProcessRoutingPreferences;
class AppAudioPolicy;
class Runtime final {
  public:
    explicit Runtime(std::filesystem::path storage, bool manageMicrophones = false);
    ~Runtime();
    JSON request(const JSON& request);
    // Serialized MTA control owner. Audio retry plus throttled routing recovery.
    void recoverAudio();
    // Serialized control owner. Called after deferred lease restoration. Saves
    // the external output choice for every matching binding before a caller may
    // acknowledge/forget its receipt. No preference setter or broker elevation.
    bool acknowledgeApplicationOutputOverride(ProcessRoutingPreferences&);
    // Same recovery pass used by the owner loop, with explicit storage/policy
    // for isolated native acceptance. Never acquires new application routes.
    // endpointOccupants includes inactive/shared/unrelated sessions and must be
    // checked independently before releasing the account-wide sink.
    JSON recoverApplicationOutputs(AppRoutingJournal&, AppAudioPolicy&);
    // Same serialized owner as recovery. Acquires only the authenticated account
    // routing sink after complete capture preparation. Native acceptance may
    // supply isolated durable storage; no JSON request exposes policy setters.
    JSON acquireApplicationOutputs(AppRoutingJournal&, AppAudioPolicy&);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz::windows
