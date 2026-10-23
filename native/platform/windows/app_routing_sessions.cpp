#include "app_routing_sessions.hpp"
#include <windows.h>
#include <audiopolicy.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl.h>
#include <stdexcept>

namespace loopz::windows {
namespace {
// Microsoft's GetProcessId documentation defines this success code explicitly;
// Windows 10 SDK headers do not consistently expose the symbolic constant.
constexpr HRESULT noSingleProcess = static_cast<HRESULT>((0x889U << 16U) | 0x00dU);
void require(HRESULT value) {
    if (FAILED(value))
        throw std::runtime_error("Cannot examine application audio sessions; recovery retained");
}
std::string endpointText(std::wstring_view text) {
    const auto count =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0)
        throw std::runtime_error("Invalid application audio endpoint identity");
    std::string result(static_cast<size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), count, nullptr,
                            nullptr) != count)
        throw std::runtime_error("Cannot encode application audio endpoint identity");
    return result;
}
} // namespace
bool appRoutingSessionMayOwnStream(const AppRoutingSession& session,
                                   bool localRenderClientsAbsent) {
    if (session.active)
        return true;
    if (session.systemSounds)
        return false;
    if (session.shared || !session.pid)
        return true; // An initial creator cannot prove all shared clients have exited.
    if (localRenderClientsAbsent && session.pid == GetCurrentProcessId())
        return false;
    HANDLE process =
        OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, session.pid);
    if (!process)
        return GetLastError() != ERROR_INVALID_PARAMETER;
    const auto state = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return state != WAIT_OBJECT_0;
}
std::vector<AppRoutingSession> appRoutingSessions() {
    using Microsoft::WRL::ComPtr;
    ComPtr<IMMDeviceEnumerator> enumerator;
    require(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                             IID_PPV_ARGS(&enumerator)));
    ComPtr<IMMDeviceCollection> endpoints;
    require(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &endpoints));
    UINT count{};
    require(endpoints->GetCount(&count));
    if (count > 4096)
        throw std::runtime_error("Too many application audio endpoints");
    std::vector<AppRoutingSession> result;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> endpoint;
        require(endpoints->Item(i, &endpoint));
        LPWSTR raw{};
        require(endpoint->GetId(&raw));
        struct Id {
            LPWSTR value;
            ~Id() {
                CoTaskMemFree(value);
            }
        } id{raw};
        if (!raw)
            throw std::runtime_error("Missing application audio endpoint identity");
        const auto uid = endpointText(raw);
        ComPtr<IAudioSessionManager2> manager;
        require(endpoint->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &manager));
        ComPtr<IAudioSessionEnumerator> sessions;
        require(manager->GetSessionEnumerator(&sessions));
        int n{};
        require(sessions->GetCount(&n));
        if (n < 0 || n > 4096)
            throw std::runtime_error("Too many application audio sessions");
        for (int j = 0; j < n; ++j) {
            ComPtr<IAudioSessionControl> session;
            require(sessions->GetSession(j, &session));
            AudioSessionState state{};
            require(session->GetState(&state));
            if (state == AudioSessionStateExpired)
                continue;
            if (state != AudioSessionStateActive && state != AudioSessionStateInactive)
                throw std::runtime_error("Invalid application audio session state");
            ComPtr<IAudioSessionControl2> detail;
            require(session.As(&detail));
            DWORD pid{};
            const auto owner = detail->GetProcessId(&pid);
            require(owner);
            // GetProcessId supplies only the initial creator for a shared
            // session. Retain its endpoint evidence, but never treat that PID
            // as authorization to mutate every process sharing the session.
            const auto creator = pid;
            const bool shared = owner == noSingleProcess;
            const auto system = detail->IsSystemSoundsSession();
            require(system);
            if (system != S_OK && system != S_FALSE)
                throw std::runtime_error("Unknown system audio session classification");
            if (shared)
                pid = 0;
            else if (owner != S_OK)
                throw std::runtime_error("Unknown audio session owner; recovery retained");
            result.push_back(
                {pid, uid, state == AudioSessionStateActive, creator, shared, system == S_OK});
        }
    }
    return result;
}
} // namespace loopz::windows
