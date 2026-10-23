#pragma once
#include "driver/transport_protocol.hpp"
#include <memory>
#include <string>

namespace loopz::windows {
// Blocking private KS calls. Use on the COM-initialized transport worker only;
// never call from a WASAPI callback or the audio render thread.
class DriverTransport final {
  public:
    DriverTransport();
    ~DriverTransport();
    DriverTransport(const DriverTransport&) = delete;
    DriverTransport& operator=(const DriverTransport&) = delete;
    bool connect(const std::string& endpoint, std::string& error, bool preferFloat = true);
    void close() noexcept;
    loopz_driver::AudioFormat format() const noexcept;
    unsigned version() const noexcept;
    unsigned sampleBytes() const noexcept;
    unsigned frameBytes() const noexcept;
    unsigned packetFrames() const noexcept;
    bool read(loopz_driver::DuplexBridge::Packet& packet, std::string& error);
    bool write(const unsigned char* samples, unsigned frames, std::string& error);
    // Optional additive property: older transport-v2 drivers can reject this
    // without affecting ordinary playback/publication.
    bool diagnostics(loopz_driver::DuplexBridge::Diagnostics& value);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz::windows
