#pragma once
#include <loopz/capture_buffer.hpp>
#include <loopz/macos/driver_protocol.hpp>
#include <memory>
#include <string>
#include <vector>

namespace loopz {
struct AppInfo {
    std::string bundle, name;
    int pid;
};
std::vector<AppInfo> applications();
std::string inventoryJSON();
struct ApplicationCaptureConfig {
    bool preferDeviceStream{true};
    bool requireDeviceStream{false};
    uint32_t requestedBufferFrames{128};
};
class Capture {
  public:
    Capture();
    ~Capture();
    bool startApplication(const std::string& bundle, bool muteOriginal, std::string& error,
                          const ApplicationCaptureConfig& config = {});
    bool startMicrophone(const std::string& uid, std::string& error);
    bool healthy(std::string& error) const;
    void stop();
    CaptureBuffer& app();
    CaptureBuffer& mic();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
uint64_t hostNowNs() noexcept;
uint64_t ticksToNs(uint64_t ticks) noexcept;
uint64_t nsToTicks(uint64_t ns) noexcept;
} // namespace loopz
