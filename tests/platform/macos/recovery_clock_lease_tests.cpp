#include "../../../native/platform/macos/probe/recovery_clock_lease.hpp"
#include <cassert>

namespace {
using namespace loopz::macos::diagnostic;
struct Calls {
    bool same{true}, throwOnIdentity{false};
    OSStatus startStatus{noErr}, stopStatus{noErr};
    unsigned starts{0}, stops{0};
    RecoveryClockOperations operations() {
        return {[this] {
                    if (throwOnIdentity)
                        throw std::runtime_error("Identity inspection failed");
                    return same;
                },
                [this] {
                    ++starts;
                    return startStatus;
                },
                [this] {
                    ++stops;
                    return stopStatus;
                }};
    }
};
} // namespace
int main() {
    Calls success;
    {
        RecoveryClockLease lease(success.operations());
        assert(success.starts == 1 && success.stops == 0);
        assert(lease.close() && lease.close());
    }
    assert(success.stops == 1);

    Calls unwind;
    try {
        RecoveryClockLease lease(unwind.operations());
        throw std::runtime_error("Subsequent clock query failed");
    } catch (const std::runtime_error&) {
    }
    assert(unwind.starts == 1 && unwind.stops == 1);

    Calls failedStart;
    failedStart.startStatus = kAudioHardwareIllegalOperationError;
    bool rejected = false;
    try {
        RecoveryClockLease lease(failedStart.operations());
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    assert(rejected && failedStart.starts == 1 && !failedStart.stops);

    Calls foreign;
    foreign.same = false;
    rejected = false;
    try {
        RecoveryClockLease lease(foreign.operations());
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected && !foreign.starts && !foreign.stops);

    Calls replaced;
    {
        RecoveryClockLease lease(replaced.operations());
        replaced.same = false;
        assert(!lease.close());
        replaced.same = true;
        assert(!lease.close());
    }
    assert(replaced.starts == 1 && !replaced.stops);

    Calls failedStop;
    {
        RecoveryClockLease lease(failedStop.operations());
        failedStop.stopStatus = kAudioHardwareIllegalOperationError;
        assert(!lease.close() && !lease.close());
    }
    assert(failedStop.starts == 1 && failedStop.stops == 1);

    Calls unknown;
    {
        RecoveryClockLease lease(unknown.operations());
        unknown.throwOnIdentity = true;
        assert(!lease.close());
    }
    assert(unknown.starts == 1 && !unknown.stops);
    std::puts(
        "Timing-service ownership balances startup/unwind and refuses stale or uncertain release");
}
