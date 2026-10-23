#include "broker_binary.hpp"
#include "broker_installation.hpp"
#include "broker_runtime.hpp"
#include "broker_pipe.hpp"
#include "driver_identity.hpp"
#include <windows.h>
#include <aclapi.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <array>
#include <iostream>

namespace loopz::windows {
namespace {
struct ServiceState {
    SERVICE_STATUS_HANDLE handle{};
    SERVICE_STATUS status{};
    std::mutex mutex;
    std::atomic<bool> stopping{false};
    HANDLE stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    HANDLE finished{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    ~ServiceState() {
        if (stop)
            CloseHandle(stop);
        if (finished)
            CloseHandle(finished);
    }
    void publish(DWORD next, DWORD failure = 0) {
        std::lock_guard guard(mutex);
        if (!handle || status.dwCurrentState == SERVICE_STOPPED)
            return;
        if (stopping.load() && next != SERVICE_STOPPED)
            next = SERVICE_STOP_PENDING;
        status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        status.dwCurrentState = next;
        status.dwControlsAccepted =
            next == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
        status.dwWin32ExitCode = failure ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR;
        status.dwServiceSpecificExitCode = failure;
        const bool pending = next == SERVICE_START_PENDING || next == SERVICE_STOP_PENDING;
        status.dwCheckPoint = pending ? status.dwCheckPoint + 1 : 0;
        status.dwWaitHint = pending ? 30000 : 0;
        SetServiceStatus(handle, &status);
    }
    void heartbeat() {
        std::lock_guard guard(mutex);
        if (handle && (status.dwCurrentState == SERVICE_START_PENDING ||
                       status.dwCurrentState == SERVICE_STOP_PENDING)) {
            ++status.dwCheckPoint;
            SetServiceStatus(handle, &status);
        }
    }
    void interrogate() {
        std::lock_guard guard(mutex);
        if (handle && status.dwCurrentState != SERVICE_STOPPED)
            SetServiceStatus(handle, &status);
    }
};
// Static lifetime also covers any control callback racing with final status.
ServiceState state;
std::atomic<int> processExit{0};
void logFailure(std::string_view message) noexcept {
    const auto source = RegisterEventSourceW(nullptr, kDeviceBrokerServiceName);
    if (!source)
        return;
    std::array<wchar_t, 2048> text{};
    const auto bytes = message.substr(0, 1024);
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                           static_cast<int>(bytes.size()), text.data(),
                                           static_cast<int>(text.size() - 1));
    const wchar_t* strings[]{count > 0 ? text.data() : L"Microphone setup service failed"};
    ReportEventW(source, EVENTLOG_ERROR_TYPE, 0, 1, nullptr, 1, 0, strings, nullptr);
    DeregisterEventSource(source);
}
// Clients verify the kernel-reported pipe PID against SCM and the executable
// image. Grant only the query right needed for that check; default System
// process permissions otherwise deny ordinary users even limited queries.
void allowClientInspection() {
    PSECURITY_DESCRIPTOR descriptor{};
    PACL existing{};
    if (GetSecurityInfo(GetCurrentProcess(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                        nullptr, &existing, nullptr, &descriptor) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot inspect setup process permissions");
    struct Free {
        void* value;
        ~Free() {
            if (value)
                LocalFree(value);
        }
    } old{descriptor};
    if (!existing)
        throw std::runtime_error("Setup process requires explicit access permissions");
    alignas(DWORD) std::array<BYTE, SECURITY_MAX_SID_SIZE> authenticated{};
    DWORD size = static_cast<DWORD>(authenticated.size());
    if (!CreateWellKnownSid(WinAuthenticatedUserSid, nullptr, authenticated.data(), &size))
        throw std::runtime_error("Cannot identify authenticated setup clients");
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = PROCESS_QUERY_LIMITED_INFORMATION;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(authenticated.data());
    PACL updated{};
    if (SetEntriesInAclW(1, &access, existing, &updated) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot prepare setup client inspection permissions");
    Free changed{updated};
    if (SetSecurityInfo(GetCurrentProcess(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                        nullptr, updated, nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot enable setup server identity inspection");
}
DWORD WINAPI control(DWORD command, DWORD, void*, void*) noexcept {
    try {
        if (command == SERVICE_CONTROL_STOP || command == SERVICE_CONTROL_SHUTDOWN) {
            state.stopping.store(true);
            SetEvent(state.stop);
            state.publish(SERVICE_STOP_PENDING);
            return NO_ERROR;
        }
        if (command == SERVICE_CONTROL_INTERROGATE) {
            state.interrogate();
            return NO_ERROR;
        }
        return ERROR_CALL_NOT_IMPLEMENTED;
    } catch (...) {
        SetEvent(state.stop);
        return ERROR_EXCEPTION_IN_SERVICE;
    }
}
void WINAPI serviceMain(DWORD, LPWSTR*) noexcept {
    DWORD failure = 0;
    try {
        if (!state.stop || !state.finished)
            throw std::runtime_error("Cannot create service lifecycle events");
        state.handle = RegisterServiceCtrlHandlerExW(kDeviceBrokerServiceName, control, nullptr);
        if (!state.handle)
            throw std::runtime_error("Cannot register microphone setup service controls");
        state.publish(SERVICE_START_PENDING);
        std::jthread heartbeat([] {
            while (WaitForSingleObject(state.finished, 1000) == WAIT_TIMEOUT)
                state.heartbeat();
        });
        struct Finish {
            std::jthread& thread;
            ~Finish() {
                SetEvent(state.finished);
                if (thread.joinable())
                    thread.join();
            }
        } finish{heartbeat};
        DWORD session{};
        if (currentOwnerSid() != L"S-1-5-18" ||
            !ProcessIdToSessionId(GetCurrentProcessId(), &session) || session != 0)
            throw std::runtime_error("Microphone setup service requires LocalSystem in Session 0");
        BrokerBinaryGuard installedBinary;
        allowClientInspection();
        WindowsBrokerDeviceAdapter adapter(readBrokerInstallation());
        runBrokerRuntime(deviceIntentStoreRoot(), kDeviceBrokerPipe, adapter, state.stop, {}, [] {
            state.publish(SERVICE_RUNNING);
        });
    } catch (const std::exception& error) {
        failure = 1;
        processExit.store(1);
        logFailure(error.what());
    } catch (...) {
        failure = 2;
        processExit.store(1);
        logFailure("Unexpected microphone setup service failure");
    }
    // Runtime has joined device work and the heartbeat has stopped before SCM
    // receives STOPPED. No active PnP operation outlives this service lifecycle.
    state.publish(SERVICE_STOPPED, failure);
}
} // namespace
int dispatchBrokerService() {
    SERVICE_TABLE_ENTRYW table[]{{const_cast<LPWSTR>(kDeviceBrokerServiceName), serviceMain},
                                 {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
        std::cerr << "Start microphone setup through the Windows service manager (Windows error "
                  << GetLastError() << ")\n";
        return 1;
    }
    return processExit.load();
}
} // namespace loopz::windows
int wmain(int argc, wchar_t**) {
    if (argc != 1)
        return 1; // No command-line path, package or owner overrides.
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                                  LOAD_LIBRARY_SEARCH_SYSTEM32))
        return 1;
    return loopz::windows::dispatchBrokerService();
}
