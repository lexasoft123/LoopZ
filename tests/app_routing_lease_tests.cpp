#include <loopz/app_routing_lease.hpp>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace loopz;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void rejected(F&& action, const char* message) {
    bool failed = false;
    try {
        action();
    } catch (const std::exception&) {
        failed = true;
    }
    require(failed, message);
}
const std::array<std::string, 3> original{"console", "", "communications"};
struct Preferences final : AppRoutingPreferences {
    AppRoutingJournal& journal;
    std::string identity{"owned-app"};
    std::array<std::string, 3> values{original};
    unsigned reads{}, writes{}, throwAfterWrite{}, failRead{};
    bool ignoreWrites{};
    std::vector<std::pair<unsigned, std::string>> mutations;
    std::function<void()> beforeRead, afterWrite;
    explicit Preferences(AppRoutingJournal& store) : journal(store) {}
    std::string application() const override {
        return identity;
    }
    std::array<std::string, 3> read() override {
        ++reads;
        if (beforeRead)
            beforeRead();
        if (reads == failRead)
            throw std::runtime_error("Injected read failure");
        return values;
    }
    void write(unsigned role, std::string_view value) override {
        const auto records = journal.snapshot();
        require(records.size() == 1 && records[0].application == identity &&
                    records[0].original == original,
                "OS mutation preceded durable original receipt");
        require(role < 3, "Invalid role");
        ++writes;
        mutations.emplace_back(role, value);
        if (!ignoreWrites)
            values[role] = value;
        if (afterWrite)
            afterWrite();
        if (writes == throwAfterWrite)
            throw std::runtime_error("Injected lost setter acknowledgment");
    }
};
struct Fixture {
    std::filesystem::path directory;
    unsigned next{};
    Fixture() {
        directory = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
                    ("loopz-lease-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(std::filesystem::create_directory(directory), "Create lease fixture");
    }
    ~Fixture() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    std::filesystem::path path() {
        return directory / (std::to_string(++next) + ".sqlite3");
    }
};
} // namespace
int main() {
    Fixture fixture;
    {
        AppRoutingJournal journal(fixture.path(), true);
        Preferences p(journal);
        AppRoutingLease lease(journal);
        lease.apply(p, "feed-A");
        p.values[0] = "user-output";
        require(lease.releaseOverride(p) == AppRoutingReleaseResult::PreservedUserChange &&
                    p.values == std::array<std::string, 3>{"user-output", "", "communications"} &&
                    journal.snapshot()[0].phase == AppRoutingReceiptPhase::UserOverride,
                "Explicit override restoration lost user choice or durable latch");
        const auto writes = p.writes;
        require(lease.apply(p, "feed-B", AppRoutingOverrideMode::RetainUntilDesiredStateSaved) ==
                        AppRoutingApplyResult::UserOverride &&
                    p.writes == writes,
                "Explicit override operation allowed automatic reacquisition");
    }

    {
        const auto path = fixture.path();
        {
            AppRoutingJournal journal(path, true);
            Preferences p(journal);
            AppRoutingLease lease(journal);
            p.throwAfterWrite = 1;
            rejected(
                [&] {
                    lease.apply(p, "feed-A");
                },
                "Lost setter accepted");
            require(journal.snapshot()[0].phase == AppRoutingReceiptPhase::Prepared,
                    "Interrupted apply lost prepared phase");
        }
        AppRoutingJournal journal(path);
        Preferences p(journal);
        p.values[0] = "feed-A";
        AppRoutingLease lease(journal);
        require(lease.apply(p, "feed-A") == AppRoutingApplyResult::Applied &&
                    journal.snapshot()[0].phase == AppRoutingReceiptPhase::Applied,
                "Interrupted apply was mistaken for user override");
        const auto applied = journal.snapshot()[0];
        const auto releasing =
            journal.beginRelease(p.identity, applied.revision, AppRoutingReceiptPhase::Releasing);
        require(releasing && releasing->revision > applied.revision &&
                    !journal.confirm(p.identity, applied.revision, "feed-A") &&
                    !journal.forget(p.identity, applied.revision),
                "Late apply acknowledgment crossed restoration");
        require(
            journal.beginRelease(p.identity, releasing->revision, AppRoutingReceiptPhase::Releasing)
                    ->revision == releasing->revision,
            "Repeated release changed revision");
        p.values[0] = original[0];
        require(lease.apply(p, "feed-B") == AppRoutingApplyResult::Applied &&
                    journal.snapshot()[0].original == original,
                "Interrupted restoration did not finish before reacquisition");
    }

    {
        AppRoutingJournal journal(fixture.path(), true);
        Preferences p(journal);
        AppRoutingLease lease(journal);
        require(lease.apply(p, "feed-A") == AppRoutingApplyResult::Applied, "Initial lease");
        require(p.writes == 3 && journal.snapshot()[0].original == original, "Initial ownership");
        require(lease.apply(p, "feed-A") == AppRoutingApplyResult::Applied && p.writes == 6,
                "Same-target relaunch reapply was skipped");
        require(lease.release(p) == AppRoutingReleaseResult::Restored && p.values == original,
                "Original roles were not restored");
        require(journal.snapshot().empty() &&
                    lease.release(p) == AppRoutingReleaseResult::NoReceipt,
                "Verified receipt not released");
    }
    {
        AppRoutingJournal journal(fixture.path(), true);
        Preferences p(journal);
        AppRoutingLease lease(journal);
        p.throwAfterWrite = 2;
        rejected(
            [&] {
                lease.apply(p, "feed-A");
            },
            "Lost setter acknowledgment accepted");
        require(journal.snapshot().size() == 1 && p.values[0] == "feed-A" &&
                    p.values[1] == "feed-A",
                "Partial application receipt lost");
        p.throwAfterWrite = 0;
        AppRoutingLease recovered(journal);
        require(recovered.release(p) == AppRoutingReleaseResult::Restored && p.values == original &&
                    journal.snapshot().empty(),
                "Partial application did not recover");
    }
    {
        AppRoutingJournal journal(fixture.path(), true);
        Preferences p(journal);
        AppRoutingLease lease(journal);
        p.failRead = 4;
        rejected(
            [&] {
                lease.apply(p, "feed-A");
            },
            "Read failure accepted");
        require(!journal.snapshot().empty(), "Read failure discarded receipt");
        p.failRead = 0;
        require(lease.release(p) == AppRoutingReleaseResult::Restored && p.values == original,
                "Read failure recovery");
    }
    {
        AppRoutingJournal journal(fixture.path(), true);
        Preferences p(journal);
        AppRoutingLease lease(journal);
        lease.apply(p, "feed-A");
        p.throwAfterWrite = p.writes + 1;
        rejected(
            [&] {
                lease.release(p);
            },
            "Ambiguous restore acknowledged");
        require(!journal.snapshot().empty() && p.values[0] == original[0], "Restore receipt lost");
        p.throwAfterWrite = 0;
        p.values[1] = "user-choice";
        const auto before = p.writes;
        require(lease.release(p) == AppRoutingReleaseResult::PreservedUserChange &&
                    p.writes == before + 1 &&
                    p.values == std::array<std::string, 3>{original[0], "user-choice", original[2]},
                "Recovery overwrote user change or repeated completed setter");
    }
    {
        AppRoutingJournal journal(fixture.path(), true);
        Preferences p(journal);
        AppRoutingLease lease(journal);
        lease.apply(p, "feed-A");
        p.values[0] = original[0];
        require(lease.apply(p, "feed-B") == AppRoutingApplyResult::UserOverride &&
                    p.values == original && journal.snapshot().empty(),
                "User return to original output was overwritten");
        for (const auto& mutation : p.mutations)
            require(mutation.second != "feed-B", "Override was retargeted");
    }
    {
        AppRoutingJournal journal(fixture.path(), true);
        Preferences p(journal);
        AppRoutingLease lease(journal);
        p.beforeRead = [&] {
            if (p.reads == 2)
                p.values[0] = "user-choice";
        };
        require(lease.apply(p, "feed-A") == AppRoutingApplyResult::UserOverride && p.writes == 0 &&
                    p.values[0] == "user-choice",
                "Change between prepare and setter overwritten");
        require(journal.snapshot().empty(), "Verified external override receipt retained");
    }
    {
        AppRoutingJournal journal(fixture.path(), true);
        Preferences p(journal);
        AppRoutingLease lease(journal);
        p.ignoreWrites = true;
        rejected(
            [&] {
                lease.apply(p, "feed-A");
            },
            "False successful setter accepted");
        require(!journal.snapshot().empty(), "Failed verification lost receipt");
        require(lease.release(p) == AppRoutingReleaseResult::Restored && journal.snapshot().empty(),
                "Unchanged baseline cleanup");
    }
    {
        AppRoutingJournal journal(fixture.path(), true);
        Preferences p(journal);
        AppRoutingLease lease(journal);
        lease.apply(p, "feed-A");
        lease.apply(p, "feed-B");
        require(journal.snapshot()[0].attemptedTargets == std::vector<std::string>{"feed-B"},
                "Confirmed target history not pruned");
        p.values[0] = "feed-A";
        require(lease.release(p) == AppRoutingReleaseResult::PreservedUserChange &&
                    p.values[0] == "feed-A",
                "User selection of older Feed overwritten");
    }
    {
        AppRoutingJournal journal(fixture.path(), true);
        Preferences p(journal);
        AppRoutingLease lease(journal);
        lease.apply(p, "feed-A");
        const auto old = journal.snapshot()[0].revision;
        p.afterWrite = [&] {
            journal.prepare(p.identity, original, "feed-B");
        };
        rejected(
            [&] {
                lease.release(p);
            },
            "Stale restoration erased a new lease");
        require(journal.snapshot().size() == 1 && journal.snapshot()[0].revision > old,
                "New receipt not retained");
    }
    {
        AppRoutingJournal journal(fixture.path(), true);
        Preferences p(journal);
        AppRoutingLease lease(journal);
        p.beforeRead = [&] {
            if (p.reads == 2)
                p.identity = "different-app";
        };
        rejected(
            [&] {
                lease.apply(p, "feed-A");
            },
            "Changed application identity accepted");
        require(p.writes == 0 && journal.snapshot()[0].application == "owned-app",
                "Changed identity mutated or lost receipt");
    }
    {
        AppRoutingJournal journal(fixture.path(), true);
        Preferences p(journal);
        AppRoutingLease lease(journal);
        lease.apply(p, "feed-A");
        const auto count = p.writes;
        rejected(
            [&] {
                lease.apply(p, "");
            },
            "Invalid target accepted");
        require(p.writes == count && !journal.snapshot().empty(),
                "Invalid target mutated ownership");
        p.ignoreWrites = true;
        rejected(
            [&] {
                lease.release(p);
            },
            "Unrestored settings acknowledged");
        require(!journal.snapshot().empty(), "Unrestored receipt discarded");
    }
    {
        AppRoutingJournal journal(fixture.path(), true);
        Preferences p(journal);
        AppRoutingLease lease(journal);
        p.beforeRead = [&] {
            if (p.reads == 2)
                journal.prepare(p.identity, original, "newer-feed");
        };
        rejected(
            [&] {
                lease.apply(p, "feed-A");
            },
            "Ownership changed during read was accepted");
        require(p.writes == 0 && journal.snapshot()[0].revision == 2,
                "Stale apply wrote a setting or erased newer ownership");
    }
    {
        const auto path = fixture.path();
        std::array<std::string, 3> settings;
        uint64_t retainedRevision{};
        {
            AppRoutingJournal journal(path, true);
            Preferences p(journal);
            AppRoutingLease lease(journal);
            lease.apply(p, "feed-A");
            p.values[0] = original[0];
            require(
                lease.apply(p, "feed-A", AppRoutingOverrideMode::RetainUntilDesiredStateSaved) ==
                        AppRoutingApplyResult::UserOverride &&
                    p.values == original,
                "Deferred override did not restore remaining owned roles");
            require(journal.snapshot().size() == 1,
                    "Unsaved override lost durable ownership evidence");
            retainedRevision = journal.snapshot()[0].revision;
            settings = p.values;
        }
        {
            AppRoutingJournal reopened(path);
            Preferences p(reopened);
            p.values = settings;
            AppRoutingLease recovered(reopened);
            require(recovered.apply(p, "feed-A",
                                    AppRoutingOverrideMode::RetainUntilDesiredStateSaved) ==
                            AppRoutingApplyResult::UserOverride &&
                        p.writes == 0 && reopened.snapshot()[0].revision == retainedRevision,
                    "Controller restart retook the user's original output before desired state was "
                    "saved");
            // Desired state is now durably disabled by the platform. Only then
            // may a verified release acknowledge/remove the recovery receipt.
            require(recovered.release(p) == AppRoutingReleaseResult::Restored &&
                        reopened.snapshot().empty(),
                    "Saved override acknowledgment failed");
        }
    }
    {
        AppRoutingJournal journal(fixture.path(), true);
        Preferences p(journal);
        AppRoutingLease lease(journal);
        lease.apply(p, "feed-A");
        p.values[1] = "user-choice";
        require(lease.apply(p, "feed-A", AppRoutingOverrideMode::RetainUntilDesiredStateSaved) ==
                        AppRoutingApplyResult::UserOverride &&
                    p.values[1] == "user-choice" && !journal.snapshot().empty(),
                "Non-original override was not retained for acknowledgment");
        require(lease.release(p, AppRoutingReleaseMode::RetainVerified) ==
                        AppRoutingReleaseResult::PreservedUserChange &&
                    !journal.snapshot().empty(),
                "Verified-but-unacknowledged release cleared receipt");
        require(lease.release(p) == AppRoutingReleaseResult::PreservedUserChange &&
                    journal.snapshot().empty(),
                "Final override acknowledgment lost user choice");
    }
    std::cout << "PASS: durable-before-mutation leases, reapply, partial failures, verified "
                 "release, user overrides, stale ownership and identity checks\n";
}
