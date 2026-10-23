#include "ipc.hpp"
#include "runtime.hpp"
#include <windows.h>
#include <sddl.h>
#include <objbase.h>
#include <array>
#include <algorithm>
#include <mutex>
#include <thread>
#include <vector>

namespace loopz::windows {
namespace {
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
std::wstring userSID() {
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value))
        throw std::runtime_error("Cannot read current user token");
    DWORD length = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &length);
    std::vector<BYTE> buffer(length);
    if (!GetTokenInformation(token.value, TokenUser, buffer.data(), length, &length))
        throw std::runtime_error("Cannot read current user identity");
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text))
        throw std::runtime_error("Cannot encode current user identity");
    std::wstring result(text);
    LocalFree(text);
    return result;
}
int childEngine() {
    std::array<wchar_t, 32768> path{};
    const auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count == path.size())
        return 1;
    std::wstring command = L"\"" + std::wstring(path.data()) + L"\" --engine";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(path.data(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process))
        return 1;
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    return exitCode == 0 ? 0 : 1;
}
std::string readMessage(HANDLE pipe) {
    std::string bytes;
    std::array<char, 16384> buffer{};
    for (;;) {
        DWORD count = 0;
        const bool ok = ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &count,
                                 nullptr) != FALSE;
        const auto error = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && error != ERROR_MORE_DATA)
            throw std::runtime_error("Audio engine connection closed");
        if (bytes.size() + count > 1024 * 1024)
            throw std::runtime_error("Audio engine message is too large");
        bytes.append(buffer.data(), count);
        if (ok)
            return bytes;
    }
}
} // namespace
std::wstring pipeName() {
    return L"\\\\.\\pipe\\LoopZ.Engine.v1." + userSID();
}
void startEngine() {
    wchar_t path[32768]{};
    const auto length = GetModuleFileNameW(nullptr, path, 32768);
    if (!length || length >= 32768)
        throw std::runtime_error("Cannot locate LoopZ executable");
    std::wstring command = L"\"" + std::wstring(path) + L"\" --background";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(path, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &startup, &info))
        throw std::runtime_error("Cannot start the Windows audio engine");
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
}
JSON engineRequest(const JSON& request) {
    const auto name = pipeName();
    Handle pipe;
    const auto deadline = GetTickCount64() + 10000;
    do {
        pipe.value = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
        if (pipe.value != INVALID_HANDLE_VALUE)
            break;
        const auto error = GetLastError();
        if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND)
            break;
        (void)WaitNamedPipeW(name.c_str(), 100);
        Sleep(5);
    } while (GetTickCount64() < deadline);
    if (pipe.value == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot connect to the audio engine");
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe.value, &mode, nullptr, nullptr))
        throw std::runtime_error("Cannot set audio transport mode");
    const auto bytes = request.dump();
    if (bytes.size() > 1024 * 1024)
        throw std::runtime_error("Audio request is too large");
    DWORD written = 0;
    if (!WriteFile(pipe.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
        written != bytes.size())
        throw std::runtime_error("Audio request could not be delivered");
    return parseControlJSON(readMessage(pipe.value));
}
int runBackground() {
    const auto sid = userSID();
    Handle mutex{CreateMutexW(nullptr, TRUE, (L"Local\\LoopZ.Supervisor.v1." + sid).c_str())};
    if (!mutex.value)
        return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;
    unsigned failures = 0;
    for (;;) {
        const auto started = GetTickCount64();
        if (childEngine() == 0)
            return 0; // An explicit shutdown must never restart routing.
        if (GetTickCount64() - started > 60000)
            failures = 0;
        if (++failures > 10)
            return 1; // Avoid an endless crash loop after a broken upgrade.
        Sleep(std::min<DWORD>(5000, failures * 500));
    }
}
int runEngine() {
    const auto sid = userSID();
    Handle mutex{CreateMutexW(nullptr, TRUE, (L"Local\\LoopZ.Engine.v1." + sid).c_str())};
    if (!mutex.value)
        return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto dacl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + sid + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(dacl.c_str(), SDDL_REVISION_1,
                                                              &descriptor, nullptr))
        return 1;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    const auto name = pipeName();
    Handle pipe{CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                 PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
                                     PIPE_REJECT_REMOTE_CLIENTS,
                                 1, 1024 * 1024, 1024 * 1024, 5000, &security)};
    LocalFree(descriptor);
    if (pipe.value == INVALID_HANDLE_VALUE)
        return 1;
    std::mutex control;
    Runtime runtime(userDirectory() / L"routing.json", true);
    std::jthread recovery([&](std::stop_token stop) {
        const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(apartment))
            return;
        unsigned inventoryTicks = 0;
        while (!stop.stop_requested()) {
            Sleep(100);
            if (stop.stop_requested())
                break;
            try {
                std::lock_guard lock(control);
                if (++inventoryTicks == 30) {
                    inventoryTicks = 0;
                    (void)runtime.request({{"version", 1}, {"operation", "state"}});
                } else {
                    runtime.recoverAudio();
                }
            } catch (...) {
            }
        }
        CoUninitialize();
    });
    for (;;) {
        if (!ConnectNamedPipe(pipe.value, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
            return 1;
        bool shutdown = false;
        try {
            const auto request = parseControlJSON(readMessage(pipe.value));
            shutdown =
                request.value("version", 0) == 1 && request.value("operation", "") == "shutdown";
            std::string reply;
            {
                std::lock_guard lock(control);
                reply = (shutdown ? JSON{{"ok", true}} : runtime.request(request)).dump();
            }
            if (reply.size() <= 1024 * 1024) {
                DWORD written = 0;
                (void)WriteFile(pipe.value, reply.data(), static_cast<DWORD>(reply.size()),
                                &written, nullptr);
                (void)FlushFileBuffers(pipe.value);
            }
        } catch (...) {
        }
        DisconnectNamedPipe(pipe.value);
        if (shutdown)
            return 0;
    }
}
} // namespace loopz::windows
