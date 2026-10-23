#include "broker_identity.hpp"
#include <sddl.h>
#include <exception>
#include <stdexcept>
#include <vector>

namespace loopz::windows {
namespace {
[[noreturn]] void fail(const char* operation) {
    throw std::runtime_error(std::string(operation) + " (Windows error " +
                             std::to_string(GetLastError()) + ")");
}
struct Token {
    HANDLE value{};
    ~Token() {
        if (value)
            CloseHandle(value);
    }
};
struct Impersonation {
    explicit Impersonation(HANDLE pipe) {
        if (!ImpersonateNamedPipeClient(pipe))
            fail("Authenticate microphone setup client");
    }
    ~Impersonation() {
        // Continuing under a client token would make subsequent requests run
        // with the wrong identity. There is no safe recovery on this thread.
        if (!RevertToSelf())
            std::terminate();
    }
};
struct SidText {
    LPWSTR value{};
    ~SidText() {
        if (value)
            LocalFree(value);
    }
};
} // namespace
std::wstring brokerClientOwner(HANDLE serverPipe) {
    Token previous;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &previous.value))
        throw std::runtime_error("Setup authentication cannot nest impersonation");
    if (GetLastError() != ERROR_NO_TOKEN)
        fail("Inspect setup server identity");
    DWORD flags{};
    if (!GetNamedPipeInfo(serverPipe, &flags, nullptr, nullptr, nullptr))
        fail("Inspect setup server pipe");
    if (!(flags & PIPE_SERVER_END) || !(flags & PIPE_TYPE_MESSAGE))
        throw std::invalid_argument("Setup authentication requires a message-mode server pipe");
    Impersonation impersonation(serverPipe);
    Token client;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &client.value))
        fail("Read setup client token");
    SECURITY_IMPERSONATION_LEVEL level{};
    DWORD length{};
    if (!GetTokenInformation(client.value, TokenImpersonationLevel, &level, sizeof(level), &length))
        fail("Read setup authentication level");
    if (level < SecurityIdentification)
        throw std::runtime_error("Anonymous microphone setup clients are not allowed");
    if (GetTokenInformation(client.value, TokenUser, nullptr, 0, &length) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || !length || length > 65536)
        throw std::runtime_error("Invalid setup client identity size");
    std::vector<unsigned char> bytes(length);
    if (!GetTokenInformation(client.value, TokenUser, bytes.data(), length, &length))
        fail("Read setup client identity");
    const auto sid = reinterpret_cast<const TOKEN_USER*>(bytes.data())->User.Sid;
    if (!IsValidSid(sid) || IsWellKnownSid(sid, WinAnonymousSid))
        throw std::runtime_error("Invalid setup client identity");
    SidText text;
    if (!ConvertSidToStringSidW(sid, &text.value))
        fail("Encode setup client identity");
    return std::wstring(text.value);
}
} // namespace loopz::windows
