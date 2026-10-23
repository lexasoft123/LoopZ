#include "driver_transport.hpp"
#include "driver/transport_negotiation.hpp"
#include <windows.h>
#include <devicetopology.h>
#include <mmdeviceapi.h>
#include <ks.h>
#include <ksproxy.h>
#include <wrl.h>
#include "driver/transport_guid.hpp"
#include <cstring>
#include <sstream>

namespace loopz::windows {
namespace {
using Microsoft::WRL::ComPtr;
using namespace loopz_driver;
bool report(HRESULT result, const char* action, std::string& error) {
    if (SUCCEEDED(result))
        return true;
    std::ostringstream message;
    message << action << " (0x" << std::hex << static_cast<unsigned long>(result) << ")";
    error = message.str();
    return false;
}
struct Request {
    KSPROPERTY property{};
    TransportInstance instance{};
};
static_assert(offsetof(Request, instance) == sizeof(KSPROPERTY));
HRESULT exchange(IKsControl* control, TransportProperty property, unsigned long long generation,
                 void* value, ULONG bytes, bool set, unsigned version = transportVersion) {
    Request request;
    request.property.Set = KSPROPSETID_LoopZTransport;
    request.property.Id = static_cast<ULONG>(property);
    request.property.Flags = set ? KSPROPERTY_TYPE_SET : KSPROPERTY_TYPE_GET;
    request.instance.version = version;
    request.instance.generation = generation;
    ULONG returned{};
    const auto result =
        control->KsProperty(&request.property, sizeof(request), value, bytes, &returned);
    if (SUCCEEDED(result) && !set && returned != bytes)
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    return result;
}
} // namespace
struct DriverTransport::Impl {
    ComPtr<IKsControl> control;
    unsigned long long generation = 0;
    TransportConfiguration configuration{};
};
DriverTransport::DriverTransport() : impl_(std::make_unique<Impl>()) {}
DriverTransport::~DriverTransport() {
    close();
}
void DriverTransport::close() noexcept {
    if (impl_->control && impl_->generation)
        exchange(impl_->control.Get(), TransportProperty::Disconnect, impl_->generation, nullptr, 0,
                 true, impl_->configuration.version);
    impl_->generation = 0;
    impl_->configuration = {};
    impl_->control.Reset();
}
bool DriverTransport::connect(const std::string& endpoint, std::string& error, bool preferFloat) {
    close();
    error.clear();
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, endpoint.data(),
                                          static_cast<int>(endpoint.size()), nullptr, 0);
    if (!size) {
        error = "Invalid virtual microphone endpoint";
        return false;
    }
    std::wstring id(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, endpoint.data(),
                        static_cast<int>(endpoint.size()), id.data(), size);
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device, adapter;
    ComPtr<IDeviceTopology> topology;
    ComPtr<IConnector> connector;
    ComPtr<IKsControl> control;
    if (!report(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 IID_PPV_ARGS(&enumerator)),
                "Open audio devices", error) ||
        !report(enumerator->GetDevice(id.c_str(), &device), "Find virtual microphone", error) ||
        !report(device->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, &topology),
                "Open microphone topology", error) ||
        !report(topology->GetConnector(0, &connector), "Find microphone adapter", error))
        return false;
    LPWSTR adapterId{};
    if (!report(connector->GetDeviceIdConnectedTo(&adapterId), "Find adapter identity", error))
        return false;
    const auto found = enumerator->GetDevice(adapterId, &adapter);
    CoTaskMemFree(adapterId);
    if (!report(found, "Open microphone adapter", error) ||
        !report(adapter->Activate(__uuidof(IKsControl), CLSCTX_ALL, nullptr, &control),
                "Open private audio transport", error))
        return false;
    TransportConfiguration configuration;
    HRESULT queried = S_OK;
    const auto negotiated = negotiateTransport(
        [&](unsigned version, TransportCapabilities& caps) {
            queried = exchange(control.Get(), TransportProperty::Capabilities, 0, &caps,
                               sizeof(caps), false, version);
            if (SUCCEEDED(queried))
                return CapabilityQueryResult::Supported;
            if (queried == E_INVALIDARG || queried == E_NOTIMPL ||
                queried == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED))
                return CapabilityQueryResult::Unsupported;
            return CapabilityQueryResult::Failed;
        },
        configuration, preferFloat);
    if (negotiated != TransportNegotiationResult::Ready) {
        if (negotiated == TransportNegotiationResult::QueryFailed)
            report(queried, "Query driver transport", error);
        else
            error = "The virtual microphone driver requires an update";
        return false;
    }
    TransportConnection connection{};
    if (!report(exchange(control.Get(), TransportProperty::Connect, 0, &connection,
                         sizeof(connection), false, configuration.version),
                "Connect private audio transport", error))
        return false;
    if (!connection.generation) {
        error = "Driver returned an invalid audio session";
        return false;
    }
    impl_->control = std::move(control);
    impl_->generation = connection.generation;
    impl_->configuration = configuration;
    return true;
}
AudioFormat DriverTransport::format() const noexcept {
    return impl_->configuration.format;
}
unsigned DriverTransport::version() const noexcept {
    return impl_->configuration.version;
}
unsigned DriverTransport::sampleBytes() const noexcept {
    return impl_->configuration.sampleBytes;
}
unsigned DriverTransport::frameBytes() const noexcept {
    return impl_->configuration.frameBytes();
}
unsigned DriverTransport::packetFrames() const noexcept {
    return impl_->configuration.packetFrames;
}
bool DriverTransport::diagnostics(DuplexBridge::Diagnostics& value) {
    value = {};
    return impl_->control &&
           SUCCEEDED(exchange(impl_->control.Get(), TransportProperty::Diagnostics,
                              impl_->generation, &value, sizeof(value), false,
                              impl_->configuration.version));
}
bool DriverTransport::read(DuplexBridge::Packet& packet, std::string& error) {
    packet = {};
    if (!impl_->control) {
        error = "Private audio transport is disconnected";
        return false;
    }
    if (!report(exchange(impl_->control.Get(), TransportProperty::Playback, impl_->generation,
                         &packet, sizeof(packet), false, impl_->configuration.version),
                "Read playback audio", error))
        return false;
    if (packet.frames > packetFrames() || packet.discontinuity > 1) {
        error = "Driver returned an invalid playback packet";
        packet = {};
        return false;
    }
    return true;
}
bool DriverTransport::write(const unsigned char* samples, unsigned frames, std::string& error) {
    if (!impl_->control || !samples || !frames || frames > packetFrames()) {
        error = "Invalid processed audio packet";
        return false;
    }
    TransportProcessed packet{};
    packet.frames = frames;
    std::memcpy(packet.samples, samples, static_cast<size_t>(frames) * frameBytes());
    return report(exchange(impl_->control.Get(), TransportProperty::Processed, impl_->generation,
                           &packet, sizeof(packet), true, impl_->configuration.version),
                  "Publish microphone audio", error);
}
} // namespace loopz::windows
