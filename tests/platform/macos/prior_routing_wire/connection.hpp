#pragma once
#include "wire.hpp"
#include <atomic>

namespace loopz::macos::routing {
class Connection {
  public:
    using Handler = std::function<xpc_object_t(xpc_object_t)>;
    Connection(RoutingServiceEndpoint endpoint, const char* role, Handler handler = {},
               std::function<void(bool)> availability = {});
    ~Connection();
    bool connect(std::string& error);
    void disconnect() noexcept;
    bool alive() const noexcept;
    uint64_t session() const noexcept;
    xpc_object_t request(const char* operation, xpc_object_t payload, uint64_t revision);

  private:
    struct State;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz::macos::routing
