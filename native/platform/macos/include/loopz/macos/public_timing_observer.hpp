#pragma once

#include <loopz/macos/public_timing.hpp>

namespace loopz::macos {
// One control owner, one read-only native batch in flight. Workers own their
// catalog and shared completion state; neither shutdown nor snapshots join a
// potentially blocked native call. No audio callback accesses this object.
class PublicTimingObserver {
  public:
    explicit PublicTimingObserver(PublicTimingCatalog::Operations operations);
    ~PublicTimingObserver();
    PublicTimingObserver(const PublicTimingObserver&) = delete;
    PublicTimingObserver& operator=(const PublicTimingObserver&) = delete;
    std::shared_ptr<const PublicTimingObservationBatch> snapshot(std::span<const std::string> uids,
                                                                 uint64_t nowNs);
    std::shared_ptr<const PublicTimingObservationBatch> latest() const noexcept;
    void invalidate() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz::macos
