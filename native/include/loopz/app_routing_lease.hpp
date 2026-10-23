#pragma once
#include <loopz/app_routing_journal.hpp>
#include <array>
#include <string>
#include <string_view>

namespace loopz {
// Platform-authenticated application access. The implementation must pin and
// revalidate the process identity for every read/write, validate virtual targets
// before apply(), and use exact UTF-8 encodings of opaque preference values.
// One serialized owner must control each application and its journal receipts.
// This protocol verifies preferences; the platform separately verifies streams.
class AppRoutingPreferences {
  public:
    virtual ~AppRoutingPreferences() = default;
    virtual std::string application() const = 0;
    virtual std::array<std::string, 3> read() = 0;
    virtual void write(unsigned role, std::string_view value) = 0;
};
enum class AppRoutingApplyResult { Applied, UserOverride };
enum class AppRoutingReleaseResult { NoReceipt, Restored, PreservedUserChange };
enum class AppRoutingReleaseMode { ForgetVerified, RetainVerified };
enum class AppRoutingOverrideMode { ForgetVerified, RetainUntilDesiredStateSaved };

class AppRoutingLease final {
  public:
    explicit AppRoutingLease(AppRoutingJournal& journal) : journal_(journal) {}
    // Durable prepare precedes EVERY setter, including same-target reapply after
    // a process relaunch. Any failed/ambiguous call leaves its receipt intact.
    // Confirmed leases relinquish on unowned preferences, including the baseline.
    // Prepared attempts may resume from their baseline after an interrupted setter. The caller must latch UserOverride until a new user request;
    // blindly reacquiring on the next inventory poll would defeat that choice.
    AppRoutingApplyResult apply(AppRoutingPreferences&, std::string_view target,
                                AppRoutingOverrideMode = AppRoutingOverrideMode::ForgetVerified);
    // Explicitly latch an observed external choice before restoring other roles.
    // This operation never applies a target, even if preferences change again.
    AppRoutingReleaseResult
    releaseOverride(AppRoutingPreferences&,
                    AppRoutingReleaseMode = AppRoutingReleaseMode::RetainVerified);
    // Compare current ownership before each setter and verify all roles before
    // CAS-forgetting the receipt. No destructor silently releases a lease.
    // An OS without an atomic compare/set cannot exclude a simultaneous user
    // change between a final read and setter; the adapter must report that limit.
    // RetainVerified leaves the durable receipt after verifying restoration. Use
    // it while saving a user override to desired state; a crash then preserves
    // the evidence that prevents automatic reacquisition on the next launch.
    AppRoutingReleaseResult release(AppRoutingPreferences&,
                                    AppRoutingReleaseMode = AppRoutingReleaseMode::ForgetVerified);

  private:
    AppRoutingReleaseResult releaseFor(AppRoutingPreferences&, AppRoutingReleaseMode,
                                       AppRoutingReceiptPhase);
    AppRoutingJournal& journal_;
};
} // namespace loopz
