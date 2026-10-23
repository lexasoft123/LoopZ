// Opt-in administrative live-driver fixture. Never registered with CTest.
// Refuses an existing sink and removes only its newly created owner-scoped node.
#include "device_operations.hpp"
#include "driver_identity.hpp"
#include "endpoint_roles.hpp"
#include "app_routing_sink_lifetime.hpp"
#include <loopz/document_json.hpp>
#include <devguid.h>
#include <devicetopology.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <propsys.h>
#include <propvarutil.h>
#include <functiondiscoverykeys_devpkey.h>
#include <array>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl.h>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <iostream>
#include <filesystem>
#include <thread>
#include <chrono>
using namespace loopz;
using namespace loopz::windows;
using Microsoft::WRL::ComPtr;
namespace {
void require(bool condition, const char* detail) {
    if (!condition)
        throw std::runtime_error(detail);
}
struct Devices {
    HDEVINFO value{SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_MEDIA, nullptr)};
    ~Devices() {
        if (value != INVALID_HANDLE_VALUE)
            SetupDiDestroyDeviceInfoList(value);
    }
};
std::string utf8(std::wstring_view value) {
    require(value.size() <= 32768, "Diagnostic endpoint ID is oversized");
    const auto length = static_cast<int>(value.size());
    const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length,
                                           nullptr, 0, nullptr, nullptr);
    require(bytes > 0, "Invalid diagnostic endpoint identity");
    std::string result(static_cast<size_t>(bytes), '\0');
    require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, result.data(),
                                bytes, nullptr, nullptr) == bytes,
            "Cannot encode diagnostic endpoint identity");
    return result;
}
JSON instanceProperty(IMMDevice* device) {
    JSON result;
    ComPtr<IPropertyStore> properties;
    const auto opened = device->OpenPropertyStore(STGM_READ, &properties);
    result["openStatus"] = opened;
    if (SUCCEEDED(opened)) {
        PROPVARIANT value;
        PropVariantInit(&value);
        const auto read = properties->GetValue(PKEY_Device_InstanceId, &value);
        result["readStatus"] = read;
        result["type"] = value.vt;
        if (SUCCEEDED(read) && value.vt == VT_LPWSTR && value.pwszVal)
            result["instance"] = utf8(value.pwszVal);
        PropVariantClear(&value);
        PropVariantInit(&value);
        const auto containerRead = properties->GetValue(PKEY_Device_ContainerId, &value);
        result["containerReadStatus"] = containerRead;
        result["containerType"] = value.vt;
        if (SUCCEEDED(containerRead) && value.vt == VT_CLSID && value.puuid) {
            std::array<wchar_t, 40> text{};
            if (StringFromGUID2(*value.puuid, text.data(), static_cast<int>(text.size())) > 0)
                result["container"] = utf8(text.data());
        }
        PropVariantClear(&value);
    }
    return result;
}
JSON topologyIdentity(IMMDevice* device, IMMDeviceEnumerator* enumerator) {
    JSON result;
    ComPtr<IDeviceTopology> topology;
    const auto activated =
        device->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, &topology);
    result["activateStatus"] = activated;
    if (FAILED(activated))
        return result;
    UINT connectorCount{};
    result["countStatus"] = topology->GetConnectorCount(&connectorCount);
    result["connectorCount"] = connectorCount;
    JSON connections = JSON::array();
    for (UINT index = 0; index < connectorCount && index < 16; ++index) {
        ComPtr<IConnector> current;
        JSON entry{{"index", index}};
        const auto opened = topology->GetConnector(index, &current);
        entry["openStatus"] = opened;
        if (SUCCEEDED(opened)) {
            ConnectorType kind{};
            BOOL live{};
            entry["typeStatus"] = current->GetType(&kind);
            entry["type"] = static_cast<unsigned>(kind);
            entry["isConnectedStatus"] = current->IsConnected(&live);
            entry["isConnected"] = live != FALSE;
            LPWSTR connectedId{};
            entry["connectedIdStatus"] = current->GetDeviceIdConnectedTo(&connectedId);
            if (connectedId) {
                entry["connectedId"] = utf8(connectedId);
                CoTaskMemFree(connectedId);
            }
        }
        connections.push_back(std::move(entry));
    }
    result["connectors"] = std::move(connections);
    ComPtr<IConnector> connector;
    const auto connected = topology->GetConnector(0, &connector);
    result["connectorStatus"] = connected;
    if (FAILED(connected))
        return result;
    LPWSTR adapterId{};
    const auto identified = connector->GetDeviceIdConnectedTo(&adapterId);
    result["connectedIdStatus"] = identified;
    if (FAILED(identified))
        return result;
    const std::wstring id(adapterId);
    CoTaskMemFree(adapterId);
    result["connectedId"] = utf8(id);
    std::array<wchar_t, 512> instance{};
    ULONG bytes = static_cast<ULONG>(sizeof(instance));
    DEVPROPTYPE type{};
    const auto queried =
        CM_Get_Device_Interface_PropertyW(id.c_str(), &DEVPKEY_Device_InstanceId, &type,
                                          reinterpret_cast<PBYTE>(instance.data()), &bytes, 0);
    result["interfacePropertyStatus"] = queried;
    result["interfacePropertyType"] = type;
    if (queried == CR_SUCCESS && type == DEVPROP_TYPE_STRING) {
        instance.back() = 0;
        result["interfaceInstance"] = utf8(instance.data());
        DEVINST node{};
        const auto located = CM_Locate_DevNodeW(&node, instance.data(), CM_LOCATE_DEVNODE_PHANTOM);
        result["locateStatus"] = located;
        JSON ancestry = JSON::array();
        if (located == CR_SUCCESS)
            for (unsigned depth = 0; depth < 8; ++depth) {
                std::array<wchar_t, MAX_DEVICE_ID_LEN> nodeId{};
                std::array<wchar_t, 2048> hardware{};
                ULONG hardwareBytes = static_cast<ULONG>(sizeof(hardware)), hardwareType{};
                JSON entry;
                if (CM_Get_Device_IDW(node, nodeId.data(), static_cast<ULONG>(nodeId.size()), 0) ==
                    CR_SUCCESS)
                    entry["instance"] = utf8(nodeId.data());
                const auto read = CM_Get_DevNode_Registry_PropertyW(
                    node, CM_DRP_HARDWAREID, &hardwareType, hardware.data(), &hardwareBytes, 0);
                entry["hardwareStatus"] = read;
                if (read == CR_SUCCESS && hardwareType == REG_MULTI_SZ) {
                    hardware.back() = 0;
                    entry["firstHardwareId"] = utf8(hardware.data());
                }
                ancestry.push_back(std::move(entry));
                DEVINST parent{};
                if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS)
                    break;
                node = parent;
            }
        result["ancestry"] = std::move(ancestry);
    }
    ComPtr<IMMDevice> adapter;
    const auto found = enumerator->GetDevice(id.c_str(), &adapter);
    result["adapterLookupStatus"] = found;
    if (SUCCEEDED(found))
        result["adapterProperty"] = instanceProperty(adapter.Get());
    return result;
}
JSON rootInterfaces(std::wstring instance) {
    GUID category = KSCATEGORY_AUDIO;
    ULONG characters{};
    const auto sized = CM_Get_Device_Interface_List_SizeW(&characters, &category, instance.data(),
                                                          CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    JSON result{{"sizeStatus", sized}};
    if (sized != CR_SUCCESS || characters == 0 || characters > 32768)
        return result;
    std::vector<wchar_t> interfaces(characters, 0);
    const auto listed =
        CM_Get_Device_Interface_ListW(&category, instance.data(), interfaces.data(), characters,
                                      CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    result["listStatus"] = listed;
    JSON values = JSON::array();
    if (listed == CR_SUCCESS)
        for (size_t offset = 0; offset < interfaces.size() && interfaces[offset];) {
            const auto length = wcsnlen(interfaces.data() + offset, interfaces.size() - offset);
            require(length < interfaces.size() - offset, "Unterminated interface identity");
            values.push_back(utf8(std::wstring_view(interfaces.data() + offset, length)));
            offset += length + 1;
        }
    result["interfaces"] = std::move(values);
    return result;
}
JSON pnpEndpointInventory(IMMDeviceEnumerator* enumerator) {
    JSON result = JSON::array();
    for (auto category : {DEVINTERFACE_AUDIO_RENDER, DEVINTERFACE_AUDIO_CAPTURE}) {
        ULONG characters{};
        const auto sized = CM_Get_Device_Interface_List_SizeW(
            &characters, &category, nullptr, CM_GET_DEVICE_INTERFACE_LIST_ALL_DEVICES);
        require(sized == CR_SUCCESS && characters > 0 && characters <= 32768,
                "Cannot size endpoint interfaces");
        std::vector<wchar_t> paths(characters, 0);
        require(CM_Get_Device_Interface_ListW(&category, nullptr, paths.data(), characters,
                                              CM_GET_DEVICE_INTERFACE_LIST_ALL_DEVICES) ==
                    CR_SUCCESS,
                "Cannot enumerate endpoint interfaces");
        for (size_t offset = 0; offset < paths.size() && paths[offset];) {
            const auto length = wcsnlen(paths.data() + offset, paths.size() - offset);
            require(length < paths.size() - offset, "Unterminated endpoint interface");
            const auto* path = paths.data() + offset;
            JSON entry{{"interface", utf8(std::wstring_view(path, length))}};
            std::array<wchar_t, 512> instance{};
            ULONG bytes = static_cast<ULONG>(sizeof(instance));
            DEVPROPTYPE type{};
            const auto read = CM_Get_Device_Interface_PropertyW(
                path, &DEVPKEY_Device_InstanceId, &type, reinterpret_cast<PBYTE>(instance.data()),
                &bytes, 0);
            entry["instanceStatus"] = read;
            if (read == CR_SUCCESS && type == DEVPROP_TYPE_STRING) {
                instance.back() = 0;
                entry["instance"] = utf8(instance.data());
                DEVINST node{};
                const auto located =
                    CM_Locate_DevNodeW(&node, instance.data(), CM_LOCATE_DEVNODE_PHANTOM);
                entry["locateStatus"] = located;
                if (located == CR_SUCCESS) {
                    std::array<wchar_t, 512> parent{};
                    bytes = static_cast<ULONG>(sizeof(parent));
                    if (CM_Get_DevNode_PropertyW(node, &DEVPKEY_Device_Parent, &type,
                                                 reinterpret_cast<PBYTE>(parent.data()), &bytes,
                                                 0) == CR_SUCCESS &&
                        type == DEVPROP_TYPE_STRING)
                        entry["parent"] = utf8(parent.data());
                }
            }
            ComPtr<IMMDevice> device;
            const auto found = enumerator->GetDevice(path, &device);
            entry["getDeviceStatus"] = found;
            if (SUCCEEDED(found)) {
                LPWSTR id{};
                const auto identified = device->GetId(&id);
                entry["getIdStatus"] = identified;
                if (SUCCEEDED(identified) && id) {
                    entry["uid"] = utf8(id);
                    CoTaskMemFree(id);
                }
            }
            result.push_back(std::move(entry));
            offset += length + 1;
        }
    }
    return result;
}
JSON rawEndpointInventory() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    require(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       IID_PPV_ARGS(&enumerator))),
            "Cannot open diagnostic enumerator");
    ComPtr<IMMDeviceCollection> collection;
    require(SUCCEEDED(enumerator->EnumAudioEndpoints(eAll, DEVICE_STATEMASK_ALL, &collection)),
            "Cannot enumerate diagnostic endpoints");
    UINT count{};
    require(SUCCEEDED(collection->GetCount(&count)), "Cannot count diagnostic endpoints");
    JSON result = JSON::array();
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        require(SUCCEEDED(collection->Item(index, &device)), "Cannot inspect diagnostic endpoint");
        LPWSTR id{};
        require(SUCCEEDED(device->GetId(&id)), "Cannot identify diagnostic endpoint");
        const std::wstring wideId(id);
        CoTaskMemFree(id);
        DWORD state{};
        require(SUCCEEDED(device->GetState(&state)), "Cannot query diagnostic endpoint state");
        ComPtr<IMMEndpoint> endpoint;
        EDataFlow flow{};
        require(SUCCEEDED(device.As(&endpoint)) && SUCCEEDED(endpoint->GetDataFlow(&flow)),
                "Cannot query diagnostic endpoint flow");
        ComPtr<IAudioClient> diagnosticClient;
        JSON engine;
        const auto activated =
            device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &diagnosticClient);
        engine["activateStatus"] = activated;
        if (SUCCEEDED(activated)) {
            WAVEFORMATEX* format{};
            const auto queried = diagnosticClient->GetMixFormat(&format);
            engine["formatStatus"] = queried;
            if (SUCCEEDED(queried) && format) {
                engine["rate"] = format->nSamplesPerSec;
                engine["channels"] = format->nChannels;
            }
            CoTaskMemFree(format);
        }
        result.push_back({{"uid", utf8(wideId)},
                          {"engine", std::move(engine)},
                          {"state", state},
                          {"flow", flow == eRender ? "render" : "capture"},
                          {"deviceProperty", instanceProperty(device.Get())},
                          {"topology", topologyIdentity(device.Get(), enumerator.Get())}});
    }
    return result;
}
JSON identifiedEndpointInventory() {
    JSON result = JSON::array();
    for (const auto& [uid, endpoint] : loopzEndpoints(true))
        result.push_back({{"uid", uid},
                          {"mixKey", endpoint.mixKey},
                          {"role", endpoint.role == EndpointRole::Feed ? "render" : "capture"}});
    return result;
}
void renderSilence(const std::string& uid, std::wstring_view owner) {
    // UID came only from hardware-verified sink ancestry, never from a default.
    const std::wstring id(uid.begin(), uid.end());
    ComPtr<IMMDeviceEnumerator> enumerator;
    require(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       IID_PPV_ARGS(&enumerator))),
            "Cannot open sink enumerator");
    ComPtr<IMMDevice> device;
    require(SUCCEEDED(enumerator->GetDevice(id.c_str(), &device)), "Sink disappeared");
    ComPtr<IAudioClient> client;
    require(SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)),
            "Cannot activate sink");
    WAVEFORMATEX* format{};
    require(SUCCEEDED(client->GetMixFormat(&format)) && format, "Cannot query sink format");
    struct Format {
        WAVEFORMATEX* value;
        ~Format() {
            CoTaskMemFree(value);
        }
    } release{format};
    require(format->nSamplesPerSec == 48000 && format->nChannels == 2, "Wrong sink format");
    require(SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, format, nullptr)),
            "Cannot initialize sink render stream");
    UINT32 frames{};
    require(SUCCEEDED(client->GetBufferSize(&frames)) && frames, "Cannot size sink buffer");
    ComPtr<IAudioRenderClient> render;
    require(SUCCEEDED(client->GetService(IID_PPV_ARGS(&render))), "Cannot open sink renderer");
    BYTE* bytes{};
    require(SUCCEEDED(render->GetBuffer(frames, &bytes)), "Cannot acquire sink buffer");
    require(SUCCEEDED(render->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT)),
            "Cannot submit silent sink buffer");
    const auto inactive = observeAppRoutingSink(owner);
    require(inactive.endpoint == uid && inactive.endpointsExamined && inactive.defaultsExamined &&
                inactive.occupied && !inactive.defaultEndpoint && inactive.error.empty(),
            "Inactive live render client did not retain sink lifetime");
    require(SUCCEEDED(client->Start()), "Cannot start sink stream");
    const auto active = observeAppRoutingSink(owner);
    require(active.occupied && active.endpointsExamined && active.defaultsExamined &&
                active.error.empty(),
            "Active silent render client did not retain sink lifetime");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    require(SUCCEEDED(client->Stop()), "Cannot stop sink stream");
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--pnp-inventory") {
        try {
            require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)),
                    "Cannot initialize inventory");
            struct Apartment {
                ~Apartment() {
                    CoUninitialize();
                }
            } apartment;
            ComPtr<IMMDeviceEnumerator> enumerator;
            require(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                               IID_PPV_ARGS(&enumerator))),
                    "Cannot open inventory");
            std::ofstream output(std::filesystem::path(argv[2]), std::ios::binary);
            output << pnpEndpointInventory(enumerator.Get()).dump(2) << '\n';
            output.close();
            require(static_cast<bool>(output), "Cannot save inventory");
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    const bool hold = argc == 6 && std::wstring_view(argv[1]) == L"--hold";
    if (!hold && (argc != 4 || std::wstring_view(argv[1]) != L"--run")) {
        std::cerr << "Use --run installed-INF report-path\n";
        return 2;
    }
    JSON report{{"ok", false}, {"cleanup", false}, {"checks", JSON::array()}};
    try {
        const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        require(SUCCEEDED(initialized), "Cannot initialize live sink fixture");
        struct Apartment {
            ~Apartment() {
                CoUninitialize();
            }
        } apartment;
        auto lock = std::make_unique<DeviceSetupLock>();
        if (hold)
            require(!std::filesystem::exists(argv[4]) && !std::filesystem::exists(argv[5]),
                    "Hold fixture requires fresh coordination paths");
        const auto owner = currentOwnerSid();
        const auto key = driverAppRoutingSinkKey(owner);
        const auto instance = driverInstance(key);
        report["sinkKey"] = key;
        const auto beforeDefaults = audioDefaultEndpoints();
        const auto beforeEndpoints = loopzEndpoints();
        report["rawBefore"] = rawEndpointInventory();
        Devices devices;
        require(devices.value != INVALID_HANDLE_VALUE, "Cannot open live fixture device set");
        SP_DEVINFO_DATA info{sizeof(info)};
        require(!SetupDiOpenDeviceInfoW(devices.value, instance.c_str(), nullptr, 0, &info) &&
                    GetLastError() == ERROR_NO_SUCH_DEVINST,
                "Sink already exists or absence cannot be established");
        try {
            const auto created = upsertAppRoutingSink(devices.value, owner, argv[2]);
            require(created.created && !created.rebootRequired,
                    "Sink was not newly created or needs reboot");
            report["rootStarted"] = deviceStarted(created.device);
            GUID container{};
            DEVPROPTYPE containerType{};
            DWORD containerBytes{};
            const auto containerRead = SetupDiGetDevicePropertyW(
                devices.value, const_cast<SP_DEVINFO_DATA*>(&created.device),
                &DEVPKEY_Device_ContainerId, &containerType, reinterpret_cast<PBYTE>(&container),
                sizeof(container), &containerBytes, 0);
            report["rootContainerRead"] = containerRead != FALSE;
            if (containerRead && containerType == DEVPROP_TYPE_GUID &&
                containerBytes == sizeof(container)) {
                std::array<wchar_t, 40> text{};
                require(StringFromGUID2(container, text.data(), static_cast<int>(text.size())) > 0,
                        "Cannot format root container");
                report["rootContainer"] = utf8(text.data());
            }
            finalizeAppRoutingSink(devices.value, owner);
            std::string feed;
            unsigned activeFeeds = 0;
            for (const auto& [uid, endpoint] : loopzEndpoints(true))
                if (endpoint.mixKey == key)
                    require(endpoint.role == EndpointRole::Feed, "Sink exposes a capture endpoint");
            for (const auto& [uid, endpoint] : loopzEndpoints())
                if (endpoint.mixKey == key) {
                    require(endpoint.role == EndpointRole::Feed,
                            "Sink exposes an active capture endpoint");
                    ++activeFeeds;
                    feed = uid;
                    report["parentIdentityVerified"] = endpoint.parentVerified;
                    report["topologyIdentityVerified"] = endpoint.topologyVerified;
                    require(endpoint.parentVerified,
                            "PnP parent path did not independently identify the test sink");
                }
            require(activeFeeds == 1 && !feed.empty(),
                    "Sink does not have exactly one active render endpoint");
            report["renderUid"] = feed;
            report["checks"].push_back("One hardware-identified render endpoint and no identified "
                                       "microphone across all endpoint states");
            const auto idle = observeAppRoutingSink(owner);
            require(idle.endpoint == feed && idle.endpointsExamined && idle.defaultsExamined &&
                        !idle.occupied && !idle.defaultEndpoint && idle.error.empty(),
                    "Idle hardware-identified sink lifetime is not completely examined");
            if (!hold) {
                renderSilence(feed, owner);
                report["sinkLifetimeVerified"] = true;
                report["checks"].push_back(
                    "Inactive live and active silent clients retain lifetime");
            } else {
                report["sinkLifetimeVerified"] = false;
                report["checks"].push_back(
                    "Hold mode opens no render session; desktop client owns routing proof");
            }
            const auto reused = upsertAppRoutingSink(devices.value, owner, argv[2]);
            require(!reused.created && !reused.rebootRequired,
                    "Sink retry unexpectedly recreated its adapter");
            finalizeAppRoutingSink(devices.value, owner);
            require(loopzEndpoints().contains(feed),
                    "Sink retry replaced render endpoint identity");
            report["checks"].push_back("Idempotent setup preserves render endpoint identity");
            if (hold) {
                // The normal-token fixture may provision ordinary mixes while
                // this exclusively owned new sink remains alive. Do not hold
                // the global device-setup mutex across that desktop work.
                lock.reset();
                {
                    std::ofstream ready(std::filesystem::path(argv[4]), std::ios::binary);
                    ready << JSON{{"endpoint", feed}, {"key", key}}.dump();
                    ready.close();
                    require(bool(ready), "Cannot publish owned sink readiness");
                }
                const auto deadline = GetTickCount64() + 180000;
                while (!std::filesystem::exists(argv[5]) && GetTickCount64() < deadline)
                    Sleep(100);
                const bool released = std::filesystem::exists(argv[5]);
                const auto lockDeadline = GetTickCount64() + 20000;
                while (!lock && GetTickCount64() < lockDeadline) {
                    try {
                        lock = std::make_unique<DeviceSetupLock>();
                    } catch (const std::exception&) {
                        Sleep(100);
                    }
                }
                require(bool(lock), "Cannot reacquire setup lock for owned sink cleanup");
                require(loopzEndpoints().contains(feed), "Held sink endpoint identity changed");
                require(released, "Desktop routing fixture did not release held sink in time");
                report["heldForDesktopAcceptance"] = true;
            }
            report["ok"] = true;
        } catch (const std::exception& error) {
            report["error"] = error.what();
            try {
                report["rootInterfacesAtFailure"] = rootInterfaces(instance);
                report["rawAtFailure"] = rawEndpointInventory();
                report["identifiedAtFailure"] = identifiedEndpointInventory();
            } catch (const std::exception& diagnostic) {
                report["diagnosticError"] = diagnostic.what();
            }
        }
        require(bool(lock), "No setup lock for sink cleanup");
        // Initial absence was verified under the setup lock. This API verifies
        // the exact protected owner and sink role before removing anything.
        const auto removed = removeAppRoutingSink(owner);
        require(!removed.rebootRequired, "Sink cleanup requires reboot");
        Devices fresh;
        require(fresh.value != INVALID_HANDLE_VALUE, "Cannot verify sink cleanup");
        SP_DEVINFO_DATA absent{sizeof(absent)};
        require(!SetupDiOpenDeviceInfoW(fresh.value, instance.c_str(), nullptr, 0, &absent) &&
                    GetLastError() == ERROR_NO_SUCH_DEVINST,
                "Sink root remains after cleanup");
        require(!removeAppRoutingSink(owner).existed, "Repeated sink removal is not idempotent");
        require(audioDefaultEndpoints() == beforeDefaults, "Sink test changed audio defaults");
        const auto afterEndpoints = loopzEndpoints();
        for (const auto& [uid, endpoint] : beforeEndpoints) {
            const auto found = afterEndpoints.find(uid);
            require(found != afterEndpoints.end() && found->second.role == endpoint.role &&
                        found->second.mixKey == endpoint.mixKey,
                    "Sink test replaced an existing LoopZ endpoint");
        }
        report["cleanup"] = true;
        report["checks"].push_back("Exact sink removed; repeated removal harmless; defaults and "
                                   "prior LoopZ endpoints preserved");
    } catch (const std::exception& error) {
        report["cleanupError"] = error.what();
    }
    std::ofstream output(std::filesystem::path(argv[3]), std::ios::binary);
    output << report.dump(2) << '\n';
    output.close();
    if (!output) {
        std::cerr << "Cannot save sink fixture report\n";
        return 1;
    }
    return report.at("ok").get<bool>() && report.at("cleanup").get<bool>() ? 0 : 1;
}
