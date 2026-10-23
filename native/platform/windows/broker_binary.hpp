#pragma once
#include <filesystem>
#include <memory>

namespace loopz::windows {
std::filesystem::path brokerServiceDirectory();
// Holds the protected directory and exact installed executable against rename
// through service shutdown. No command-line or IPC path override exists.
class BrokerBinaryGuard final {
  public:
    BrokerBinaryGuard();
    ~BrokerBinaryGuard();
    BrokerBinaryGuard(const BrokerBinaryGuard&) = delete;
    BrokerBinaryGuard& operator=(const BrokerBinaryGuard&) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz::windows
