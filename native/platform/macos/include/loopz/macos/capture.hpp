#pragma once
#include <loopz/capture_buffer.hpp>
#include <loopz/macos/driver_protocol.hpp>
#include <loopz/input_timing.hpp>
#include <memory>
#include <string>
#include <vector>

namespace loopz {
namespace macos {
struct ApplicationAudioSelection;
}
struct AppInfo {
    std::string bundle, name;
    int pid;
};
std::vector<AppInfo> applications();
std::string inventoryJSON();
struct ApplicationCaptureRoute {
    std::string deviceUID;
    uint32_t streamIndex, channels, firstChannel, deviceChannels;
    double sampleRate;
    bool operator==(const ApplicationCaptureRoute&) const = default;
};
// Capture-free control snapshot, ordered by UID and stream index. Failure is
// empty with a diagnostic; no output may disappear by partial enumeration.
std::vector<ApplicationCaptureRoute> applicationCaptureRoutes(const std::string& bundle,
                                                              std::string& error);
struct ApplicationCaptureConfig {
    bool preferDeviceStream{true};
    bool requireDeviceStream{false};
    uint32_t requestedBufferFrames{128};
    bool deliveryDiagnostics{false};
    std::string deviceUID;
    uint32_t streamIndex{0};
};
class Capture {
  public:
    Capture();
    ~Capture();
    bool startApplication(const std::string& bundle, bool muteOriginal, std::string& error,
                          const ApplicationCaptureConfig& config = {});
    // Control only. Update the existing private tap without restarting IO or
    // clearing captured history. False asks its owner to close/reacquire.
    bool setApplicationMute(bool muteOriginal) noexcept;
    bool startMicrophone(const std::string& uid, std::string& error);
    bool healthy(std::string& error) const;
    bool healthy(const macos::ApplicationAudioSelection& selected, std::string& error) const;
    void stop();
    CaptureBuffer& app();
    CaptureBuffer& mic();
    // Control-only verified private-tap timing, available after one second of
    // valid callback delivery. Does not read the source app's playback buffer.
    // Control only. False inspects current private format before delivery is
    // warm; it cannot certify a new budget. Used to validate retained timing.
    std::optional<InputTimingFacts> applicationInputTiming(bool requireDelivery = true) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
uint64_t hostNowNs() noexcept;
uint64_t ticksToNs(uint64_t ticks) noexcept;
uint64_t nsToTicks(uint64_t ns) noexcept;
} // namespace loopz
