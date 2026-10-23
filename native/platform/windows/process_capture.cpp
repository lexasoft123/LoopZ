#include "process_capture.hpp"

#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace loopz::windows {
namespace {
using Microsoft::WRL::ComPtr;

std::string failure(const char* operation, HRESULT hr) {
    char buffer[160]{};
    std::snprintf(buffer, sizeof(buffer), "%s (0x%08lx)", operation,
                  static_cast<unsigned long>(hr));
    return buffer;
}

// The async API retains this object through completion. Params and the event
// belong to it too, including when the caller times out and drops its ref.
class Activation final : public Microsoft::WRL::RuntimeClass<
                             Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                             IActivateAudioInterfaceCompletionHandler, Microsoft::WRL::FtmBase> {
  public:
    Activation() : event(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
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
    HANDLE event;
    HRESULT result{E_PENDING};
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    PROPVARIANT blob{};
    ComPtr<IAudioClient> client;
};
} // namespace

struct ProcessCapture::Impl {
    std::shared_ptr<AudioHistory> history = std::make_shared<AudioHistory>(2);
    std::thread worker;
    HANDLE stopEvent{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    mutable std::mutex mutex;
    std::condition_variable changed;
    bool ready{false}, started{false};
    std::atomic<uint64_t> lastFrames{0}, lastFlags{0}, positionDelta{0}, qpcDelta{0};
    std::string error;

    ~Impl() {
        if (stopEvent)
            CloseHandle(stopEvent);
    }
    void publish(bool ok, std::string message = {}) {
        {
            std::lock_guard lock(mutex);
            started = ok;
            error = std::move(message);
            ready = true;
        }
        changed.notify_all();
    }

    void run(uint32_t pid) {
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(apartment)) {
            publish(false, failure("Process capture COM initialization failed", apartment));
            return;
        }
        struct ApartmentScope {
            ~ApartmentScope() {
                CoUninitialize();
            }
        } apartmentScope;
        {
            const auto execute = [&] {
                auto activation = Microsoft::WRL::Make<Activation>();
                if (!activation || !activation->event) {
                    publish(false, "Cannot allocate process capture activation");
                    return;
                }
                activation->params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
                activation->params.ProcessLoopbackParams.TargetProcessId = pid;
                activation->params.ProcessLoopbackParams.ProcessLoopbackMode =
                    PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
                activation->blob.vt = VT_BLOB;
                activation->blob.blob.cbSize = sizeof(activation->params);
                activation->blob.blob.pBlobData = reinterpret_cast<BYTE*>(&activation->params);
                ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
                HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                                         __uuidof(IAudioClient), &activation->blob,
                                                         activation.Get(), &operation);
                if (FAILED(hr)) {
                    publish(false, failure("Selected-app capture is unavailable", hr));
                    return;
                }
                HANDLE events[]{stopEvent, activation->event};
                if (WaitForMultipleObjects(2, events, FALSE, 10000) != WAIT_OBJECT_0 + 1) {
                    publish(false, "Selected-app capture activation timed out or was cancelled");
                    return;
                }
                if (FAILED(activation->result)) {
                    publish(false,
                            failure("Selected-app capture activation failed", activation->result));
                    return;
                }
                auto client = activation->client;
                WAVEFORMATEX format{};
                format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
                format.nChannels = 2;
                format.nSamplesPerSec = 48000;
                format.wBitsPerSample = 32;
                format.nBlockAlign = 8;
                format.nAvgBytesPerSec = 384000;
                hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                        AUDCLNT_STREAMFLAGS_LOOPBACK |
                                            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                        200000, 0, &format, nullptr);
                if (FAILED(hr)) {
                    publish(false, failure("Selected-app capture initialization failed", hr));
                    return;
                }
                ComPtr<IAudioCaptureClient> capture;
                hr = client->GetService(IID_PPV_ARGS(&capture));
                if (FAILED(hr)) {
                    publish(false, failure("Selected-app capture service failed", hr));
                    return;
                }
                // The virtual process-loopback client need not implement endpoint
                // buffer-size queries. Bound our own packet storage instead.
                constexpr UINT32 capacity = 96000;
                std::array<std::vector<float>, 2> planes{std::vector<float>(capacity),
                                                         std::vector<float>(capacity)};
                const std::array<const float*, 2> pointers{planes[0].data(), planes[1].data()};
                hr = client->Start();
                if (FAILED(hr)) {
                    publish(false, failure("Selected-app capture start failed", hr));
                    return;
                }
                publish(true);
                DWORD task = 0;
                HANDLE priority = AvSetMmThreadCharacteristicsW(L"Audio", &task);
                bool running = true;
                uint64_t previousPosition = 0, previousQpc = 0;
                while (running && WaitForSingleObject(stopEvent, 3) == WAIT_TIMEOUT) {
                    for (;;) {
                        UINT32 available = 0;
                        hr = capture->GetNextPacketSize(&available);
                        if (FAILED(hr) || !available)
                            break;
                        BYTE* bytes = nullptr;
                        UINT32 frames = 0;
                        DWORD flags = 0;
                        UINT64 position = 0, qpc100ns = 0;
                        hr = capture->GetBuffer(&bytes, &frames, &flags, &position, &qpc100ns);
                        if (FAILED(hr))
                            break;
                        lastFrames.store(frames);
                        lastFlags.store(flags);
                        positionDelta.store(position - previousPosition);
                        qpcDelta.store(qpc100ns - previousQpc);
                        previousPosition = position;
                        previousQpc = qpc100ns;
                        const bool valid = frames && frames <= capacity &&
                                           (bytes || (flags & AUDCLNT_BUFFERFLAGS_SILENT)) &&
                                           !(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) &&
                                           qpc100ns && qpc100ns <= UINT64_MAX / 100;
                        if (valid) {
                            const auto* samples = reinterpret_cast<const float*>(bytes);
                            for (uint32_t frame = 0; frame < frames; ++frame) {
                                planes[0][frame] =
                                    flags & AUDCLNT_BUFFERFLAGS_SILENT ? 0 : samples[frame * 2];
                                planes[1][frame] =
                                    flags & AUDCLNT_BUFFERFLAGS_SILENT ? 0 : samples[frame * 2 + 1];
                            }
                            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
                                history->stop();
                            // Windows 10's virtual process client can return a
                            // stationary device position despite advancing QPC
                            // timestamps. It is not a hardware sample counter.
                            // Preserve packet timing and explicit gap flags without
                            // asserting source-frame continuity from that value.
                            (void)history->append({pointers, frames}, frames, qpc100ns * 100,
                                                  48000);
                        }
                        hr = capture->ReleaseBuffer(frames);
                        if (FAILED(hr))
                            break;
                    }
                    if (FAILED(hr)) {
                        publish(false, failure("Selected-app capture was interrupted", hr));
                        running = false;
                    }
                }
                (void)client->Stop();
                if (priority)
                    AvRevertMmThreadCharacteristics(priority);
            };
            execute();
        }
        history->stop();
    }
};

ProcessCapture::ProcessCapture() : impl_(std::make_unique<Impl>()) {}
ProcessCapture::~ProcessCapture() {
    stop();
}
bool ProcessCapture::start(uint32_t pid, std::string& error) {
    stop();
    if (!pid || !impl_->stopEvent) {
        error = "Selected-app capture needs a process and a stop event";
        return false;
    }
    ResetEvent(impl_->stopEvent);
    impl_->ready = false;
    impl_->started = false;
    impl_->error.clear();
    impl_->worker = std::thread([this, pid] {
        try {
            impl_->run(pid);
        } catch (const std::exception& e) {
            impl_->publish(false, e.what());
        }
    });
    std::unique_lock lock(impl_->mutex);
    impl_->changed.wait(lock, [this] {
        return impl_->ready;
    });
    error = impl_->error;
    return impl_->started;
}
void ProcessCapture::stop() {
    if (impl_->stopEvent)
        SetEvent(impl_->stopEvent);
    if (impl_->worker.joinable())
        impl_->worker.join();
    impl_->history->stop();
}
std::shared_ptr<AudioHistory> ProcessCapture::history() const {
    return impl_->history;
}
std::string ProcessCapture::error() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->error;
}
ProcessCapture::PacketStatus ProcessCapture::packetStatus() const noexcept {
    return {impl_->lastFrames.load(), impl_->lastFlags.load(), impl_->positionDelta.load(),
            impl_->qpcDelta.load()};
}
} // namespace loopz::windows
