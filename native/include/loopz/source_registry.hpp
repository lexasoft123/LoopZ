#pragma once

#include <loopz/audio_scene.hpp>
#include <loopz/input_timing.hpp>
#include <loopz/audio_history.hpp>
#include <loopz/audio_delivery_timing.hpp>

namespace loopz {

// Backend adapter. Only read() and immutable channels() run on the audio thread.
// open() prepares/starts a source but registry admission gates its public reads;
// close() joins its producer callbacks before returning and does not free the
// source's reader-visible history. A destructor calls close() on the control
// domain. No endpoint may request protected permission during recovery.
struct SourceOpenPolicy {
    uint32_t requiredChannels;
    bool muteOriginal;
};
struct SourceDiagnostics {
    AudioHistoryReadSnapshot history;
    // Callback-owned delivery data is available only after close joins the
    // producer. Live history counters are independent atomic observations.
    std::optional<AudioDeliverySnapshot> stoppedDelivery;
};
class SourceEndpoint : public AudioSource {
  public:
    virtual bool open(SourceOpenPolicy policy, std::string& error) = 0;
    virtual void close() noexcept = 0;
    virtual bool healthy(std::string& error) = 0;
    // Control-domain event notification bypasses any backend health cache.
    virtual void invalidateHealth() noexcept {}
    // Control only. Empty means timing is not verified. A summed capture must
    // report every contributing stream, or return empty for the whole source.
    virtual std::vector<InputTimingFacts> inputTiming() const {
        return {};
    }
    // Previously verified delivery for a stopped producer. Used only while no
    // audio can arrive. A reopened producer must validate its actual format
    // before inputTiming() may reuse this history.
    virtual std::vector<InputTimingFacts> retainedInputTiming() const {
        return {};
    }
    virtual void clearInputTiming() noexcept {}
    // Control only, opt-in. The default path has no diagnostic allocation.
    virtual std::shared_ptr<const SourceDiagnostics> diagnostics() const {
        return {};
    }
    // Control owner only. A prepared provider may retain its current capture
    // when demand changes within the same logical layout. Update completeness
    // for the new policy, including partial profiles. A false result asks
    // the registry to retire admission and close/reopen normally. Backends
    // should restore a partially applied policy before returning false.
    virtual bool updatePolicy(SourceOpenPolicy) noexcept {
        return false;
    }
    virtual bool complete() const noexcept {
        return true;
    }
};
struct SourceConnectionStatus {
    SourceKind kind;
    std::string selector;
    uint32_t channels, requiredChannels;
    bool demanded, connected, complete, muteOriginal;
    uint64_t attempts;
    std::string error;
    // Lifetime of this shared provider, across prepared scenes and reconnects.
    // Counts unavailable frames per read; concurrent logical readers each count.
    uint64_t missingReadFrames{0};
    std::shared_ptr<const SourceDiagnostics> diagnostics{};
};

// One control owner. Shared providers are keyed by source kind/selector;
// logical routes keep independent clock cursors. Plans and resolvers allocate
// before native submission and never acquire hardware. poll() acts only on a
// committed scene's active graph, so abandoned edits cannot mute another app.
class SourceRegistry {
  public:
    using Factory =
        std::function<std::shared_ptr<SourceEndpoint>(SourceKind, const std::string&, uint32_t)>;
    using Clock = std::function<uint64_t()>;
    // Optional control clock refreshes retry/deadline time after blocking
    // backend calls. Each provider receives its own acquisition interval.
    explicit SourceRegistry(Factory factory, Clock controlClock = {});
    ~SourceRegistry();
    SourceRegistry(const SourceRegistry&) = delete;
    SourceRegistry& operator=(const SourceRegistry&) = delete;

    // Compute the largest required channel layout for each shared endpoint
    // before resolving any one binding, independent of document source order.
    void prepare(const RouteDocument& document);
    ResolvedAudioSource resolve(const VirtualDeviceDefinition& device, const SourceBinding& binding,
                                const SourceRouting& routing);
    // Supplemental bits refer only to devices in this committed document. They
    // preserve existing captures when a native client has moved to a staged
    // mapping; sources introduced by an uncommitted edit are never acquired.
    // Preview demand acquires metering without muting original app playback.
    void poll(const AudioScene* committed, uint64_t nowMilliseconds,
              uint32_t supplementalDemand = 0, uint32_t previewDemand = 0);
    void retryNow(uint64_t nowMilliseconds) noexcept;
    void stop() noexcept;
    bool restored() const noexcept;
    std::vector<SourceConnectionStatus> status() const;
    std::vector<InputTimingFacts> inputTiming(SourceKind kind, const std::string& selector) const;
    // Mark one provider for the normal close/backoff/reacquisition path on the
    // next control poll. Healthy peers and saved bindings remain untouched.
    void rejectInputTiming(SourceKind kind, const std::string& selector);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz
