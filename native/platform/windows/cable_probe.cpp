#include "endpoint_roles.hpp"
#include "driver_transport.hpp"
#include "probe_recording_limits.hpp"
#include "probe_streaming_capture.hpp"
#include <windows.h>
#include <realtimeapiset.h>
#include <objbase.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl.h>
// Opt-in loaded-driver acceptance. Generates quiet markers only in LoopZ Feed.
#include <zcore/device/audio_host.h>
#include <zcore/device/audio_input.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cmath>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <thread>
#include <vector>

namespace {
using JSON = nlohmann::json;
constexpr double tau = 6.283185307179586;
uint64_t qpcTicks() noexcept {
    LARGE_INTEGER ticks{};
    QueryPerformanceCounter(&ticks);
    return static_cast<uint64_t>(ticks.QuadPart);
}
JSON diagnoseSharedOpen(const std::string& uid, unsigned rate, unsigned channels) {
    using Microsoft::WRL::ComPtr;
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized))
        return {{"apartmentError", static_cast<unsigned long>(initialized)}};
    struct Apartment {
        ~Apartment() {
            CoUninitialize();
        }
    } apartment;
    const auto hex = [](HRESULT result) {
        std::ostringstream text;
        text << "0x" << std::hex << static_cast<unsigned long>(result);
        return text.str();
    };
    std::string raw = uid.starts_with("wasapi:") ? uid.substr(7) : uid;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(),
                                          static_cast<int>(raw.size()), nullptr, 0);
    if (count <= 0)
        throw std::runtime_error("Invalid diagnostic endpoint identity");
    std::wstring id(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()),
                        id.data(), count);
    ComPtr<IMMDeviceEnumerator> enumerator;
    auto result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(&enumerator));
    if (FAILED(result))
        return {{"enumerator", hex(result)}};
    ComPtr<IMMDevice> device;
    result = enumerator->GetDevice(id.c_str(), &device);
    if (FAILED(result))
        return {{"device", hex(result)}};
    JSON attempts = JSON::array();
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        const bool exclusive = attempt == 4;
        const bool client3Attempt = attempt == 5 || attempt == 6;
        const bool eventDriven = attempt == 1 || exclusive || client3Attempt;
        ComPtr<IAudioClient> client;
        result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
        JSON report{{"profile", attempt == 2 || exclusive ? "pcm16" : "mix"},
                    {"exclusive", exclusive},
                    {"uniqueSession", attempt == 3},
                    {"eventDriven", eventDriven},
                    {"client3", client3Attempt},
                    {"rawProcessing", attempt == 7},
                    {"activate", hex(result)}};
        if (SUCCEEDED(result)) {
            WAVEFORMATEX* mix{};
            result = client->GetMixFormat(&mix);
            report["getMix"] = hex(result);
            WAVEFORMATEXTENSIBLE native{};
            native.Format = {WAVE_FORMAT_EXTENSIBLE,
                             static_cast<WORD>(channels),
                             rate,
                             rate * channels * 2,
                             static_cast<WORD>(channels * 2),
                             16,
                             sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)};
            native.Samples.wValidBitsPerSample = 16;
            native.dwChannelMask =
                channels == 2 ? KSAUDIO_SPEAKER_STEREO : KSAUDIO_SPEAKER_DIRECTOUT;
            native.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
            if (SUCCEEDED(result) && mix) {
                const auto* format = attempt == 2 || exclusive ? &native.Format : mix;
                report["channels"] = format->nChannels;
                report["rate"] = format->nSamplesPerSec;
                report["bits"] = format->wBitsPerSample;
                WAVEFORMATEX* closest{};
                const auto shareMode =
                    exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;
                report["support"] = hex(
                    client->IsFormatSupported(shareMode, format, exclusive ? nullptr : &closest));
                CoTaskMemFree(closest);
                GUID session{};
                const auto generated = attempt == 3 ? CoCreateGuid(&session) : S_OK;
                result = generated;
                bool initializationCalled = false;
                if (client3Attempt) {
                    ComPtr<IAudioClient3> client3;
                    result = client.As(&client3);
                    report["queryClient3"] = hex(result);
                    if (SUCCEEDED(result)) {
                        UINT32 preferred{}, fundamental{}, minimum{}, maximum{};
                        result = client3->GetSharedModeEnginePeriod(
                            format, &preferred, &fundamental, &minimum, &maximum);
                        report["queryPeriod"] = hex(result);
                        report["periods"] = {{"preferred", preferred},
                                             {"fundamental", fundamental},
                                             {"minimum", minimum},
                                             {"maximum", maximum}};
                        if (SUCCEEDED(result)) {
                            const auto frames = attempt == 5 ? minimum : preferred;
                            report["requestedPeriod"] = frames;
                            initializationCalled = true;
                            result = client3->InitializeSharedAudioStream(
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK, frames, format, nullptr);
                        }
                    }
                } else {
                    if (attempt == 7) {
                        ComPtr<IAudioClient2> client2;
                        result = client.As(&client2);
                        if (SUCCEEDED(result)) {
                            AudioClientProperties properties{};
                            properties.cbSize = sizeof(properties);
                            properties.eCategory = AudioCategory_Other;
                            properties.Options = AUDCLNT_STREAMOPTIONS_RAW;
                            result = client2->SetClientProperties(&properties);
                        }
                        report["setProperties"] = hex(result);
                    }
                    if (SUCCEEDED(result)) {
                        initializationCalled = true;
                        result = client->Initialize(
                            shareMode, eventDriven ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0,
                            exclusive ? 100000 : 0, exclusive ? 100000 : 0, format,
                            attempt == 3 ? &session : nullptr);
                    }
                }
                report["initializeCalled"] = initializationCalled;
                report[initializationCalled ? "initialize" : "preparationError"] = hex(result);
                HANDLE event{};
                if (SUCCEEDED(result) && eventDriven) {
                    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    report["setEvent"] = hex(event ? client->SetEventHandle(event) : E_OUTOFMEMORY);
                }
                client.Reset(); // Release the client before closing its event.
                if (event)
                    CloseHandle(event);
            }
            CoTaskMemFree(mix);
        }
        attempts.push_back(std::move(report));
    }
    return attempts;
}
class PrivateRoundtrip {
  public:
    void start(std::string uid) {
        finish();
        ready.store(false);
        failed.store(false);
        failure.clear();
        if (uid.starts_with("wasapi:"))
            uid.erase(0, 7);
        worker = std::jthread([this, uid = std::move(uid)](std::stop_token stop) {
            const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(initialized)) {
                failed.store(true);
                ready.store(true);
                return;
            }
            {
                loopz::windows::DriverTransport transport;
                std::string error;
                // This probe intentionally exercises the legacy PCM16 private contract.
                if (!transport.connect(uid, error, false)) {
                    failure = error;
                    failed.store(true);
                } else {
                    version.store(transport.version());
                    bits.store(transport.sampleBytes() * 8);
                }
                ready.store(true);
                loopz_driver::DuplexBridge::Packet packet;
                while (!stop.stop_requested() && !failed.load()) {
                    if (!transport.read(packet, error)) {
                        failure = error;
                        failed.store(true);
                        break;
                    }
                    if (!packet.frames) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        continue;
                    }
                    // A measured half-amplitude output proves recording came
                    // through processing, rather than the old direct cable.
                    for (unsigned i = 0; i < packet.frames * transport.format().frameBytes();
                         i += 2) {
                        const unsigned raw = static_cast<unsigned>(packet.samples[i]) |
                                             (static_cast<unsigned>(packet.samples[i + 1]) << 8);
                        const int signedSample =
                            raw >= 32768 ? static_cast<int>(raw) - 65536 : static_cast<int>(raw);
                        const auto sample = static_cast<unsigned short>(signedSample / 2);
                        packet.samples[i] = static_cast<unsigned char>(sample & 255);
                        packet.samples[i + 1] = static_cast<unsigned char>(sample >> 8);
                    }
                    if (!transport.write(packet.samples, packet.frames, error)) {
                        failure = error;
                        failed.store(true);
                    }
                }
            }
            CoUninitialize();
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!ready.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (!ready.load() || failed.load()) {
            finish();
            throw std::runtime_error(failure.empty() ? "Private transport did not connect"
                                                     : failure);
        }
    }
    void finish() {
        if (worker.joinable()) {
            worker.request_stop();
            worker.join();
        }
    }
    bool healthy() const {
        return !failed.load();
    }
    unsigned protocolVersion() const {
        return version.load();
    }
    unsigned bitsPerSample() const {
        return bits.load();
    }
    ~PrivateRoundtrip() {
        finish();
    }

  private:
    std::jthread worker;
    std::atomic<bool> ready{false}, failed{false};
    std::atomic<unsigned> version{0}, bits{0};
    std::string failure;
};
struct Marker {
    std::atomic<unsigned> mode{1};
    uint64_t position = 0;
    static bool render(void* raw, const singz::AudioHostRenderBlock& block) noexcept {
        auto& self = *static_cast<Marker*>(raw);
        if (block.outputChannels != 2)
            return false;
        const auto mode = self.mode.load();
        for (unsigned c = 0; c < 2; ++c) {
            const double frequency = mode == 1 ? (c ? 887 : 443) : (c ? 1597 : 997);
            for (unsigned i = 0; i < block.frames; ++i)
                block.output[c][i] =
                    0.05f * static_cast<float>(
                                std::sin(tau * frequency * static_cast<double>(self.position + i) /
                                         block.sampleRate));
        }
        self.position += block.frames;
        return true;
    }
};
struct Recorder {
    std::atomic_flag access = ATOMIC_FLAG_INIT;
    std::vector<float> samples;
    size_t captured{};
    bool collecting{};
    explicit Recorder(size_t capacity = 48000 * 4) : samples(capacity) {}
    void accept(const singz::AudioInputBlockView& block) noexcept {
        // The callback never waits or allocates. Control-side begin/end owns
        // the same flag only outside the measured collection interval.
        if (access.test_and_set(std::memory_order_acquire))
            return;
        if (collecting) {
            const auto count = std::min<size_t>(block.frames, samples.size() - captured);
            std::copy_n(block.mono, count, samples.data() + captured);
            captured += count;
        }
        access.clear(std::memory_order_release);
    }
    struct Control {
        std::atomic_flag& flag;
        explicit Control(std::atomic_flag& value) : flag(value) {
            while (flag.test_and_set(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ~Control() {
            flag.clear(std::memory_order_release);
        }
    };
    void begin() {
        Control lock(access);
        captured = 0;
        collecting = true;
    }
    std::vector<float> end() {
        Control lock(access);
        collecting = false;
        return {samples.begin(), samples.begin() + captured};
    }
};
JSON spectrum(const std::vector<float>& samples, double rate, double target, double other) {
    double re = 0, im = 0, otherRe = 0, otherIm = 0, peak = 0;
    size_t zeroRun = 0, maximumZeroRun = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
        const double sample = samples[i];
        peak = std::max(peak, std::abs(sample));
        zeroRun = std::abs(sample) < 1e-8 ? zeroRun + 1 : 0;
        maximumZeroRun = std::max(maximumZeroRun, zeroRun);
        const double phase = tau * static_cast<double>(i) / rate;
        re += sample * std::cos(phase * target);
        im += sample * std::sin(phase * target);
        otherRe += sample * std::cos(phase * other);
        otherIm += sample * std::sin(phase * other);
    }
    const double scale = samples.empty() ? 0 : 2.0 / static_cast<double>(samples.size());
    return {{"frames", samples.size()},
            {"peak", peak},
            {"maximumZeroRun", maximumZeroRun},
            {"target", std::hypot(re, im) * scale},
            {"other", std::hypot(otherRe, otherIm) * scale}};
}
struct FormatMarker {
    unsigned channels, rate, capacity, captured = 0;
    float amplitude = .05f;
    std::vector<float> recording;
    std::array<double, 64> sine{}, cosine{}, stepSine{}, stepCosine{};
    std::atomic<bool> collecting{false}, invalid{false};
    FormatMarker(unsigned count, unsigned sampleRate, unsigned recordingMs = 1500,
                 bool streaming = false)
        : channels(count), rate(sampleRate), capacity(sampleRate * (recordingMs / 1000 + 1)),
          recording(streaming ? 0 : static_cast<size_t>(count) * capacity) {
        for (unsigned c = 0; c < channels; ++c) {
            cosine[c] = 1;
            stepSine[c] = std::sin(tau * (443 + c * 37) / rate);
            stepCosine[c] = std::cos(tau * (443 + c * 37) / rate);
        }
    }
    float next(unsigned channel) noexcept {
        const auto value = amplitude * static_cast<float>(sine[channel]);
        const auto next = sine[channel] * stepCosine[channel] + cosine[channel] * stepSine[channel];
        cosine[channel] = cosine[channel] * stepCosine[channel] - sine[channel] * stepSine[channel];
        sine[channel] = next;
        return value;
    }
    static bool render(void* raw, const singz::AudioHostRenderBlock& block) noexcept {
        auto& self = *static_cast<FormatMarker*>(raw);
        if (block.outputChannels != self.channels || block.inputChannels != self.channels ||
            block.sampleRate != self.rate || !block.input || !block.output) {
            self.invalid.store(true);
            return false;
        }
        const unsigned count =
            self.collecting.load() ? std::min(block.frames, self.capacity - self.captured) : 0;
        for (unsigned c = 0; c < self.channels; ++c) {
            for (unsigned i = 0; i < block.frames; ++i) {
                block.output[c][i] = self.next(c);
                if (i < count)
                    self.recording[static_cast<size_t>(c) * self.capacity + self.captured + i] =
                        block.input[c][i];
            }
        }
        self.captured += count;
        return true;
    }
};
void rawChecked(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::ostringstream message;
        message << operation << " (0x" << std::hex << static_cast<unsigned long>(result) << ")";
        throw std::runtime_error(message.str());
    }
}
struct RawEndpoint {
    Microsoft::WRL::ComPtr<IAudioClient> client;
    HANDLE event{};
    UINT32 bufferFrames{}, periodFrames{};
    REFERENCE_TIME defaultPeriod{}, minimumPeriod{};
    RawEndpoint() = default;
    RawEndpoint(const RawEndpoint&) = delete;
    RawEndpoint& operator=(const RawEndpoint&) = delete;
    ~RawEndpoint() {
        if (client)
            client->Stop();
        client.Reset();
        if (event)
            CloseHandle(event);
    }
    void open(IMMDeviceEnumerator* enumerator, const std::string& uid, unsigned rate,
              unsigned channels, bool shortPeriod = false, bool exclusive = false,
              bool exclusiveFloat = false) {
        const auto raw = uid.starts_with("wasapi:") ? uid.substr(7) : uid;
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(),
                                              static_cast<int>(raw.size()), nullptr, 0);
        if (!count)
            throw std::runtime_error("Invalid raw-probe endpoint identity");
        std::wstring id(static_cast<size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()),
                            id.data(), count);
        Microsoft::WRL::ComPtr<IMMDevice> device;
        rawChecked(enumerator->GetDevice(id.c_str(), &device), "Find raw-probe endpoint");
        rawChecked(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client),
                   "Activate raw-probe endpoint");
        struct Format {
            WAVEFORMATEX* value{};
            ~Format() {
                CoTaskMemFree(value);
            }
        } format;
        rawChecked(client->GetMixFormat(&format.value), "Read raw-probe mix format");
        const auto* wave = format.value;
        const bool floating =
            wave && (wave->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                     (wave->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                      wave->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
                      reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wave)->SubFormat ==
                          KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
        WAVEFORMATEXTENSIBLE native{};
        if (exclusive) {
            native.Format = {WAVE_FORMAT_EXTENSIBLE,
                             static_cast<WORD>(channels),
                             rate,
                             rate * channels * (exclusiveFloat ? 4 : 2),
                             static_cast<WORD>(channels * (exclusiveFloat ? 4 : 2)),
                             static_cast<WORD>(exclusiveFloat ? 32 : 16),
                             sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)};
            native.Samples.wValidBitsPerSample = exclusiveFloat ? 32 : 16;
            native.dwChannelMask =
                channels == 2 ? KSAUDIO_SPEAKER_STEREO : KSAUDIO_SPEAKER_DIRECTOUT;
            native.SubFormat =
                exclusiveFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
            wave = &native.Format;
        }
        if (!exclusive &&
            (!floating || wave->wBitsPerSample != 32 || wave->nSamplesPerSec != rate ||
             wave->nChannels != channels || wave->nBlockAlign != channels * sizeof(float)))
            throw std::runtime_error("Raw probe requires the exact public float32 mix format");
        if (exclusive) {
            rawChecked(client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, wave, nullptr),
                       "Check requested exclusive format");
            REFERENCE_TIME duration = 100000; // Event-driven, whole 10 ms buffers.
            auto initialized =
                client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                   duration, duration, wave, nullptr);
            if (initialized == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                rawChecked(client->GetBufferSize(&bufferFrames),
                           "Read aligned exclusive buffer size");
                duration =
                    static_cast<REFERENCE_TIME>((10000000ULL * bufferFrames + rate - 1) / rate);
                client.Reset();
                rawChecked(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client),
                           "Reopen aligned exclusive endpoint");
                initialized = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK, duration,
                                                 duration, wave, nullptr);
            }
            rawChecked(initialized, "Initialize requested exclusive stream");
        } else if (shortPeriod) {
            Microsoft::WRL::ComPtr<IAudioClient3> client3;
            rawChecked(client.As(&client3), "Open low-latency audio interface");
            UINT32 preferred{}, fundamental{}, minimum{}, maximum{};
            rawChecked(client3->GetSharedModeEnginePeriod(wave, &preferred, &fundamental, &minimum,
                                                          &maximum),
                       "Read shared engine periods");
            if (!fundamental || !minimum || minimum > maximum)
                throw std::runtime_error("Invalid shared engine periods");
            const auto desired = std::max(minimum, (rate + 249) / 250); // At least 4 ms.
            periodFrames = ((desired + fundamental - 1) / fundamental) * fundamental;
            if (periodFrames > maximum || periodFrames >= (rate + 99) / 100)
                throw std::runtime_error("Endpoint does not expose a short shared period: " +
                                         JSON({{"preferred", preferred},
                                               {"fundamental", fundamental},
                                               {"minimum", minimum},
                                               {"maximum", maximum}})
                                             .dump());
            rawChecked(client3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                            periodFrames, wave, nullptr),
                       "Initialize short-period shared stream");
        } else {
            rawChecked(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                          AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, wave, nullptr),
                       "Initialize raw-probe shared stream");
        }
        rawChecked(client->GetBufferSize(&bufferFrames), "Get raw-probe buffer size");
        rawChecked(client->GetDevicePeriod(&defaultPeriod, &minimumPeriod),
                   "Get raw-probe device periods");
        if (!bufferFrames || bufferFrames > rate * 2)
            throw std::runtime_error("Unexpected raw-probe endpoint buffering");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event)
            throw std::runtime_error("Cannot create raw-probe event");
        rawChecked(client->SetEventHandle(event), "Set raw-probe event");
    }
};
JSON reopenCaptureSelection(const std::string& uid, unsigned rate, unsigned channels) {
    using Microsoft::WRL::ComPtr;
    rawChecked(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Initialize selection probe");
    struct Apartment {
        ~Apartment() {
            CoUninitialize();
        }
    } apartment;
    const auto raw = uid.starts_with("wasapi:") ? uid.substr(7) : uid;
    const auto endpoints = loopz::windows::loopzEndpoints();
    const auto owned = endpoints.find(raw);
    if (owned == endpoints.end() || owned->second.role != loopz::windows::EndpointRole::Microphone)
        throw std::runtime_error("Cached selection is not an active LoopZ microphone");
    ComPtr<IMMDeviceEnumerator> enumerator;
    rawChecked(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)),
               "Create selection enumerator");
    RawEndpoint capture;
    capture.open(enumerator.Get(), raw, rate, channels);
    ComPtr<IAudioCaptureClient> recorder;
    rawChecked(capture.client->GetService(IID_PPV_ARGS(&recorder)),
               "Open selected microphone capture");
    rawChecked(capture.client->Start(), "Start selected microphone capture");
    UINT32 pending{};
    for (unsigned attempt = 0; attempt < 20 && !pending; ++attempt) {
        WaitForSingleObject(capture.event, 50);
        rawChecked(recorder->GetNextPacketSize(&pending), "Poll selected microphone capture");
    }
    if (!pending)
        throw std::runtime_error("Selected microphone did not produce a capture packet");
    BYTE* data{};
    UINT32 frames{};
    DWORD flags{};
    rawChecked(recorder->GetBuffer(&data, &frames, &flags, nullptr, nullptr),
               "Read selected microphone capture");
    rawChecked(recorder->ReleaseBuffer(frames), "Release selected microphone capture");
    if (!frames)
        throw std::runtime_error("Selected microphone returned an empty packet");
    return {{"ok", true},         {"endpoint", raw},      {"mixKey", owned->second.mixKey},
            {"sampleRate", rate}, {"channels", channels}, {"capturedFrames", frames},
            {"flags", flags}};
}
JSON rawFormatRecording(const std::string& feed, const std::string& mic, FormatMarker& marker,
                        bool shortPeriod = false, unsigned recordingMs = 1500,
                        bool leasePeriod = false) {
    using Microsoft::WRL::ComPtr;
    rawChecked(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Initialize raw-probe apartment");
    struct Apartment {
        ~Apartment() {
            CoUninitialize();
        }
    } apartment;
    ComPtr<IMMDeviceEnumerator> enumerator;
    rawChecked(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)),
               "Open raw-probe enumerator");
    RawEndpoint feedLease, microphoneLease, playback, capture;
    if (leasePeriod) {
        // Test ordinary clients while another shared client holds the engine's
        // supported short period. Only the test feed receives startup silence.
        feedLease.open(enumerator.Get(), feed, marker.rate, marker.channels, true);
        microphoneLease.open(enumerator.Get(), mic, marker.rate, marker.channels, true);
        ComPtr<IAudioRenderClient> silence;
        rawChecked(feedLease.client->GetService(IID_PPV_ARGS(&silence)),
                   "Open period-lease renderer");
        BYTE* bytes{};
        rawChecked(silence->GetBuffer(feedLease.bufferFrames, &bytes),
                   "Acquire period-lease silence");
        rawChecked(silence->ReleaseBuffer(feedLease.bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT),
                   "Release period-lease silence");
        rawChecked(microphoneLease.client->Start(), "Start microphone period lease");
        rawChecked(feedLease.client->Start(), "Start feed period lease");
    }
    playback.open(enumerator.Get(), feed, marker.rate, marker.channels, shortPeriod);
    capture.open(enumerator.Get(), mic, marker.rate, marker.channels, shortPeriod);
    ComPtr<IAudioRenderClient> renderer;
    ComPtr<IAudioCaptureClient> recorder;
    rawChecked(playback.client->GetService(IID_PPV_ARGS(&renderer)), "Open raw-probe renderer");
    rawChecked(capture.client->GetService(IID_PPV_ARGS(&recorder)), "Open raw-probe recorder");
    uint64_t rendered = 0, packets = 0, discontinuities = 0, positionGaps = 0, timestampErrors = 0,
             silentFrames = 0, previousEnd = 0;
    bool havePosition = false;
    const auto write = [&](UINT32 frames) {
        BYTE* bytes{};
        rawChecked(renderer->GetBuffer(frames, &bytes), "Acquire raw-probe playback buffer");
        auto* samples = reinterpret_cast<float*>(bytes);
        for (unsigned frame = 0; frame < frames; ++frame)
            for (unsigned channel = 0; channel < marker.channels; ++channel)
                samples[frame * marker.channels + channel] = marker.next(channel);
        rawChecked(renderer->ReleaseBuffer(frames, 0), "Release raw-probe playback buffer");
        rendered += frames;
    };
    write(playback.bufferFrames);
    rawChecked(capture.client->Start(), "Start raw-probe recording");
    rawChecked(playback.client->Start(), "Start raw-probe playback");
    const auto startTicks = qpcTicks();
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const auto start = std::chrono::steady_clock::now();
    HANDLE events[]{playback.event, capture.event};
    while (std::chrono::steady_clock::now() - start <
           std::chrono::milliseconds(1200 + recordingMs)) {
        UINT32 padding{};
        rawChecked(playback.client->GetCurrentPadding(&padding), "Get raw-probe playback padding");
        if (padding > playback.bufferFrames)
            throw std::runtime_error("Invalid playback padding");
        if (padding < playback.bufferFrames)
            write(playback.bufferFrames - padding);
        UINT32 pending{};
        rawChecked(recorder->GetNextPacketSize(&pending), "Get raw-probe capture packet");
        while (pending) {
            BYTE* bytes{};
            UINT32 frames{};
            DWORD flags{};
            UINT64 position{}, timestamp{};
            rawChecked(recorder->GetBuffer(&bytes, &frames, &flags, &position, &timestamp),
                       "Acquire raw-probe capture buffer");
            ++packets;
            discontinuities += (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
            timestampErrors += (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0;
            if (!(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)) {
                if (havePosition && position != previousEnd)
                    ++positionGaps;
                previousEnd = position + frames;
                havePosition = true;
            }
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                silentFrames += frames;
            if (std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(1200)) {
                const auto count = std::min(frames, marker.capacity - marker.captured);
                const auto* samples = reinterpret_cast<const float*>(bytes);
                for (unsigned channel = 0; channel < marker.channels; ++channel)
                    for (unsigned frame = 0; frame < count; ++frame)
                        marker.recording[static_cast<size_t>(channel) * marker.capacity +
                                         marker.captured + frame] =
                            flags & AUDCLNT_BUFFERFLAGS_SILENT
                                ? 0
                                : samples[frame * marker.channels + channel];
                marker.captured += count;
            }
            rawChecked(recorder->ReleaseBuffer(frames), "Release raw-probe capture buffer");
            rawChecked(recorder->GetNextPacketSize(&pending), "Get next raw-probe capture packet");
        }
        if (WaitForMultipleObjects(2, events, FALSE, 10) == WAIT_FAILED)
            throw std::runtime_error("Raw-probe event wait failed");
    }
    const auto endTicks = qpcTicks();
    return {{"streamStartQpcTicks", startTicks},
            {"recordingStartQpcTicks",
             startTicks + static_cast<uint64_t>(frequency.QuadPart) * 12 / 10},
            {"streamEndQpcTicks", endTicks},
            {"renderedFrames", rendered},
            {"capturePackets", packets},
            {"captureDiscontinuities", discontinuities},
            {"capturePositionGaps", positionGaps},
            {"captureTimestampErrors", timestampErrors},
            {"captureSilentFrames", silentFrames},
            {"inputBufferFrames", capture.bufferFrames},
            {"outputBufferFrames", playback.bufferFrames},
            {"inputPeriodFrames", capture.periodFrames},
            {"outputPeriodFrames", playback.periodFrames},
            {"inputDefaultPeriod100ns", capture.defaultPeriod},
            {"outputDefaultPeriod100ns", playback.defaultPeriod},
            {"leasedInputPeriodFrames", microphoneLease.periodFrames},
            {"leasedOutputPeriodFrames", feedLease.periodFrames}};
}
JSON exclusiveFormatRecording(const std::string& feed, const std::string& mic, FormatMarker& marker,
                              unsigned recordingMs, bool floating = false,
                              loopz::windows::StreamingProbeCapture* streaming = nullptr) {
    using Microsoft::WRL::ComPtr;
    rawChecked(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Initialize exclusive probe");
    struct Apartment {
        ~Apartment() {
            CoUninitialize();
        }
    } apartment;
    DWORD task{};
    struct Priority {
        HANDLE handle{};
        ~Priority() {
            if (handle)
                AvRevertMmThreadCharacteristics(handle);
        }
    } priority{AvSetMmThreadCharacteristicsW(L"Audio", &task)};
    const auto mmcssError = priority.handle ? 0 : GetLastError();
    ComPtr<IMMDeviceEnumerator> enumerator;
    rawChecked(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)),
               "Create exclusive probe enumerator");
    RawEndpoint playback, capture;
    playback.open(enumerator.Get(), feed, marker.rate, marker.channels, false, true, floating);
    capture.open(enumerator.Get(), mic, marker.rate, marker.channels, false, true, floating);
    ComPtr<IAudioRenderClient> renderer;
    ComPtr<IAudioCaptureClient> recorder;
    rawChecked(playback.client->GetService(IID_PPV_ARGS(&renderer)), "Open exclusive renderer");
    rawChecked(capture.client->GetService(IID_PPV_ARGS(&recorder)), "Open exclusive recorder");
    uint64_t rendered = 0, packets = 0, discontinuities = 0, timestampErrors = 0, positionGaps = 0,
             previousEnd = 0, silentFrames = 0;
    bool havePosition = false;
    const auto write = [&] {
        BYTE* bytes{};
        rawChecked(renderer->GetBuffer(playback.bufferFrames, &bytes),
                   "Acquire exclusive playback buffer");
        auto* samples = reinterpret_cast<short*>(bytes);
        for (unsigned frame = 0; frame < playback.bufferFrames; ++frame)
            for (unsigned channel = 0; channel < marker.channels; ++channel)
                if (floating)
                    reinterpret_cast<float*>(bytes)[frame * marker.channels + channel] =
                        marker.next(channel);
                else
                    samples[frame * marker.channels + channel] = static_cast<short>(
                        std::clamp(marker.next(channel) * 32768.0f, -32768.0f, 32767.0f));
        rawChecked(renderer->ReleaseBuffer(playback.bufferFrames, 0),
                   "Release exclusive playback buffer");
        rendered += playback.bufferFrames;
    };
    write();
    rawChecked(capture.client->Start(), "Start exclusive capture");
    rawChecked(playback.client->Start(), "Start exclusive playback");
    const auto startTicks = qpcTicks();
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const auto start = std::chrono::steady_clock::now();
    HANDLE events[]{playback.event, capture.event};
    while (std::chrono::steady_clock::now() - start <
           std::chrono::milliseconds(1200 + recordingMs)) {
        const auto ready = WaitForMultipleObjects(2, events, FALSE, 10);
        if (ready == WAIT_OBJECT_0) {
            write();
        } else if (ready == WAIT_OBJECT_0 + 1) {
            BYTE* bytes{};
            UINT32 frames{};
            DWORD flags{};
            UINT64 position{}, timestamp{};
            const auto acquired =
                recorder->GetBuffer(&bytes, &frames, &flags, &position, &timestamp);
            if (acquired == AUDCLNT_S_BUFFER_EMPTY)
                continue;
            rawChecked(acquired, "Acquire exclusive capture buffer");
            ++packets;
            discontinuities += (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
            timestampErrors += (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0;
            if (!(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)) {
                if (havePosition && position != previousEnd)
                    ++positionGaps;
                previousEnd = position + frames;
                havePosition = true;
            }
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                silentFrames += frames;
            if (std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(1200)) {
                if (streaming) {
                    streaming->push(reinterpret_cast<const float*>(bytes), frames,
                                    (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
                } else {
                    const auto count = std::min(frames, marker.capacity - marker.captured);
                    const auto* samples = reinterpret_cast<const short*>(bytes);
                    for (unsigned channel = 0; channel < marker.channels; ++channel)
                        for (unsigned frame = 0; frame < count; ++frame)
                            marker.recording[static_cast<size_t>(channel) * marker.capacity +
                                             marker.captured + frame] =
                                flags & AUDCLNT_BUFFERFLAGS_SILENT ? 0
                                : floating ? reinterpret_cast<const float*>(
                                                 bytes)[frame * marker.channels + channel]
                                           : samples[frame * marker.channels + channel] / 32768.0f;
                    marker.captured += count;
                }
            }
            rawChecked(recorder->ReleaseBuffer(frames), "Release exclusive capture buffer");
        } else if (ready == WAIT_FAILED) {
            throw std::runtime_error("Exclusive probe event wait failed");
        }
    }
    return {{"bitsPerSample", floating ? 32 : 16},
            {"qpcFrequency", frequency.QuadPart},
            {"streamStartQpcTicks", startTicks},
            {"recordingStartQpcTicks",
             startTicks + static_cast<uint64_t>(frequency.QuadPart) * 12 / 10},
            {"streamEndQpcTicks", qpcTicks()},
            {"renderedFrames", rendered},
            {"capturePackets", packets},
            {"captureDiscontinuities", discontinuities},
            {"capturePositionGaps", positionGaps},
            {"captureTimestampErrors", timestampErrors},
            {"captureSilentFrames", silentFrames},
            {"inputBufferFrames", capture.bufferFrames},
            {"outputBufferFrames", playback.bufferFrames},
            {"mmcssRegistered", priority.handle != nullptr},
            {"mmcssError", mmcssError}};
}
// Measures application submission to application receipt, including both client
// buffers. This is not hardware/acoustic latency or a selected-app capture path.
JSON exclusiveLatencyRecording(const std::string& feed, const std::string& mic, unsigned rate,
                               unsigned channels, unsigned recordingMs) {
    using Microsoft::WRL::ComPtr;
    if (rate != 48000 || channels != 2 || recordingMs < 15000)
        throw std::runtime_error(
            "Latency probe requires 48000 Hz, 2 channels and at least 15000 ms");
    rawChecked(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Initialize latency probe");
    struct Apartment {
        ~Apartment() {
            CoUninitialize();
        }
    } apartment;
    DWORD task{};
    struct Priority {
        HANDLE value{};
        ~Priority() {
            if (value)
                AvRevertMmThreadCharacteristics(value);
        }
    } priority{AvSetMmThreadCharacteristicsW(L"Audio", &task)};
    rawChecked(priority.value ? S_OK : HRESULT_FROM_WIN32(GetLastError()),
               "Schedule latency probe");
    ComPtr<IMMDeviceEnumerator> enumerator;
    rawChecked(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)),
               "Create latency enumerator");
    RawEndpoint playback, capture;
    playback.open(enumerator.Get(), feed, rate, channels, false, true);
    capture.open(enumerator.Get(), mic, rate, channels, false, true);
    ComPtr<IAudioRenderClient> renderer;
    ComPtr<IAudioCaptureClient> recorder;
    rawChecked(playback.client->GetService(IID_PPV_ARGS(&renderer)), "Open latency renderer");
    rawChecked(capture.client->GetService(IID_PPV_ARGS(&recorder)), "Open latency recorder");
    constexpr unsigned trials = 20;
    std::array<uint64_t, trials> starts{}, submittedBefore{}, submittedAfter{}, received{};
    uint64_t nextStart = uint64_t(rate) * 2;
    for (unsigned i = 0; i < trials; ++i) {
        starts[i] = nextStart;
        nextStart += uint64_t(rate) * (500 + 17 * (i % 3)) / 1000;
    }
    const auto burstFrames = rate * 8 / 1000, edgeFrames = rate / 1000;
    unsigned generating = 0, detected = 0, unexpected = 0, quiet = rate;
    uint64_t rendered = 0, packets = 0, discontinuities = 0, gaps = 0, timestampErrors = 0,
             previousEnd = 0;
    bool havePosition = false, armed = true;
    std::array<double, 2> peaks{};
    const auto write = [&] {
        BYTE* bytes{};
        rawChecked(renderer->GetBuffer(playback.bufferFrames, &bytes), "Acquire latency playback");
        auto* output = reinterpret_cast<short*>(bytes);
        unsigned submission = trials;
        for (unsigned frame = 0; frame < playback.bufferFrames; ++frame) {
            const auto position = rendered + frame;
            while (generating < trials && position >= starts[generating] + burstFrames)
                ++generating;
            double sample = 0;
            if (generating < trials && position >= starts[generating]) {
                const auto offset = static_cast<unsigned>(position - starts[generating]);
                if (!offset)
                    submission = generating;
                const auto edge = std::min({1.0, double(offset) / edgeFrames,
                                            double(burstFrames - 1 - offset) / edgeFrames});
                sample = .05 * edge * std::sin(tau * 1000 * offset / rate);
            }
            const auto pcm = static_cast<short>(sample * 32768);
            for (unsigned c = 0; c < channels; ++c)
                output[frame * channels + c] = pcm;
        }
        if (submission < trials)
            submittedBefore[submission] = qpcTicks();
        rawChecked(renderer->ReleaseBuffer(playback.bufferFrames, 0), "Submit latency playback");
        if (submission < trials)
            submittedAfter[submission] = qpcTicks();
        rendered += playback.bufferFrames;
    };
    write();
    rawChecked(capture.client->Start(), "Start latency recording");
    rawChecked(playback.client->Start(), "Start latency playback");
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const auto startTicks = qpcTicks();
    const auto start = std::chrono::steady_clock::now();
    HANDLE events[]{playback.event, capture.event};
    while (std::chrono::steady_clock::now() - start <
           std::chrono::milliseconds(1200 + recordingMs)) {
        const auto ready = WaitForMultipleObjects(2, events, FALSE, 10);
        if (ready == WAIT_OBJECT_0) {
            write();
        } else if (ready == WAIT_OBJECT_0 + 1) {
            BYTE* bytes{};
            UINT32 frames{};
            DWORD flags{};
            UINT64 position{}, timestamp{};
            const auto result = recorder->GetBuffer(&bytes, &frames, &flags, &position, &timestamp);
            if (result == AUDCLNT_S_BUFFER_EMPTY)
                continue;
            const auto receipt = qpcTicks();
            rawChecked(result, "Receive latency microphone packet");
            ++packets;
            const bool warmed = receipt >= startTicks + uint64_t(frequency.QuadPart) * 12 / 10;
            if (warmed) {
                discontinuities += (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
                timestampErrors += (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0;
                if (havePosition && position != previousEnd)
                    ++gaps;
            }
            previousEnd = position + frames;
            havePosition = true;
            const auto* input = reinterpret_cast<const short*>(bytes);
            for (unsigned frame = 0; frame < frames; ++frame) {
                const auto sample = flags & AUDCLNT_BUFFERFLAGS_SILENT
                                        ? 0.0
                                        : std::abs(double(input[frame * channels])) / 32768;
                for (unsigned c = 0; c < channels; ++c)
                    peaks[c] = std::max(
                        peaks[c], flags & AUDCLNT_BUFFERFLAGS_SILENT
                                      ? 0.0
                                      : std::abs(double(input[frame * channels + c])) / 32768);
                if (sample < .001) {
                    quiet = std::min(quiet + 1, rate);
                    if (quiet >= rate / 200)
                        armed = true;
                } else {
                    quiet = 0;
                }
                if (armed && sample >= .005) {
                    if (detected < trials && submittedAfter[detected] &&
                        receipt >= submittedAfter[detected] &&
                        receipt - submittedBefore[detected] <
                            uint64_t(frequency.QuadPart) * 35 / 100) {
                        received[detected++] = receipt;
                    } else {
                        ++unexpected;
                    }
                    armed = false;
                }
            }
            rawChecked(recorder->ReleaseBuffer(frames), "Release latency capture");
        } else if (ready == WAIT_FAILED) {
            throw std::runtime_error("Latency event wait failed");
        }
    }
    JSON observations = JSON::array(), signal = JSON::array();
    std::vector<double> delays;
    double uncertainty = 0;
    for (unsigned i = 0; i < trials; ++i) {
        if (!received[i])
            continue;
        const auto scale = 1000.0 / static_cast<double>(frequency.QuadPart);
        const auto low = double(received[i] - submittedAfter[i]) * scale;
        const auto high = double(received[i] - submittedBefore[i]) * scale;
        uncertainty = std::max(uncertainty, high - low);
        delays.push_back((low + high) / 2);
        observations.push_back({{"trial", i},
                                {"submittedBeforeQpcTicks", submittedBefore[i]},
                                {"submittedAfterQpcTicks", submittedAfter[i]},
                                {"receivedQpcTicks", received[i]},
                                {"receiptLatencyLowerMs", low},
                                {"receiptLatencyUpperMs", high}});
    }
    bool ok = detected == trials && !unexpected && !discontinuities && !gaps && !timestampErrors;
    for (const auto peak : peaks) {
        signal.push_back({{"peak", peak}});
        ok = ok && peak > .024 && peak < .026;
    }
    std::sort(delays.begin(), delays.end());
    JSON latency{
        {"path", "application submission to LoopZ Feed -> half-gain mix -> public microphone "
                 "application receipt"},
        {"mode", "exclusive PCM16"},
        {"excludes", "selected-app capture, physical hardware, acoustic and network paths"},
        {"includes", "both WASAPI client buffers, marker onset, engine and driver buffering, "
                     "receiver scheduling"},
        {"detectionAbsoluteThreshold", .005},
        {"expectedTrials", trials},
        {"receivedTrials", detected},
        {"unexpectedMarkers", unexpected},
        {"maximumSubmissionUncertaintyMs", uncertainty},
        {"observations", observations}};
    if (!delays.empty()) {
        latency["minimumMs"] = delays.front();
        latency["medianMs"] = (delays[(delays.size() - 1) / 2] + delays[delays.size() / 2]) / 2;
        latency["maximumMs"] = delays.back();
    }
    return {{"ok", ok},
            {"probe", "latency-exclusive-pcm16"},
            {"sampleRate", rate},
            {"channels", channels},
            {"recordingMs", recordingMs},
            {"signal", signal},
            {"latency", latency},
            {"diagnostics",
             {{"streamStartQpcTicks", startTicks},
              {"recordingStartQpcTicks", startTicks + uint64_t(frequency.QuadPart) * 12 / 10},
              {"streamEndQpcTicks", qpcTicks()},
              {"capturePackets", packets},
              {"captureDiscontinuities", discontinuities},
              {"capturePositionGaps", gaps},
              {"captureTimestampErrors", timestampErrors},
              {"inputBufferFrames", capture.bufferFrames},
              {"outputBufferFrames", playback.bufferFrames},
              {"qpcFrequency", frequency.QuadPart}}}};
}

JSON formatSpectrum(const float* samples, unsigned frames, unsigned rate, unsigned target,
                    unsigned other) {
    double aSin = 0, aCos = 1, bSin = 0, bCos = 1, re = 0, im = 0, br = 0, bi = 0, peak = 0;
    const double as = std::sin(tau * target / rate), ac = std::cos(tau * target / rate),
                 bs = std::sin(tau * other / rate), bc = std::cos(tau * other / rate);
    for (unsigned i = 0; i < frames; ++i) {
        const double sample = samples[i];
        peak = std::max(peak, std::abs(sample));
        re += sample * aCos;
        im += sample * aSin;
        br += sample * bCos;
        bi += sample * bSin;
        const double an = aSin * ac + aCos * as, bn = bSin * bc + bCos * bs;
        aCos = aCos * ac - aSin * as;
        bCos = bCos * bc - bSin * bs;
        aSin = an;
        bSin = bn;
    }
    const double scale = frames ? 2.0 / frames : 0;
    return {{"frames", frames},
            {"peak", peak},
            {"target", std::hypot(re, im) * scale},
            {"other", std::hypot(br, bi) * scale}};
}
int formatRoundtrip(const std::string& key, unsigned rate, unsigned channels, bool raw = false,
                    bool shortPeriod = false, unsigned recordingMs = 1500, bool leasePeriod = false,
                    bool exclusive = false, bool latency = false, bool exclusiveFloat = false,
                    bool precision = false, bool streaming = false) {
    if (streaming && (!exclusive || !exclusiveFloat || precision || latency))
        throw std::invalid_argument("Streaming analysis requires exclusive float32 stress mode");
    if (!loopz_driver::AudioFormat{rate, channels}.valid())
        throw std::runtime_error("Unsupported format acceptance request");
    singz::AudioHost host;
    const auto inventory = host.enumerate();
    const auto endpoints = loopz::windows::loopzEndpoints();
    std::string feed, mic;
    JSON observed = JSON::array();
    for (const auto& device : inventory.devices) {
        auto uid = device.uid;
        if (uid.starts_with("wasapi:"))
            uid.erase(0, 7);
        const auto found = endpoints.find(uid);
        if (found == endpoints.end() || found->second.mixKey != key)
            continue;
        observed.push_back({{"uid", device.uid},
                            {"rate", device.nominalSampleRate},
                            {"inputs", device.inputChannels},
                            {"outputs", device.outputChannels},
                            {"role", static_cast<int>(found->second.role)}});
        if (device.nominalSampleRate != rate)
            throw std::runtime_error("Endpoint nominal sample rate does not match saved mix: " +
                                     observed.dump());
        if (found->second.role == loopz::windows::EndpointRole::Feed &&
            device.outputChannels == channels)
            feed = device.uid;
        if (found->second.role == loopz::windows::EndpointRole::Microphone &&
            device.inputChannels == channels)
            mic = device.uid;
    }
    if (feed.empty() || mic.empty())
        throw std::runtime_error("Public endpoint channel counts do not match saved mix: " +
                                 observed.dump());
    if (latency) {
        const auto result = exclusiveLatencyRecording(feed, mic, rate, channels, recordingMs);
        std::cout << result.dump() << '\n';
        return result.at("ok").get<bool>() ? 0 : 1;
    }
    auto marker = std::make_unique<FormatMarker>(channels, rate, recordingMs, streaming);
    if (precision) {
        if (!exclusive || !exclusiveFloat)
            throw std::runtime_error("Precision probe requires explicit float32 streams");
        marker->amplitude = 1e-6f; // Entire signal is below one PCM16 quantization step.
    }
    const double signalScale = precision ? .05 / 1e-6 : 1.0;
    std::unique_ptr<loopz::windows::StreamingProbeCapture> streamingCapture;
    std::jthread analysis;
    if (streaming) {
        streamingCapture = std::make_unique<loopz::windows::StreamingProbeCapture>(rate, channels);
        analysis = std::jthread([&] {
            for (;;) {
                if (streamingCapture->consumeOne())
                    continue;
                if (streamingCapture->producerFinished()) {
                    while (streamingCapture->consumeOne()) {
                    }
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
    struct FinishCapture {
        loopz::windows::StreamingProbeCapture* capture{};
        ~FinishCapture() {
            if (capture)
                capture->finishProducer();
        }
    } finishCapture{
        streamingCapture.get()}; // Signals completion before the analysis thread joins on error.
    JSON diagnostics;
    if (exclusive) {
        diagnostics = exclusiveFormatRecording(feed, mic, *marker, recordingMs, exclusiveFloat,
                                               streamingCapture.get());
    } else if (raw) {
        diagnostics = rawFormatRecording(feed, mic, *marker, shortPeriod, recordingMs, leasePeriod);
    } else {
        singz::AudioHostConfig config;
        config.inputDeviceUid = mic;
        config.outputDeviceUid = feed;
        config.requestedSampleRate = rate;
        for (unsigned c = 0; c < channels; ++c) {
            config.inputChannels.push_back(c);
            config.outputChannels.push_back(c);
        }
        const auto opened = host.open(config, FormatMarker::render, marker.get());
        if (!opened.ok || opened.format.sampleRate != rate) {
            const JSON diagnosis{{"feed", diagnoseSharedOpen(feed, rate, channels)},
                                 {"microphone", diagnoseSharedOpen(mic, rate, channels)}};
            throw std::runtime_error(
                "Cannot open exact-format duplex endpoints: " + opened.message +
                "; direct WASAPI diagnostics: " + diagnosis.dump());
        }
        const auto started = host.start();
        if (!started.ok)
            throw std::runtime_error(started.message);
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        marker->collecting.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(recordingMs));
        marker->collecting.store(false);
        const auto status = host.status();
        diagnostics = {{"callbacks", status.callbacks},
                       {"xruns", status.xruns},
                       {"deadlineMisses", status.deadlineMisses},
                       {"discontinuities", status.discontinuities},
                       {"fifoUnderflows", status.diagnostics.fifoUnderflows},
                       {"fifoOverflows", status.diagnostics.fifoOverflows},
                       {"fifoMinimumFrames", status.diagnostics.fifoMinimumFrames},
                       {"fifoMaximumFrames", status.diagnostics.fifoMaximumFrames},
                       {"startupInputZeroFrames", status.diagnostics.startupInputZeroFrames},
                       {"acceptedCaptureMinusRenderedFrames",
                        status.diagnostics.acceptedCaptureMinusRenderedFrames},
                       {"inputBufferFrames", status.diagnostics.inputBufferFrames},
                       {"outputBufferFrames", status.diagnostics.outputBufferFrames}};
        host.stop(); // joins the only writer before inspecting samples/counters
    }
    if (analysis.joinable()) {
        streamingCapture->finishProducer();
        analysis.join(); // Every published packet is analyzed before reading results.
    }
    uint64_t zeroFrames = 0, longestZeroRun = 0, currentZeroRun = 0, longestZeroStart = 0;
    double maximumStep = 0;
    for (unsigned frame = 0; !streaming && frame < marker->captured; ++frame) {
        bool zero = true;
        for (unsigned channel = 0; channel < channels; ++channel) {
            const auto* samples =
                marker->recording.data() + static_cast<size_t>(channel) * marker->capacity;
            zero = zero && samples[frame] == 0;
            if (frame)
                maximumStep =
                    std::max(maximumStep, std::abs(double(samples[frame]) - samples[frame - 1]));
        }
        if (zero) {
            ++zeroFrames;
            if (++currentZeroRun > longestZeroRun) {
                longestZeroRun = currentZeroRun;
                longestZeroStart = frame + 1 - currentZeroRun;
            }
        } else
            currentZeroRun = 0;
    }
    if (streaming) {
        zeroFrames = streamingCapture->zeroFrames();
        longestZeroRun = streamingCapture->longestZeroRun();
        longestZeroStart = streamingCapture->longestZeroStart();
        maximumStep = streamingCapture->maximumStep();
        diagnostics["probeDroppedFrames"] = streamingCapture->droppedFrames();
        diagnostics["probeAcceptedFrames"] = streamingCapture->acceptedFrames();
        diagnostics["probeAnalyzedFrames"] = streamingCapture->frames();
        diagnostics["probePendingPackets"] = streamingCapture->pendingPackets();
        diagnostics["probeInvalidSamples"] = streamingCapture->invalidSamples();
        diagnostics["probeSampleStorageBytes"] = streamingCapture->sampleStorageBytes();
    }
    diagnostics["allChannelsZeroFrames"] = zeroFrames;
    diagnostics["longestAllChannelsZeroRun"] = longestZeroRun;
    diagnostics["longestAllChannelsZeroRunStartFrame"] =
        longestZeroRun ? JSON(longestZeroStart) : JSON(nullptr);
    diagnostics["maximumSampleStep"] = maximumStep;
    // Every generated channel is a continuous sinusoid. Permit quantization
    // and an isolated common zero crossing, but reject gaps/phase jumps even
    // when their contribution is too short to fail the spectral gain limits.
    const auto expectedStep =
        (2 * .026 * std::sin(tau * .5 * (443 + (channels - 1) * 37) / rate) + 2.0 / 32768) /
        signalScale;
    diagnostics["maximumExpectedSampleStep"] = expectedStep;
    const uint64_t captured = streaming ? streamingCapture->frames() : marker->captured;
    bool ok = !marker->invalid.load() && captured > rate && longestZeroRun <= 1 &&
              maximumStep <= expectedStep;
    if (streaming)
        ok = ok && !streamingCapture->droppedFrames() && !streamingCapture->invalidSamples() &&
             !streamingCapture->pendingPackets() &&
             captured == streamingCapture->acceptedFrames() &&
             captured + 2 * diagnostics.at("inputBufferFrames").get<uint64_t>() >=
                 uint64_t(rate) * recordingMs / 1000;
    if (raw || exclusive)
        for (const auto* counter : {"captureDiscontinuities", "capturePositionGaps",
                                    "captureTimestampErrors", "captureSilentFrames"})
            ok = ok && diagnostics.at(counter).get<uint64_t>() == 0;
    JSON reports = JSON::array();
    for (unsigned c = 0; c < channels; ++c) {
        JSON result;
        if (streaming) {
            const auto spectrum = streamingCapture->spectrum(c);
            result = {{"frames", spectrum.frames},
                      {"peak", spectrum.peak},
                      {"target", spectrum.target},
                      {"other", spectrum.other}};
        } else {
            result = formatSpectrum(
                marker->recording.data() + static_cast<size_t>(c) * marker->capacity,
                marker->captured, rate, 443 + c * 37, 443 + ((c + 1) % channels) * 37);
        }
        const auto target = result.at("target").get<double>();
        ok = ok && target > 0.024 / signalScale && target < 0.026 / signalScale &&
             result.at("other").get<double>() < 0.0004 / signalScale &&
             result.at("peak").get<double>() < 0.026 / signalScale;
        reports.push_back(std::move(result));
    }
    std::cout << JSON({{"ok", ok},
                       {"sampleRate", rate},
                       {"channels", channels},
                       {"expectedGain", 0.5},
                       {"inputAmplitude", marker->amplitude},
                       {"precisionProbe", precision},
                       {"probe", exclusiveFloat ? "exclusive-float32"
                                 : exclusive    ? "exclusive-pcm16"
                                 : raw          ? "raw-wasapi"
                                                : "singz-duplex"},
                       {"shortPeriod", shortPeriod},
                       {"leasedPeriod", leasePeriod},
                       {"recordingMs", recordingMs},
                       {"analysisMode", streaming ? "bounded-streaming" : "stored"},
                       {"diagnostics", diagnostics},
                       {"signal", reports}})
                     .dump()
              << '\n';
    return ok ? 0 : 1;
}
} // namespace
int main(int argc, char** argv) {
    try {
        if ((argc == 5 || argc == 6) &&
            (std::string_view(argv[1]) == "--format-roundtrip" ||
             std::string_view(argv[1]) == "--format-roundtrip-raw" ||
             std::string_view(argv[1]) == "--format-roundtrip-short-period" ||
             std::string_view(argv[1]) == "--format-roundtrip-leased-period" ||
             std::string_view(argv[1]) == "--reopen-capture" ||
             std::string_view(argv[1]) == "--format-roundtrip-exclusive" ||
             std::string_view(argv[1]) == "--format-roundtrip-exclusive-float" ||
             std::string_view(argv[1]) == "--format-stress-exclusive-float" ||
             std::string_view(argv[1]) == "--format-precision-exclusive-float" ||
             std::string_view(argv[1]) == "--latency-roundtrip-exclusive")) {
            const auto parse = [](const std::string& text) -> unsigned {
                const auto value = std::stoul(text);
                if (value > UINT32_MAX || std::to_string(value) != text)
                    throw std::invalid_argument("Invalid format integer");
                return static_cast<unsigned>(value);
            };
            const auto recordingMs = argc == 6 ? parse(argv[5]) : 1500;
            const bool streaming = std::string_view(argv[1]) == "--format-stress-exclusive-float";
            if (!(streaming ? loopz::windows::validStressProbeRecording(parse(argv[3]),
                                                                        parse(argv[4]), recordingMs)
                            : loopz::windows::validProbeRecording(parse(argv[3]), parse(argv[4]),
                                                                  recordingMs)))
                throw std::invalid_argument(
                    streaming ? "Stress recording requires a supported format and 1500..300000 ms"
                              : "Recording requires a supported format, 1500..300000 ms, and at "
                                "most 380928000 stored samples");
            if (std::string_view(argv[1]) == "--reopen-capture") {
                if (argc != 5)
                    throw std::runtime_error("Use --reopen-capture UID RATE CHANNELS");
                std::cout << reopenCaptureSelection(argv[2], parse(argv[3]), parse(argv[4])).dump()
                          << '\n';
                return 0;
            }
            return formatRoundtrip(
                argv[2], parse(argv[3]), parse(argv[4]),
                std::string_view(argv[1]) != "--format-roundtrip",
                std::string_view(argv[1]) == "--format-roundtrip-short-period", recordingMs,
                std::string_view(argv[1]) == "--format-roundtrip-leased-period",
                (std::string_view(argv[1]) == "--format-roundtrip-exclusive" ||
                 std::string_view(argv[1]) == "--format-roundtrip-exclusive-float" ||
                 std::string_view(argv[1]) == "--format-precision-exclusive-float" || streaming),
                std::string_view(argv[1]) == "--latency-roundtrip-exclusive",
                (std::string_view(argv[1]) == "--format-roundtrip-exclusive-float" ||
                 std::string_view(argv[1]) == "--format-precision-exclusive-float" || streaming),
                std::string_view(argv[1]) == "--format-precision-exclusive-float", streaming);
        }
        if (argc == 2 && std::string_view(argv[1]) == "--defaults") {
            std::cout << JSON(loopz::windows::audioDefaultEndpoints()).dump() << '\n';
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--transport-read") {
            const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(initialized))
                throw std::runtime_error("Cannot initialize kernel read regression");
            struct Apartment {
                ~Apartment() {
                    CoUninitialize();
                }
            } apartment;
            loopz::windows::DriverTransport transport;
            std::string error;
            bool ok = transport.connect(argv[2], error);
            unsigned reads = 0;
            auto packet = std::make_unique<loopz_driver::DuplexBridge::Packet>();
            while (ok && reads < 8) {
                ok = transport.read(*packet, error);
                if (ok) {
                    const auto used = packet->frames * transport.frameBytes();
                    ok =
                        std::all_of(packet->samples + used,
                                    packet->samples + sizeof(packet->samples), [](unsigned char b) {
                                        return b == 0;
                                    });
                    if (!ok)
                        error = "Driver returned stale packet padding";
                    ++reads;
                }
            }
            std::cout << JSON({{"ok", ok}, {"reads", reads}, {"error", error}}).dump() << '\n';
            return ok ? 0 : 1;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--transport-access") {
            const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(apartment))
                throw std::runtime_error("Cannot initialize transport access probe");
            {
                loopz::windows::DriverTransport transport;
                std::string error;
                const bool connected = transport.connect(argv[2], error);
                std::cout << JSON({{"connected", connected}, {"error", error}}).dump() << '\n';
            }
            CoUninitialize();
            return 0;
        }
        const bool endpointsOnly = argc == 2 && std::string_view(argv[1]) == "--endpoints";
        const bool privateRoundtrip =
            (argc == 2 || argc == 3) && std::string_view(argv[1]) == "--private-roundtrip";
        const bool engineRoundtrip =
            (argc == 2 || argc == 3) && std::string_view(argv[1]) == "--engine-roundtrip";
        const bool recordApp = argc == 4 && std::string_view(argv[1]) == "--record-app";
        unsigned recordMs = 1500;
        if (recordApp) {
            const std::string_view text = argv[3];
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), recordMs);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
                recordMs < 1000 || recordMs > 60000)
                throw std::invalid_argument("App recording duration must be 1000..60000 ms");
        }
        const bool silenceOnly = argc == 3 && std::string_view(argv[1]) == "--silence-only";
        const bool recordOnly =
            recordApp || silenceOnly ||
            ((argc == 2 || argc == 3) && std::string_view(argv[1]) == "--record-only");
        const std::string key = argc >= 3 ? argv[2] : "";
        if (argc != 1 && !recordOnly && !endpointsOnly && !privateRoundtrip && !engineRoundtrip)
            throw std::runtime_error("Use no arguments, --record-only [mix-key], --silence-only "
                                     "mix-key, --private-roundtrip [mix-key], "
                                     "--engine-roundtrip [mix-key], or --endpoints");
        singz::AudioHost host;
        const auto outputs = host.enumerate();
        const auto endpoints = loopz::windows::loopzEndpoints();
        const auto roles = loopz::windows::loopzEndpointRoles();
        auto matches = [&](std::string_view uid) {
            if (uid.starts_with("wasapi:"))
                uid.remove_prefix(7);
            const auto found = endpoints.find(std::string(uid));
            return found != endpoints.end() && (key.empty() || found->second.mixKey == key);
        };
        if (endpointsOnly) {
            JSON result = JSON::array();
            for (const auto& d : outputs.devices) {
                const auto raw = d.uid.starts_with("wasapi:") ? d.uid.substr(7) : d.uid;
                const auto identity = endpoints.find(raw);
                result.push_back(
                    {{"uid", d.uid},
                     {"name", d.label},
                     {"mixKey", identity == endpoints.end() ? "" : identity->second.mixKey},
                     {"role", static_cast<int>(loopz::windows::endpointRole(roles, d.uid))}});
            }
            std::cout << result.dump() << "\n";
            return 0;
        }
        const auto feed =
            std::find_if(outputs.devices.begin(), outputs.devices.end(), [&](const auto& d) {
                return matches(d.uid) && d.outputChannels == 2 &&
                       loopz::windows::endpointRole(roles, d.uid) ==
                           loopz::windows::EndpointRole::Feed;
            });
        const auto inputs = singz::enumerateAudioInputDevices();
        const auto mic = std::find_if(inputs.begin(), inputs.end(), [&](const auto& d) {
            return matches(d.uid) && d.channels == 2 &&
                   loopz::windows::endpointRole(roles, d.uid) ==
                       loopz::windows::EndpointRole::Microphone;
        });
        if (feed == outputs.devices.end() || mic == inputs.end())
            throw std::runtime_error("LoopZ Feed/Microphone endpoints are not available");
        const auto capacity =
            recordApp ? size_t(192000) * (recordMs / 1000 + 3) : size_t(48000 * 4);
        std::array<Recorder, 2> recorders{Recorder(capacity), Recorder(capacity)};
        std::array<singz::AudioInput, 2> capture;
        std::array<double, 2> rates{};
        for (unsigned c = 0; c < 2; ++c) {
            const auto opened = capture[c].start(
                {mic->uid, c, 64},
                [&, c](const auto& block) {
                    recorders[c].accept(block);
                },
                &inputs);
            if (!opened.ok)
                throw std::runtime_error(opened.error);
            rates[c] = opened.sampleRate;
        }
        Marker marker;
        PrivateRoundtrip transport;
        if (privateRoundtrip)
            transport.start(mic->uid);
        singz::AudioHostConfig config;
        config.outputDeviceUid = feed->uid;
        config.outputChannels = {0, 1};
        config.requestedSampleRate = 48000;
        if (!recordOnly) {
            const auto opened = host.open(config, Marker::render, &marker);
            if (!opened.ok)
                throw std::runtime_error(opened.message);
            const auto started = host.start();
            if (!started.ok)
                throw std::runtime_error(started.message);
        }
        JSON phases = JSON::array();
        ULONGLONG collectionStart100ns{}, collectionEnd100ns{};
        bool ok = true;
        const unsigned count = recordOnly ? 1 : privateRoundtrip ? 5 : 3;
        for (unsigned phase = 0; phase < count; ++phase) {
            if (phase == 1)
                host.stop();
            if (phase == 2) {
                marker.mode.store(2);
                const auto opened = host.open(config, Marker::render, &marker);
                if (!opened.ok)
                    throw std::runtime_error(opened.message);
                const auto started = host.start();
                if (!started.ok)
                    throw std::runtime_error(started.message);
            }
            if (privateRoundtrip && phase == 3)
                transport.finish(); // playback continues; recording must be silent
            if (privateRoundtrip && phase == 4)
                transport.start(mic->uid);
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
            if (recordApp)
                QueryInterruptTimePrecise(&collectionStart100ns);
            for (auto& recorder : recorders)
                recorder.begin();
            std::this_thread::sleep_for(std::chrono::milliseconds(recordMs));
            JSON channels = JSON::array();
            for (unsigned c = 0; c < 2; ++c) {
                const double left = recordOnly ? 440 : (phase >= 2 ? 997 : 443);
                const double right = recordOnly ? 440 : (phase >= 2 ? 1597 : 887);
                const auto data = spectrum(recorders[c].end(), rates[c], c ? right : left,
                                           recordOnly ? 880 : (c ? left : right));
                const auto target = data.at("target").get<double>();
                const bool halfAmplitude = privateRoundtrip || engineRoundtrip;
                const bool signal =
                    target > (halfAmplitude ? 0.024 : 0.01) &&
                    (!halfAmplitude || (target < 0.026 && data.at("peak").get<double>() < 0.026)) &&
                    data.at("other").get<double>() < (halfAmplitude ? 0.0002 : 0.003);
                const bool appSignal =
                    target > 0.0135 && target < 0.0165 &&
                    data.at("other").get<double>() < target * 0.01 &&
                    data.at("maximumZeroRun").get<size_t>() <= 4 &&
                    data.at("frames").get<size_t>() > rates[c] * recordMs / 1000 * 0.95;
                const bool silence = data.at("peak").get<double>() < 0.0001;
                ok = ok && data.at("frames").get<size_t>() > 48000 &&
                     (phase == 1 || silenceOnly || (privateRoundtrip && phase == 3)
                          ? silence
                          : (recordApp ? appSignal : signal));
                channels.push_back(data);
            }
            if (recordApp) {
                QueryInterruptTimePrecise(&collectionEnd100ns);
                // Retain the public capture clients while the parent obtains a
                // fresh post-window driver counter sample from the engine.
                std::this_thread::sleep_for(std::chrono::milliseconds(700));
            }
            phases.push_back({{"phase", phase}, {"channels", channels}});
        }
        host.stop();
        transport.finish();
        ok = ok && transport.healthy();
        for (auto& input : capture) {
            ok =
                ok && input.lastError().empty() && input.state() == singz::AudioInputState::Running;
            input.stop();
        }
        std::cout << JSON({{"ok", ok},
                           {"recordOnly", recordOnly},
                           {"recordApp", recordApp},
                           {"recordingMs", recordMs},
                           {"collectionStart100ns", collectionStart100ns},
                           {"collectionEnd100ns", collectionEnd100ns},
                           {"privateRoundtrip", privateRoundtrip},
                           {"privateTransportVersion", transport.protocolVersion()},
                           {"privateBitsPerSample", transport.bitsPerSample()},
                           {"engineRoundtrip", engineRoundtrip},
                           {"phases", phases}})
                         .dump()
                  << '\n';
        return ok ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
