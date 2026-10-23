#include "broker_identity.hpp"
#include <sddl.h>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace loopz::windows;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
template <class F> void rejects(F&& action) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "Expected authentication rejection");
}
void notImpersonating() {
    Handle token;
    require(!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token.value) &&
                GetLastError() == ERROR_NO_TOKEN,
            "Client token leaked onto server thread");
}
void isProcessOwner(const std::wstring& text) {
    Handle token;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value) != FALSE,
            "Read expected process identity");
    DWORD length{};
    require(!GetTokenInformation(token.value, TokenUser, nullptr, 0, &length) &&
                GetLastError() == ERROR_INSUFFICIENT_BUFFER,
            "Measure expected identity");
    std::vector<unsigned char> bytes(length);
    require(GetTokenInformation(token.value, TokenUser, bytes.data(), length, &length) != FALSE,
            "Read expected identity");
    PSID actual{};
    require(ConvertStringSidToSidW(text.c_str(), &actual) != FALSE, "Decode authenticated owner");
    const bool equal =
        EqualSid(actual, reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid) != FALSE;
    LocalFree(actual);
    require(equal, "Authenticated owner differs from actual client principal");
}
void complete(HANDLE pipe, OVERLAPPED& operation, bool pending) {
    if (!pending)
        return;
    if (WaitForSingleObject(operation.hEvent, 5000) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &operation);
        DWORD ignored{};
        GetOverlappedResult(pipe, &operation, &ignored, TRUE);
        throw std::runtime_error("Timed out waiting for test pipe");
    }
    DWORD transferred{};
    require(GetOverlappedResult(pipe, &operation, &transferred, FALSE) != FALSE,
            "Test pipe operation failed");
}
std::wstring exchange(DWORD quality, unsigned index, bool anonymous = false,
                      bool byteMode = false) {
    const auto name = L"\\\\.\\pipe\\LoopZ.BrokerIdentity.Test." +
                      std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(index);
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    require(event.value != nullptr, "Create test event");
    Handle server{CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
        (byteMode ? PIPE_TYPE_BYTE | PIPE_READMODE_BYTE
                  : PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE) |
            PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 512, 512, 5000, nullptr)};
    require(server.value != INVALID_HANDLE_VALUE, "Create test pipe");
    OVERLAPPED connect{};
    connect.hEvent = event.value;
    const bool connected = ConnectNamedPipe(server.value, &connect) != FALSE;
    const auto error = connected ? ERROR_SUCCESS : GetLastError();
    require(connected || error == ERROR_IO_PENDING, "Begin test connection");
    Handle client{CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              SECURITY_SQOS_PRESENT | quality, nullptr)};
    if (client.value == INVALID_HANDLE_VALUE) {
        CancelIoEx(server.value, &connect);
        DWORD ignored{};
        GetOverlappedResult(server.value, &connect, &ignored, TRUE);
        throw std::runtime_error("Connect test client");
    }
    complete(server.value, connect, !connected);
    const char request = 'x';
    DWORD bytes{};
    require(WriteFile(client.value, &request, 1, &bytes, nullptr) && bytes == 1,
            "Write test request");
    ResetEvent(event.value);
    OVERLAPPED read{};
    read.hEvent = event.value;
    char received{};
    const bool readNow = ReadFile(server.value, &received, 1, &bytes, &read) != FALSE;
    require(readNow || GetLastError() == ERROR_IO_PENDING, "Read test request");
    complete(server.value, read, !readNow);
    require(received == request, "Test message corrupted");
    std::wstring owner;
    if (anonymous || byteMode) {
        rejects([&] {
            (void)brokerClientOwner(server.value);
        });
    } else {
        owner = brokerClientOwner(server.value);
        require(owner.starts_with(L"S-1-"), "Missing authenticated SID");
        require(ImpersonateSelf(SecurityImpersonation) != FALSE, "Prepare nested-token test");
        try {
            rejects([&] {
                (void)brokerClientOwner(server.value);
            });
            Handle stillImpersonating;
            require(OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE,
                                    &stillImpersonating.value) != FALSE,
                    "Nested rejection removed existing token");
        } catch (...) {
            if (!RevertToSelf())
                std::terminate();
            throw;
        }
        require(RevertToSelf() != FALSE, "Restore nested-token test identity");
    }
    notImpersonating();
    rejects([&] {
        (void)brokerClientOwner(client.value);
    });
    notImpersonating();
    return owner;
}
} // namespace
int main() {
    try {
        rejects([] {
            (void)brokerClientOwner(INVALID_HANDLE_VALUE);
        });
        notImpersonating();
        const auto owner = exchange(SECURITY_IMPERSONATION, 1);
        isProcessOwner(owner);
        require(exchange(SECURITY_IDENTIFICATION, 2) == owner,
                "Identification token changed owner");
        (void)exchange(SECURITY_ANONYMOUS, 3, true);
        (void)exchange(SECURITY_IMPERSONATION, 4, false, true);
        std::cout << "Broker identity: token owner, anonymous rejection, wrong pipe end, "
                     "byte-mode rejection, nested token preservation and restoration passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
