#pragma once
#include <chrono>
#include <string>
#include <string_view>
#include <vector>
#include <sys/types.h>

namespace loopz::macos {
// One private, framed child channel. Only the maintenance adapter selects the
// executable; no application request can supply a command or descriptor.
class CatalogChannel {
  public:
    explicit CatalogChannel(const std::vector<std::string>& arguments,
                            std::chrono::milliseconds timeout = std::chrono::seconds(20));
    ~CatalogChannel();
    CatalogChannel(const CatalogChannel&) = delete;
    CatalogChannel& operator=(const CatalogChannel&) = delete;
    std::string exchange(std::string_view request);

  private:
    void stop() noexcept;
    pid_t child_{-1};
    int input_{-1}, output_{-1};
    std::chrono::milliseconds timeout_;
    bool failed_{false};
};
} // namespace loopz::macos
