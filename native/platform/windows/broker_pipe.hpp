#pragma once
#include <windows.h>
#include <string>
#include <string_view>

namespace loopz::windows {
inline constexpr wchar_t kDeviceBrokerPipe[] = L"\\\\.\\pipe\\LoopZDeviceBroker-v1";
// Explicit client access excludes FILE_CREATE_PIPE_INSTANCE (generic write
// includes that permission). Authenticated users cannot add server instances.
inline constexpr char kDeviceBrokerReplyAck = 6;
inline constexpr DWORD kDeviceBrokerClientAccess = FILE_READ_DATA | FILE_WRITE_DATA |
                                                   FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES |
                                                   SYNCHRONIZE | READ_CONTROL;
struct BrokerMessage {
    std::wstring owner;
    std::string bytes;
};
enum class BrokerReceive { Message, Rejected, Stopped };
// One control owner, one request/reply per connection. Keep the server handle
// open between clients so the name is never relinquished during normal service.
class BrokerPipe final {
  public:
    explicit BrokerPipe(HANDLE stopEvent, std::wstring_view name = kDeviceBrokerPipe);
    ~BrokerPipe();
    BrokerPipe(const BrokerPipe&) = delete;
    BrokerPipe& operator=(const BrokerPipe&) = delete;
    BrokerReceive receive(BrokerMessage& message);
    bool reply(std::string_view bytes);
    void disconnect() noexcept;

  private:
    HANDLE pipe_{INVALID_HANDLE_VALUE}, stop_{};
    bool connected_{};
};
} // namespace loopz::windows
