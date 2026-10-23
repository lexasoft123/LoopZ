#include "automatic_render_probe.hpp"
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace loopz::windows::probe {
namespace {
using Microsoft::WRL::ComPtr;
void checked(HRESULT result, const char* operation) {
    if (FAILED(result))
        throw std::runtime_error(operation);
}
class Activation final : public Microsoft::WRL::RuntimeClass<
                             Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                             IActivateAudioInterfaceCompletionHandler, Microsoft::WRL::FtmBase> {
  public:
    HANDLE event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    HRESULT result{E_PENDING};
    ComPtr<IAudioClient> client;
    ~Activation() {
        if (event)
            CloseHandle(event);
    }
    HRESULT STDMETHODCALLTYPE
    ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        ComPtr<IUnknown> unknown;
        HRESULT activated = E_FAIL;
        result = op->GetActivateResult(&activated, &unknown);
        if (SUCCEEDED(result))
            result = activated;
        if (SUCCEEDED(result))
            result = unknown.As(&client);
        SetEvent(event);
        return S_OK;
    }
};
struct Event {
    HANDLE value{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    ~Event() {
        if (value)
            CloseHandle(value);
    }
};
} // namespace

static int automaticRender(unsigned seconds, bool (*allowed)(const void*), const void* context,
                           bool (*finished)(const void*) = nullptr,
                           const void* finishContext = nullptr) {
    if (!seconds || seconds > 60)
        throw std::invalid_argument("Automatic silent probe duration must be 1..60 seconds");
    auto activation = Microsoft::WRL::Make<Activation>();
    if (!activation || !activation->event)
        throw std::runtime_error("Cannot prepare automatic render activation");
    wchar_t identifier[40]{};
    if (!StringFromGUID2(DEVINTERFACE_AUDIO_RENDER, identifier, 40))
        throw std::runtime_error("Cannot encode automatic render interface");
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    checked(ActivateAudioInterfaceAsync(identifier, __uuidof(IAudioClient), nullptr,
                                        activation.Get(), &operation),
            "Cannot activate automatic render stream");
    // The async API owns the completion object through callback completion,
    // including if this wait times out. No callback points into a caller stack.
    if (WaitForSingleObject(activation->event, 10000) != WAIT_OBJECT_0)
        throw std::runtime_error("Automatic render activation timed out");
    checked(activation->result, "Automatic render activation failed");
    const auto client = activation->client;
    struct Format {
        WAVEFORMATEX* value{};
        ~Format() {
            CoTaskMemFree(value);
        }
    } format;
    checked(client->GetMixFormat(&format.value), "Cannot read automatic render format");
    if (allowed) {
        const auto* wave = format.value;
        const bool floating =
            wave && (wave->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                     (wave->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                      wave->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
                      reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wave)->SubFormat ==
                          KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
        if (!floating || wave->wBitsPerSample != 32 || wave->nSamplesPerSec != 48000 ||
            wave->nChannels != 2 || wave->nBlockAlign != 8)
            throw std::runtime_error("Virtual automatic marker requires float32 48 kHz stereo");
    }
    checked(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 1000000,
                               0, format.value, nullptr),
            "Cannot initialize automatic render stream");
    Event event;
    if (!event.value)
        throw std::runtime_error("Cannot create automatic render event");
    checked(client->SetEventHandle(event.value), "Cannot set automatic render event");
    UINT32 capacity{};
    checked(client->GetBufferSize(&capacity), "Cannot read automatic render capacity");
    ComPtr<IAudioRenderClient> renderer;
    checked(client->GetService(IID_PPV_ARGS(&renderer)), "Cannot obtain automatic renderer");
    bool started = false;
    uint64_t position = 0;
    const auto fill = [&] {
        UINT32 padding{};
        checked(client->GetCurrentPadding(&padding), "Cannot read automatic render padding");
        if (padding > capacity)
            throw std::runtime_error("Invalid automatic render padding");
        if (const auto frames = capacity - padding) {
            // The first buffer is always silent. Endpoint validation happens on
            // this test/control thread, never in a production audio callback.
            const bool tone = allowed && started && allowed(context);
            BYTE* bytes{};
            checked(renderer->GetBuffer(frames, &bytes), "Cannot obtain automatic render buffer");
            if (tone) {
                auto* samples = reinterpret_cast<float*>(bytes);
                for (uint32_t frame = 0; frame < frames; ++frame) {
                    const auto value =
                        float(.03 * std::sin(6.283185307179586 * 440 *
                                             static_cast<double>(position + frame) / 48000));
                    samples[frame * 2] = samples[frame * 2 + 1] = value;
                }
            }
            position += frames;
            checked(renderer->ReleaseBuffer(frames, tone ? 0 : AUDCLNT_BUFFERFLAGS_SILENT),
                    "Cannot submit automatic silent buffer");
        }
    };
    fill();
    checked(client->Start(), "Cannot start automatic render stream");
    started = true;
    struct Stop {
        IAudioClient* client;
        ~Stop() {
            client->Stop();
            client->Reset();
        }
    } stop{client.Get()};
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end && (!finished || !finished(finishContext))) {
        const auto wait = WaitForSingleObject(event.value, 100);
        if (wait == WAIT_OBJECT_0)
            fill();
        else if (wait != WAIT_TIMEOUT)
            throw std::runtime_error("Automatic render event failed");
    }
    return 0;
}
int automaticSilence(unsigned seconds) {
    return automaticRender(seconds, nullptr, nullptr);
}
int automaticSilenceUntil(unsigned seconds, bool (*finished)(const void*), const void* context) {
    if (!finished || !context)
        throw std::invalid_argument("Controlled silent marker requires a completion predicate");
    return automaticRender(seconds, nullptr, nullptr, finished, context);
}
int automaticVirtualTone(unsigned seconds, bool (*allowed)(const void*), const void* context) {
    if (!allowed || !context)
        throw std::invalid_argument("Virtual automatic marker requires an endpoint guard");
    return automaticRender(seconds, allowed, context);
}
} // namespace loopz::windows::probe
