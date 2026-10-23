#pragma once

#include <loopz/audio_history.hpp>
#include <cstdint>
#include <memory>
#include <string>

namespace loopz::windows {
// Explicit process-tree capture: never falls back to capturing other apps.
// The worker owns COM and all WASAPI interfaces; stop joins it before history
// publication ends. A failed runtime capability probe leaves no active stream.
class ProcessCapture final {
  public:
    ProcessCapture();
    ~ProcessCapture();
    ProcessCapture(const ProcessCapture&) = delete;
    ProcessCapture& operator=(const ProcessCapture&) = delete;
    bool start(uint32_t pid, std::string& error);
    void stop();
    std::shared_ptr<AudioHistory> history() const;
    std::string error() const;
    struct PacketStatus {
        uint64_t frames, flags, positionDelta, qpcDelta100ns;
    };
    PacketStatus packetStatus() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz::windows
