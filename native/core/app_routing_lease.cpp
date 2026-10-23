#include <loopz/app_routing_lease.hpp>
#include <algorithm>
#include <optional>
#include <stdexcept>

namespace loopz {
namespace {
std::optional<AppRoutingReceipt> receiptFor(AppRoutingJournal& journal, std::string_view app) {
    const auto records = journal.snapshot();
    const auto found = std::find_if(records.begin(), records.end(), [&](const auto& item) {
        return item.application == app;
    });
    if (found == records.end())
        return {};
    return *found;
}
void requireCurrent(AppRoutingJournal& journal, const AppRoutingReceipt& receipt) {
    const auto current = receiptFor(journal, receipt.application);
    if (!current || current->revision != receipt.revision)
        throw std::runtime_error("App routing ownership changed during operation");
}
bool owns(const AppRoutingReceipt& receipt, std::string_view value) {
    return std::find(receipt.attemptedTargets.begin(), receipt.attemptedTargets.end(), value) !=
           receipt.attemptedTargets.end();
}
void validateField(std::string_view value) {
    if (value.empty() || value.size() > 32768 || value.find('\0') != std::string_view::npos)
        throw std::invalid_argument("Invalid app routing lease field");
}
void requireIdentity(AppRoutingPreferences& preferences, std::string_view expected) {
    if (preferences.application() != expected)
        throw std::runtime_error("App routing identity changed during operation");
}
} // namespace
AppRoutingApplyResult AppRoutingLease::apply(AppRoutingPreferences& preferences,
                                             std::string_view target,
                                             AppRoutingOverrideMode overrideMode) {
    if (overrideMode != AppRoutingOverrideMode::ForgetVerified &&
        overrideMode != AppRoutingOverrideMode::RetainUntilDesiredStateSaved)
        throw std::invalid_argument("Invalid app routing override acknowledgment mode");
    const auto releaseMode = overrideMode == AppRoutingOverrideMode::RetainUntilDesiredStateSaved
                                 ? AppRoutingReleaseMode::RetainVerified
                                 : AppRoutingReleaseMode::ForgetVerified;
    validateField(target);
    const std::string destination(target);
    const auto application = preferences.application();
    validateField(application);
    auto previous = receiptFor(journal_, application);
    if (previous && previous->phase == AppRoutingReceiptPhase::UserOverride) {
        releaseFor(preferences, releaseMode, AppRoutingReceiptPhase::UserOverride);
        return AppRoutingApplyResult::UserOverride;
    }
    if (previous && previous->phase == AppRoutingReceiptPhase::Releasing) {
        release(preferences);
        previous.reset();
    }
    const auto current = preferences.read();
    requireIdentity(preferences, application);
    if (previous && [&] {
            for (size_t role = 0; role < current.size(); ++role)
                if (!owns(*previous, current[role]) &&
                    !(previous->phase == AppRoutingReceiptPhase::Prepared &&
                      current[role] == previous->original[role]))
                    return true;
            return false;
        }()) {
        releaseFor(preferences, releaseMode, AppRoutingReceiptPhase::UserOverride);
        return AppRoutingApplyResult::UserOverride;
    }
    const auto receipt =
        journal_.prepare(application, previous ? previous->original : current, destination);
    for (unsigned role = 0; role < 3; ++role) {
        requireIdentity(preferences, application);
        requireCurrent(journal_, receipt);
        const auto observed = preferences.read();
        requireIdentity(preferences, application);
        requireCurrent(journal_, receipt);
        // The first application is allowed to replace its captured baseline.
        // A change to another value after prepare is external, not our target.
        if (observed[role] != current[role] && !owns(receipt, observed[role])) {
            releaseFor(preferences, releaseMode, AppRoutingReceiptPhase::UserOverride);
            return AppRoutingApplyResult::UserOverride;
        }
        preferences.write(role, destination);
    }
    requireIdentity(preferences, application);
    const auto verified = preferences.read();
    requireIdentity(preferences, application);
    if (std::any_of(verified.begin(), verified.end(), [&](const auto& value) {
            return value != destination;
        }))
        throw std::runtime_error("App routing target verification failed; receipt retained");
    if (!journal_.confirm(application, receipt.revision, destination))
        throw std::runtime_error("App routing confirmation is stale; receipt retained");
    return AppRoutingApplyResult::Applied;
}
AppRoutingReleaseResult AppRoutingLease::releaseOverride(AppRoutingPreferences& preferences,
                                                         AppRoutingReleaseMode mode) {
    return releaseFor(preferences, mode, AppRoutingReceiptPhase::UserOverride);
}
AppRoutingReleaseResult AppRoutingLease::release(AppRoutingPreferences& preferences,
                                                 AppRoutingReleaseMode mode) {
    return releaseFor(preferences, mode, AppRoutingReceiptPhase::Releasing);
}
AppRoutingReleaseResult AppRoutingLease::releaseFor(AppRoutingPreferences& preferences,
                                                    AppRoutingReleaseMode mode,
                                                    AppRoutingReceiptPhase phase) {
    if (mode != AppRoutingReleaseMode::ForgetVerified &&
        mode != AppRoutingReleaseMode::RetainVerified)
        throw std::invalid_argument("Invalid app routing release acknowledgment mode");
    const auto application = preferences.application();
    auto receipt = receiptFor(journal_, application);
    if (!receipt)
        return AppRoutingReleaseResult::NoReceipt;
    // Retain an override latch even when the caller resumes ordinary release.
    if (receipt->phase == AppRoutingReceiptPhase::UserOverride)
        phase = AppRoutingReceiptPhase::UserOverride;
    receipt = journal_.beginRelease(application, receipt->revision, phase);
    if (!receipt)
        throw std::runtime_error("App routing release is stale");
    bool preserved = false;
    for (unsigned role = 0; role < 3; ++role) {
        requireIdentity(preferences, application);
        requireCurrent(journal_, *receipt);
        const auto current = preferences.read();
        requireIdentity(preferences, application);
        requireCurrent(journal_, *receipt);
        const auto action = appRoutingRestoration(*receipt, current)[role];
        if (action == AppRoutingRestore::PreserveUserChange)
            preserved = true;
        else if (action == AppRoutingRestore::RestoreOriginal)
            preferences.write(role, receipt->original[role]);
    }
    requireIdentity(preferences, application);
    const auto final = preferences.read();
    requireIdentity(preferences, application);
    const auto actions = appRoutingRestoration(*receipt, final);
    if (std::find(actions.begin(), actions.end(), AppRoutingRestore::RestoreOriginal) !=
        actions.end())
        throw std::runtime_error("App routing restoration verification failed; receipt retained");
    preserved = preserved || std::find(actions.begin(), actions.end(),
                                       AppRoutingRestore::PreserveUserChange) != actions.end();
    requireCurrent(journal_, *receipt);
    if (mode == AppRoutingReleaseMode::ForgetVerified &&
        !journal_.forget(application, receipt->revision))
        throw std::runtime_error("App routing restoration acknowledgment is stale");
    return preserved ? AppRoutingReleaseResult::PreservedUserChange
                     : AppRoutingReleaseResult::Restored;
}
} // namespace loopz
