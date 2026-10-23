#include "endpoint_format.hpp"
#include "endpoint_roles.hpp"
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioendpoints.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl.h>
#include <chrono>
#include <thread>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <set>

namespace loopz::windows {
namespace {
using Microsoft::WRL::ComPtr;
void checked(HRESULT value, const char* operation) {
    if (SUCCEEDED(value))
        return;
    std::ostringstream text;
    text << operation << " (0x" << std::hex << static_cast<unsigned long>(value) << ")";
    throw std::runtime_error(text.str());
}
std::wstring wide(const std::string& value) {
    if (value.empty() || value.size() > 32768)
        throw std::runtime_error("Invalid virtual endpoint identity length");
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0);
    if (!count)
        throw std::runtime_error("Invalid virtual endpoint identity");
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), count) != count)
        throw std::runtime_error("Cannot decode virtual endpoint identity");
    return result;
}
// Known Windows 7+ IPolicyConfig IID, corroborated by the pinned EarTrumpet
// interface (the first eight slots precede GetPropertyValue) and the explicit
// format ABI in AudioDeviceCmdlets/SOURCE/IPolicyConfig.cs. Only this setter is
// exposed: no default-device, visibility, or arbitrary property operation.
struct EndpointPolicy : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE unusedMixFormat() = 0;
    virtual HRESULT STDMETHODCALLTYPE unusedDeviceFormat() = 0;
    virtual HRESULT STDMETHODCALLTYPE unusedResetFormat() = 0;
    virtual HRESULT STDMETHODCALLTYPE setFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
};
bool disappeared(HRESULT result) {
    return result == AUDCLNT_E_DEVICE_INVALIDATED || result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}
HRESULT selectFormat(IMMDevice* device, PCWSTR id, loopz_driver::AudioFormat expected,
                     const char* role) {
    WAVEFORMATEXTENSIBLE native{};
    native.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    native.Format.nChannels = static_cast<WORD>(expected.channels);
    native.Format.nSamplesPerSec = expected.sampleRate;
    native.Format.wBitsPerSample = 32;
    native.Format.nBlockAlign = static_cast<WORD>(expected.channels * 4);
    native.Format.nAvgBytesPerSec = expected.sampleRate * native.Format.nBlockAlign;
    native.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    native.Samples.wValidBitsPerSample = 32;
    native.dwChannelMask =
        expected.channels == 2 ? KSAUDIO_SPEAKER_STEREO : KSAUDIO_SPEAKER_DIRECTOUT;
    native.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    ComPtr<IAudioClient> client;
    auto result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (disappeared(result))
        return result;
    checked(result, "Check requested endpoint format");
    result = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &native.Format, nullptr);
    if (result == AUDCLNT_E_UNSUPPORTED_FORMAT) {
        // Older LoopZ packages expose only PCM16. Fall back only on an explicit
        // format rejection; device loss and other failures must remain visible.
        native.Format.wBitsPerSample = 16;
        native.Format.nBlockAlign = static_cast<WORD>(expected.frameBytes());
        native.Format.nAvgBytesPerSec = expected.sampleRate * native.Format.nBlockAlign;
        native.Samples.wValidBitsPerSample = 16;
        native.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        result = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &native.Format, nullptr);
    }
    if (disappeared(result))
        return result;
    checked(result, "Driver does not accept requested endpoint format");
    constexpr GUID clsid{
        0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
    constexpr GUID iid{
        0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};
    ComPtr<EndpointPolicy> policy;
    checked(CoCreateInstance(clsid, nullptr, CLSCTX_ALL, iid,
                             reinterpret_cast<void**>(policy.GetAddressOf())),
            "Open endpoint format selection");
    // Let Windows derive its internal mix format from the selected hardware
    // format. The third parameter is left null by the established policy ABI;
    // forcing our own float format can impose additional service-side limits.
    const auto selected = policy->setFormat(id, &native.Format, nullptr);
    if (disappeared(selected))
        return selected;
    if (FAILED(selected)) {
        // Keep driver acceptance distinct from the shared audio engine's
        // acceptance. These read-only queries make setup failures actionable
        // without changing defaults or trying unrelated endpoint settings.
        WAVEFORMATEX* closest{};
        const auto shared =
            client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &native.Format, &closest);
        CoTaskMemFree(closest);
        WAVEFORMATEX* current{};
        const auto mixed = client->GetMixFormat(&current);
        if (disappeared(shared) || disappeared(mixed)) {
            CoTaskMemFree(current);
            return AUDCLNT_E_DEVICE_INVALIDATED;
        }
        std::ostringstream detail;
        detail << "Select requested " << role << " format " << expected.channels << " channels at "
               << expected.sampleRate << " Hz"
               << "; driver accepted; shared query=0x" << std::hex
               << static_cast<unsigned long>(shared) << "; current mix query=0x"
               << static_cast<unsigned long>(mixed);
        if (SUCCEEDED(mixed) && current)
            detail << std::dec << "; current mix=" << current->nChannels << " channels at "
                   << current->nSamplesPerSec << " Hz";
        CoTaskMemFree(current);
        checked(selected, detail.str().c_str());
    }
    return S_OK;
}
bool matches(IMMDevice* device, loopz_driver::AudioFormat expected) {
    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)))
        return false;
    WAVEFORMATEX* format{};
    const auto result = client->GetMixFormat(&format);
    const bool match = SUCCEEDED(result) && format &&
                       format->nSamplesPerSec == expected.sampleRate &&
                       format->nChannels == expected.channels;
    CoTaskMemFree(format);
    return match;
}
} // namespace
namespace {
void ensureAdapterEndpointFormat(const std::string& key, loopz_driver::AudioFormat format,
                                 bool sink) {
    if (!format.valid() || key.size() != 64 ||
        key.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("Invalid microphone format request");
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        checked(initialized, "Initialize endpoint format setup");
    struct Apartment {
        bool owned;
        ~Apartment() {
            if (owned)
                CoUninitialize();
        }
    } apartment{SUCCEEDED(initialized)};
    std::set<std::string> resetRequested;
    std::string stableFeed, stableMicrophone;
    unsigned stablePasses = 0;
    HRESULT last = S_OK;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        std::vector<std::string> feeds, microphones;
        for (const auto& [uid, endpoint] : loopzEndpoints()) {
            if (endpoint.mixKey == key) {
                if (endpoint.role == EndpointRole::Feed)
                    feeds.push_back(uid);
                if (endpoint.role == EndpointRole::Microphone)
                    microphones.push_back(uid);
            }
        }
        if (sink) {
            for (const auto& [uid, endpoint] : loopzEndpoints(true))
                if (endpoint.mixKey == key && endpoint.role == EndpointRole::Microphone)
                    throw std::runtime_error("App routing sink unexpectedly exposes a microphone");
        }
        if (feeds.size() == 1 && microphones.size() == (sink ? 0U : 1U)) {
            // PnP can replace endpoint IDs during setup. Discover and activate
            // through fresh enumerators; never keep retrying a vanished ID.
            ComPtr<IMMDeviceEnumerator> enumerator;
            checked(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                     IID_PPV_ARGS(&enumerator)),
                    "Open endpoint format setup");
            bool complete = true;
            auto expected = feeds;
            if (!sink)
                expected.push_back(microphones.front());
            for (const auto& uid : expected) {
                const auto id = wide(uid);
                ComPtr<IMMDevice> device;
                last = enumerator->GetDevice(id.c_str(), &device);
                if (last == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ||
                    last == AUDCLNT_E_DEVICE_INVALIDATED) {
                    complete = false;
                    break;
                }
                checked(last, "Find endpoint for format setup");
                if (matches(device.Get(), format))
                    continue;
                complete = false;
                if (resetRequested.contains(uid))
                    continue;
                ComPtr<IAudioEndpointFormatControl> control;
                last = device->Activate(__uuidof(IAudioEndpointFormatControl), CLSCTX_ALL, nullptr,
                                        &control);
                if (last == AUDCLNT_E_DEVICE_INVALIDATED ||
                    last == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
                    break;
                checked(last, "Open virtual endpoint format control");
                last = control->ResetToDefault(0);
                if (last == AUDCLNT_E_DEVICE_INVALIDATED ||
                    last == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
                    break;
                // Windows may retain an old OEM default even after the driver's
                // capabilities change. Selecting the explicitly requested format
                // is distinct from restoring that obsolete factory selection.
                if (last == AUDCLNT_E_UNSUPPORTED_FORMAT) {
                    last = selectFormat(device.Get(), id.c_str(), format,
                                        uid == feeds.front() ? "feed" : "microphone");
                    if (disappeared(last))
                        break;
                } else
                    checked(last, "Refresh virtual endpoint format");
                resetRequested.insert(uid);
            }
            if (complete) {
                if (stableFeed == feeds.front() &&
                    stableMicrophone == (sink ? std::string{} : microphones.front()))
                    ++stablePasses;
                else {
                    stableFeed = feeds.front();
                    stableMicrophone = sink ? std::string{} : microphones.front();
                    stablePasses = 1;
                }
                // EndpointBuilder may briefly expose the requested format and
                // invalidate it again while processing the property change.
                // Require fresh, matching queries over three separate polls.
                if (stablePasses >= 3)
                    return;
            } else
                stablePasses = 0;
        } else
            stablePasses = 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (FAILED(last))
        checked(last, "Wait for microphone format setup");
    throw std::runtime_error("Windows has not adopted the requested microphone format");
}
} // namespace
void ensureMixEndpointFormat(const std::string& key, loopz_driver::AudioFormat format) {
    ensureAdapterEndpointFormat(key, format, false);
}
void ensureAppRoutingSinkEndpointFormat(const std::string& key) {
    ensureAdapterEndpointFormat(key, {48000, 2}, true);
}
} // namespace loopz::windows
