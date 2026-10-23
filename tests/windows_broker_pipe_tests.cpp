#include "broker_pipe.hpp"
#include "driver_identity.hpp"
#include <loopz/device_intent.hpp>
#include <objbase.h>
#include <array>
#include <future>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <source_location>

using namespace loopz::windows;
namespace {
void require(bool value, std::source_location location = std::source_location::current()) {
    if (!value)
        throw std::runtime_error("Broker pipe regression failed at line " +
                                 std::to_string(location.line()) + "; Windows error " +
                                 std::to_string(GetLastError()));
}
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
struct StopOnExit {
    HANDLE event;
    ~StopOnExit() {
        SetEvent(event);
    }
};
DWORD transfer(HANDLE pipe, bool write, void* buffer, DWORD size) {
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    require(event.value != nullptr);
    OVERLAPPED operation{};
    operation.hEvent = event.value;
    DWORD count{};
    const bool done = write ? WriteFile(pipe, buffer, size, &count, &operation) != FALSE
                            : ReadFile(pipe, buffer, size, &count, &operation) != FALSE;
    if (done)
        return count;
    require(GetLastError() == ERROR_IO_PENDING);
    if (WaitForSingleObject(event.value, 3000) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &operation);
        GetOverlappedResult(pipe, &operation, &count, TRUE);
        throw std::runtime_error("Broker fixture I/O timed out");
    }
    require(GetOverlappedResult(pipe, &operation, &count, FALSE) != FALSE);
    return count;
}
void connect(Handle& client, const std::wstring& name, DWORD level = SECURITY_IDENTIFICATION) {
    for (unsigned attempt = 0; attempt < 10; ++attempt) {
        client.value =
            CreateFileW(name.c_str(), kDeviceBrokerClientAccess, 0, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | level, nullptr);
        if (client.value != INVALID_HANDLE_VALUE)
            break;
        require(GetLastError() == ERROR_PIPE_BUSY);
        WaitNamedPipeW(name.c_str(), 500);
    }
    require(client.value != INVALID_HANDLE_VALUE);
    DWORD mode = PIPE_READMODE_MESSAGE;
    require(SetNamedPipeHandleState(client.value, &mode, nullptr, nullptr) != FALSE);
}
std::wstring uniqueName() {
    GUID guid{};
    require(SUCCEEDED(CoCreateGuid(&guid)));
    std::array<wchar_t, 40> text{};
    require(StringFromGUID2(guid, text.data(), static_cast<int>(text.size())) != 0);
    return L"\\\\.\\pipe\\LoopZDeviceBrokerTest-" + std::wstring(text.data());
}
void exchangeMessage(const std::wstring& name, std::string request) {
    Handle client;
    connect(client, name);
    require(transfer(client.value, true, request.data(), static_cast<DWORD>(request.size())) ==
            request.size());
    std::array<char, 32> response{};
    const auto count =
        transfer(client.value, false, response.data(), static_cast<DWORD>(response.size()));
    require(std::string_view(response.data(), count) == "accepted");
    char ack = kDeviceBrokerReplyAck;
    require(transfer(client.value, true, &ack, 1) == 1);
}
} // namespace
int main() {
    try {
        Handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        require(stop.value != nullptr);
        const auto name = uniqueName();
        BrokerPipe pipe(stop.value, name);
        bool duplicateRejected = false;
        try {
            BrokerPipe duplicate(stop.value, name);
        } catch (const std::runtime_error&) {
            duplicateRejected = true;
        }
        require(duplicateRejected);
        const auto owner = currentOwnerSid();
        auto server = std::async(std::launch::async, [&] {
            BrokerMessage message;
            require(pipe.receive(message) == BrokerReceive::Message);
            require(message.owner == owner && message.bytes == "first");
            require(pipe.reply("accepted"));
            require(pipe.receive(message) == BrokerReceive::Rejected); // Anonymous token.
            require(message.owner.empty() && message.bytes.empty());
            require(pipe.receive(message) == BrokerReceive::Message);
            require(message.bytes.size() == loopz::kMaximumDeviceIntentBytes &&
                    message.bytes.find_first_not_of('x') == std::string::npos);
            require(pipe.reply("accepted"));
            require(pipe.receive(message) == BrokerReceive::Rejected);
            require(message.owner.empty() && message.bytes.empty());
            require(pipe.receive(message) == BrokerReceive::Message);
            require(message.owner == owner && message.bytes == "after rejection");
            require(pipe.reply("accepted"));
            require(pipe.receive(message) == BrokerReceive::Stopped);
        });
        StopOnExit cleanup{stop.value};
        exchangeMessage(name, "first");
        {
            Handle anonymous;
            connect(anonymous, name, SECURITY_ANONYMOUS);
            char message[] = "anonymous";
            transfer(anonymous.value, true, message, sizeof(message));
        }
        exchangeMessage(name, std::string(loopz::kMaximumDeviceIntentBytes, 'x'));
        {
            Handle client;
            connect(client, name);
            std::string tooLarge(loopz::kMaximumDeviceIntentBytes + 1, 'x');
            // The server consumes this whole message, rejects it, then reuses
            // its exclusive server handle for the next client.
            transfer(client.value, true, tooLarge.data(), static_cast<DWORD>(tooLarge.size()));
        }
        exchangeMessage(name, "after rejection");
        {
            Handle idle;
            connect(idle, name);
            require(SetEvent(stop.value) != FALSE);
            require(server.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
            server.get();
        }
        Handle secondStop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        BrokerPipe idleListener(secondStop.value, uniqueName());
        auto waiting = std::async(std::launch::async, [&] {
            BrokerMessage message;
            return idleListener.receive(message);
        });
        StopOnExit secondCleanup{secondStop.value};
        SetEvent(secondStop.value);
        require(waiting.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        require(waiting.get() == BrokerReceive::Stopped);
        Handle timeoutStop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        const auto timeoutName = uniqueName();
        BrokerPipe timeoutPipe(timeoutStop.value, timeoutName);
        auto timed = std::async(std::launch::async, [&] {
            BrokerMessage message;
            return timeoutPipe.receive(message);
        });
        StopOnExit timeoutCleanup{timeoutStop.value};
        Handle silentClient;
        connect(silentClient, timeoutName);
        require(timed.wait_for(std::chrono::seconds(7)) == std::future_status::ready);
        require(timed.get() == BrokerReceive::Rejected);
        Handle replyStop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        const auto replyName = uniqueName();
        BrokerPipe waitingReply(replyStop.value, replyName);
        auto replyWaiting = std::async(std::launch::async, [&] {
            BrokerMessage message;
            require(waitingReply.receive(message) == BrokerReceive::Message);
            return waitingReply.reply("accepted");
        });
        StopOnExit replyCleanup{replyStop.value};
        Handle withholding;
        connect(withholding, replyName);
        char request[] = "request";
        transfer(withholding.value, true, request, sizeof(request));
        std::array<char, 32> response{};
        require(transfer(withholding.value, false, response.data(),
                         static_cast<DWORD>(response.size())) == 8);
        SetEvent(replyStop.value); // Client withholds acknowledgment while service stops.
        require(replyWaiting.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        require(!replyWaiting.get());
        std::cout << "Bounded authenticated request/reply, exclusive ownership, oversized "
                     "rejection, reuse and stop passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
