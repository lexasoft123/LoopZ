#include "app_routing_sessions.hpp"
#include "app_routing_sink_lifetime.hpp"
#include <windows.h>
#include <objbase.h>
#include <string_view>
#include <stdexcept>
#include <string>
using namespace loopz::windows;
namespace {
void require(bool value, const char* error) {
    if (!value)
        throw std::runtime_error(error);
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--exit")
        return 0;
    AppRoutingSession session;
    session.pid = GetCurrentProcessId();
    require(appRoutingSessionMayOwnStream(session), "Live inactive client was released");
    require(!appRoutingSessionMayOwnStream(session, true),
            "Known absent local render clients retained their inactive observer session");
    session.active = true;
    require(appRoutingSessionMayOwnStream(session, true), "Active local stream was released");
    session.active = false;
    session.shared = true;
    require(appRoutingSessionMayOwnStream(session, true), "Shared local creator was released");
    session.shared = false;
    session.systemSounds = true;
    require(!appRoutingSessionMayOwnStream(session), "Reserved inactive system session retained");
    session.active = true;
    require(appRoutingSessionMayOwnStream(session), "Active system stream released");
    session = {};
    session.shared = true;
    require(appRoutingSessionMayOwnStream(session), "Ambiguous inactive shared client released");
    session = {};
    require(appRoutingSessionMayOwnStream(session), "Unknown inactive client released");
    wchar_t module[32768]{};
    require(GetModuleFileNameW(nullptr, module, 32768) != 0, "Locate test image");
    auto command = L"\"" + std::wstring(module) + L"\" --exit";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    require(CreateProcessW(module, command.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup,
                           &child) != FALSE,
            "Start liveness child");
    session.pid = child.dwProcessId;
    const bool liveRetained = appRoutingSessionMayOwnStream(session, true);
    const auto resumed = ResumeThread(child.hThread);
    CloseHandle(child.hThread);
    const auto waited = WaitForSingleObject(child.hProcess, 10000);
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(child.hProcess, 1);
        WaitForSingleObject(child.hProcess, 5000);
    }
    session.pid = child.dwProcessId;
    session.active = true;
    require(appRoutingSessionMayOwnStream(session, true), "Foreign active stream was released");
    session.active = false;
    const bool retained = appRoutingSessionMayOwnStream(session);
    CloseHandle(child.hProcess);
    require(resumed != DWORD(-1) && liveRetained,
            "Known absent local clients released a live foreign inactive process");
    require(waited == WAIT_OBJECT_0 && !retained, "Proven-dead inactive client retained");
    require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "Initialize lifetime MTA");
    const auto absent =
        observeAppRoutingSink(L"S-1-5-21-1-2-3-" + std::to_wstring(GetCurrentProcessId()));
    CoUninitialize();
    require(absent.endpoint.empty() && !absent.endpointsExamined && !absent.defaultsExamined,
            "Missing sink identity authorized release");
    return 0;
}
