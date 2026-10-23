#include "broker_pipe.hpp"
#include "broker_identity.hpp"
#include "driver_identity.hpp"
#include <loopz/device_intent.hpp>
#include <sddl.h>
#include <array>
#include <stdexcept>

namespace loopz::windows {
static_assert(kDeviceBrokerClientAccess == 0x120183); // Matches the AU DACL below.
namespace {
struct Event {
    HANDLE value{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Event() {
        if (!value)
            throw std::runtime_error("Cannot create setup I/O event");
    }
    ~Event() {
        CloseHandle(value);
    }
};
// OVERLAPPED storage and its event outlive cancellation completion, including
// timeout/stop/error paths. Never close or reuse an in-flight stack OVERLAPPED.
bool complete(HANDLE pipe, HANDLE stop, OVERLAPPED& operation, DWORD timeout, DWORD& bytes,
              DWORD& error) {
    const std::array<HANDLE, 2> events{stop, operation.hEvent};
    const auto waited =
        WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, timeout);
    if (waited != WAIT_OBJECT_0 + 1) {
        CancelIoEx(pipe, &operation);
        GetOverlappedResult(pipe, &operation, &bytes, TRUE);
        error = waited == WAIT_OBJECT_0 ? ERROR_OPERATION_ABORTED : ERROR_TIMEOUT;
        return false;
    }
    if (GetOverlappedResult(pipe, &operation, &bytes, FALSE)) {
        error = ERROR_SUCCESS;
        return true;
    }
    error = GetLastError();
    return false;
}
bool stopped(HANDLE event) noexcept {
    return WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
}
} // namespace
BrokerPipe::BrokerPipe(HANDLE stopEvent, std::wstring_view name) : stop_(stopEvent) {
    if (!stop_ || stop_ == INVALID_HANDLE_VALUE || WaitForSingleObject(stop_, 0) == WAIT_FAILED)
        throw std::invalid_argument("Setup pipe requires a valid stop event");
    const bool fixture = name.starts_with(L"\\\\.\\pipe\\LoopZDeviceBrokerTest-");
    if ((name != kDeviceBrokerPipe && !fixture) || name.size() > 200 ||
        name.find(L'\0') != std::wstring_view::npos)
        throw std::invalid_argument("Invalid setup server pipe name");
    const auto identity = currentOwnerSid();
    if (!fixture && identity != L"S-1-5-18")
        throw std::runtime_error("The installed setup pipe must be owned by LocalSystem");
    std::wstring security = L"D:P(D;;GA;;;AN)(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x120183;;;AU)";
    if (fixture)
        security += L"(A;;GA;;;" + identity + L")";
    PSECURITY_DESCRIPTOR descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(security.c_str(), SDDL_REVISION_1,
                                                              &descriptor, nullptr))
        throw std::runtime_error("Cannot prepare setup pipe access");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    const std::wstring path(name);
    pipe_ = CreateNamedPipeW(
        path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
        65536, 65536, 0, &attributes);
    LocalFree(descriptor);
    if (pipe_ == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot exclusively own the local setup pipe");
}
BrokerPipe::~BrokerPipe() {
    disconnect();
    if (pipe_ != INVALID_HANDLE_VALUE)
        CloseHandle(pipe_);
}
void BrokerPipe::disconnect() noexcept {
    if (connected_)
        DisconnectNamedPipe(pipe_);
    connected_ = false;
}
BrokerReceive BrokerPipe::receive(BrokerMessage& message) {
    disconnect();
    message = {};
    if (stopped(stop_))
        return BrokerReceive::Stopped;
    Event event;
    OVERLAPPED operation{};
    operation.hEvent = event.value;
    DWORD bytes{}, error{};
    const bool accepted = ConnectNamedPipe(pipe_, &operation) != FALSE;
    error = accepted ? ERROR_SUCCESS : GetLastError();
    if (!accepted && error == ERROR_IO_PENDING) {
        if (!complete(pipe_, stop_, operation, INFINITE, bytes, error)) {
            connected_ = true; // Cancellation can race with a successful accept.
            disconnect();
            return stopped(stop_) ? BrokerReceive::Stopped : BrokerReceive::Rejected;
        }
    } else if (!accepted && error != ERROR_PIPE_CONNECTED) {
        connected_ = true;
        disconnect();
        return BrokerReceive::Rejected;
    }
    connected_ = true;
    ResetEvent(event.value);
    operation = {};
    operation.hEvent = event.value;
    std::string payload(kMaximumDeviceIntentBytes + 1, '\0');
    const bool read = ReadFile(pipe_, payload.data(), static_cast<DWORD>(payload.size()), &bytes,
                               &operation) != FALSE;
    error = read ? ERROR_SUCCESS : GetLastError();
    bool received = read;
    if (!read && error == ERROR_IO_PENDING)
        received = complete(pipe_, stop_, operation, 5000, bytes, error);
    if (!received || !bytes || bytes > kMaximumDeviceIntentBytes || stopped(stop_)) {
        disconnect();
        return stopped(stop_) ? BrokerReceive::Stopped : BrokerReceive::Rejected;
    }
    payload.resize(bytes);
    try {
        message.owner = brokerClientOwner(pipe_);
        message.bytes = std::move(payload);
    } catch (...) {
        disconnect();
        return BrokerReceive::Rejected;
    }
    return BrokerReceive::Message;
}
bool BrokerPipe::reply(std::string_view bytes) {
    if (!connected_ || bytes.empty() || bytes.size() > 256 * 1024 || stopped(stop_)) {
        disconnect();
        return false;
    }
    Event event;
    OVERLAPPED operation{};
    operation.hEvent = event.value;
    DWORD written{}, error{};
    bool sent = WriteFile(pipe_, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                          &operation) != FALSE;
    error = sent ? ERROR_SUCCESS : GetLastError();
    if (!sent && error == ERROR_IO_PENDING)
        sent = complete(pipe_, stop_, operation, 5000, written, error);
    if (!sent || written != bytes.size()) {
        disconnect();
        return false;
    }
    // A bounded acknowledgment replaces unbounded FlushFileBuffers. The client
    // sends it only after consuming the complete reply, so the next receive may
    // disconnect without dropping an unread response. Lost replies remain safe
    // to retry through durable operation IDs.
    ResetEvent(event.value);
    operation = {};
    operation.hEvent = event.value;
    std::array<char, 2> acknowledgment{};
    DWORD count{};
    bool read = ReadFile(pipe_, acknowledgment.data(), static_cast<DWORD>(acknowledgment.size()),
                         &count, &operation) != FALSE;
    error = read ? ERROR_SUCCESS : GetLastError();
    if (!read && error == ERROR_IO_PENDING)
        read = complete(pipe_, stop_, operation, 5000, count, error);
    const bool acknowledged = read && count == 1 && acknowledgment[0] == kDeviceBrokerReplyAck;
    disconnect();
    return acknowledged;
}
} // namespace loopz::windows
