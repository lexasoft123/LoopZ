#pragma once

#include <loopz/macos/transport_pool.hpp>
#include <xpc/xpc.h>

namespace loopz::macos {

// Snapshot schema is independent of the unchanged protocol-2 audio mapping
// and service envelope. Version 3 adds immutable endpoint latency metadata.
inline constexpr uint32_t kDeviceTransportProtocol = 3;

// Control-thread codec only. Call it after authenticating and binding the XPC
// peer's role; these functions do not authorize a connection or publish HAL IO.
XPC_RETURNS_RETAINED xpc_object_t encodeDeviceTransports(std::span<const DeviceTransport> devices,
                                                         uint64_t revision);

class RemoteDeviceTransport {
  public:
    ~RemoteDeviceTransport();
    RemoteDeviceTransport(RemoteDeviceTransport&& other) noexcept;
    RemoteDeviceTransport& operator=(RemoteDeviceTransport&& other) noexcept;
    RemoteDeviceTransport(const RemoteDeviceTransport&) = delete;
    RemoteDeviceTransport& operator=(const RemoteDeviceTransport&) = delete;
    const std::string& id() const noexcept;
    const std::string& uid() const noexcept;
    const std::string& name() const noexcept;
    bool hasOutput() const noexcept;
    uint32_t inputLatencyFrames() const noexcept;
    DeviceAudioView view() const;

  private:
    friend struct DeviceTransportSnapshot;
    RemoteDeviceTransport(std::string id, std::string uid, std::string name, bool hasOutput,
                          uint32_t inputLatencyFrames);
    static RemoteDeviceTransport decode(xpc_object_t object);
    std::string id_, uid_, name_;
    bool hasOutput_;
    uint32_t inputLatencyFrames_;
    void* mapping_{nullptr};
    size_t bytes_{0};
};

struct DeviceTransportSnapshot {
    uint64_t revision;
    std::vector<RemoteDeviceTransport> devices;
    static DeviceTransportSnapshot decode(xpc_object_t message);
};

} // namespace loopz::macos
