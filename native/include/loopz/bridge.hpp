#pragma once
#include <loopz/timeline.hpp>
#include <atomic>
#include <memory>
#include <string>

namespace loopz {
// Connect/map/unmap only on the control thread. Mapping remains pinned until
// the caller has joined its render thread. An XPC interruption marks it dead.
struct BridgeStatus {
    uint32_t protocol{0};
    bool driverAttached{false}, helperAttached{false};
};
BridgeStatus inspectBridge(std::string& error);
class BridgeClient {
  public:
    BridgeClient();
    ~BridgeClient();
    bool connect(const char* role, std::string& error);
    bool connected() const noexcept;
    SharedAudio* audio() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz
