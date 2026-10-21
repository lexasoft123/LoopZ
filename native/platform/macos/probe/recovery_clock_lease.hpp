#pragma once

#include <CoreAudio/CoreAudio.h>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

namespace loopz::macos::diagnostic {
struct RecoveryClockOperations {
    std::function<bool()> sameVirtualDevice;
    std::function<OSStatus()> start;
    std::function<OSStatus()> stop;
};

// Control-domain ownership of AudioDeviceStart(device, nullptr), required for
// the public timing services even when AUHAL audio callbacks already run.
// A stale identity never causes Stop on a newly reused/foreign object. Failed
// release is reported once, rather than blindly repeating an uncertain Stop.
class RecoveryClockLease {
  public:
    explicit RecoveryClockLease(RecoveryClockOperations operations)
        : operations_(std::move(operations)) {
        if (!operations_.sameVirtualDevice || !operations_.start || !operations_.stop ||
            !operations_.sameVirtualDevice())
            throw std::invalid_argument("Recovery clock identity is unavailable");
        const auto status = operations_.start();
        if (status)
            throw std::runtime_error("Recovery timing services could not start (status " +
                                     std::to_string(status) + ")");
        active_ = true;
    }
    ~RecoveryClockLease() {
        if (active_ && !close())
            std::fputs("Recovery timing service release could not be verified\n", stderr);
    }
    RecoveryClockLease(const RecoveryClockLease&) = delete;
    RecoveryClockLease& operator=(const RecoveryClockLease&) = delete;
    bool close() noexcept {
        if (!active_)
            return !failed_;
        active_ = false;
        try {
            failed_ = !operations_.sameVirtualDevice() || operations_.stop() != noErr;
        } catch (...) {
            failed_ = true;
        }
        return !failed_;
    }

  private:
    const RecoveryClockOperations operations_;
    bool active_{false}, failed_{false};
};
} // namespace loopz::macos::diagnostic
