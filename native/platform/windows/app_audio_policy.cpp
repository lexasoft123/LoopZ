// Audio-policy ABI/encoding adapted from EarTrumpet 2.3.0.0 (MIT).
// See third_party/eartrumpet-policy/{LICENSE,PROVENANCE.json}.
#include "app_audio_policy.hpp"
#include <windows.h>
#include <mmdeviceapi.h>
#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>
#include <wrl.h>
#include <sstream>
#include <string_view>
#include <stdexcept>

namespace loopz::windows {
namespace {
using Microsoft::WRL::ComPtr;
// Both known interface revisions have these 19 preceding slots. They are never
// called; interface negotiation selects only a known IID with this exact layout.
struct __declspec(novtable) AudioPolicy : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE unused00() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused01() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused02() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused03() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused04() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused05() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused06() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused07() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused08() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused09() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused10() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused11() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused12() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused13() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused14() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused15() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused16() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused17() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused18() = 0;
    virtual HRESULT STDMETHODCALLTYPE setOutput(UINT32, EDataFlow, ERole, HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE getOutput(UINT32, EDataFlow, ERole, HSTRING*) = 0;
};
constexpr GUID revisions[]{
    {0xab3d4648, 0xe242, 0x459f, {0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24}},
    {0x2a59116d, 0x6c4f, 0x45e0, {0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58}}};
struct String {
    HSTRING value{};
    ~String() {
        WindowsDeleteString(value);
    }
    HRESULT create(std::wstring_view text) {
        if (text.size() > 32768)
            return E_INVALIDARG;
        return WindowsCreateString(text.data(), static_cast<UINT32>(text.size()), &value);
    }
};
bool checked(HRESULT result, const char* action, std::string& error) {
    if (SUCCEEDED(result))
        return true;
    std::ostringstream message;
    message << action << " (0x" << std::hex << static_cast<unsigned long>(result) << ")";
    error = message.str();
    return false;
}
} // namespace
struct AppAudioPolicy::Impl {
    ComPtr<AudioPolicy> policy;
    bool initialized = false;
    std::string failure;
    Impl() {
        const auto initializedResult = RoInitialize(RO_INIT_MULTITHREADED);
        if (!checked(initializedResult, "Initialize app audio routing", failure))
            return;
        initialized = true;
        String name;
        if (!checked(name.create(L"Windows.Media.Internal.AudioPolicyConfig"),
                     "Create audio policy identifier", failure))
            return;
        HRESULT result = E_NOINTERFACE;
        for (const auto& revision : revisions) {
            result = RoGetActivationFactory(name.value, revision,
                                            reinterpret_cast<void**>(policy.GetAddressOf()));
            if (SUCCEEDED(result) && policy)
                return;
            policy.Reset();
        }
        checked(result, "Activate app audio routing", failure);
        if (failure.empty())
            failure = "Audio policy returned no factory";
    }
    ~Impl() {
        policy.Reset();
        if (initialized)
            RoUninitialize();
    }
};
AppAudioPolicy::AppAudioPolicy() : impl_(std::make_unique<Impl>()) {}
AppAudioPolicy::~AppAudioPolicy() = default;
bool AppAudioPolicy::available() const noexcept {
    return impl_->policy != nullptr;
}
std::string AppAudioPolicy::error() const {
    return impl_->failure;
}
bool AppAudioPolicy::get(uint32_t pid, unsigned role, std::wstring& value, std::string& error) {
    value.clear();
    if (!available() || !pid || role > 2) {
        error = "App audio routing is unavailable or invalid";
        return false;
    }
    String text;
    const auto result =
        impl_->policy->getOutput(pid, eRender, static_cast<ERole>(role), &text.value);
    if (result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        return true;
    if (!checked(result, "Read app output preference", error))
        return false;
    UINT32 length{};
    const auto* data = WindowsGetStringRawBuffer(text.value, &length);
    if (length > 32768 || (!data && length)) {
        error = "Invalid app output preference";
        return false;
    }
    if (length)
        value.assign(data, length);
    return true;
}
bool AppAudioPolicy::set(uint32_t pid, unsigned role, const std::wstring& value,
                         std::string& error) {
    if (!available() || !pid || role > 2) {
        error = "App audio routing is unavailable or invalid";
        return false;
    }
    String text;
    return checked(text.create(value), "Encode app output preference", error) &&
           checked(impl_->policy->setOutput(pid, eRender, static_cast<ERole>(role), text.value),
                   "Set app output preference", error);
}
std::wstring AppAudioPolicy::renderInterface(const std::string& uid) {
    if (uid.empty() || uid.size() > 32768)
        throw std::invalid_argument("Invalid playback endpoint identity");
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, uid.data(),
                                           static_cast<int>(uid.size()), nullptr, 0);
    if (!count)
        throw std::invalid_argument("Invalid playback endpoint encoding");
    std::wstring wide(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, uid.data(), static_cast<int>(uid.size()),
                        wide.data(), count);
    return L"\\\\?\\SWD#MMDEVAPI#" + wide + L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
}
} // namespace loopz::windows
