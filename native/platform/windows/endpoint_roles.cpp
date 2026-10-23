#include "endpoint_roles.hpp"
#include <windows.h>
#include <cfgmgr32.h>
#include <devicetopology.h>
#include <initguid.h>
#include <devpkey.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <propvarutil.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl.h>
#include <array>
#include <vector>
#include <cwchar>
#include <cwctype>
#include <stdexcept>

namespace loopz::windows {
namespace {
using Microsoft::WRL::ComPtr;
bool ownedAdapter(std::wstring_view instance, std::string& mixKey) {
    std::wstring id(instance);
    DEVINST node{};
    if (CM_Locate_DevNodeW(&node, id.data(), CM_LOCATE_DEVNODE_PHANTOM) != CR_SUCCESS)
        return false;
    for (unsigned depth = 0; depth < 8; ++depth) {
        std::array<wchar_t, 2048> ids{};
        ULONG size = static_cast<ULONG>(sizeof(ids)), type{};
        if (CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_HARDWAREID, &type, ids.data(), &size,
                                              0) == CR_SUCCESS &&
            type == REG_MULTI_SZ) {
            // The buffer is zero-initialized; require a fully bounded MULTI_SZ.
            ids.back() = 0;
            for (size_t offset = 0; offset < ids.size() && ids[offset];) {
                const size_t length = wcsnlen(ids.data() + offset, ids.size() - offset);
                if (length == ids.size() - offset)
                    break;
                if (_wcsicmp(ids.data() + offset, L"ROOT\\LoopZAudio") == 0) {
                    std::array<wchar_t, MAX_DEVICE_ID_LEN> adapterInstance{};
                    if (CM_Get_Device_IDW(node, adapterInstance.data(),
                                          static_cast<ULONG>(adapterInstance.size()),
                                          0) == CR_SUCCESS) {
                        std::wstring normalized(adapterInstance.data());
                        for (auto& c : normalized)
                            c = static_cast<wchar_t>(towlower(c));
                        constexpr std::wstring_view prefix = L"root\\loopzaudio\\lz_";
                        if (normalized.starts_with(prefix)) {
                            const auto key = normalized.substr(prefix.size());
                            if (key.size() == 64 &&
                                key.find_first_not_of(L"0123456789abcdef") == std::wstring::npos) {
                                for (const auto c : key)
                                    mixKey.push_back(static_cast<char>(c));
                            }
                        }
                    }
                    return true;
                }
                offset += length + 1;
            }
        }
        DEVINST parent{};
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS)
            break;
        node = parent;
    }
    return false;
}
bool endpointParent(std::wstring_view endpointId, EDataFlow flow, bool includeInactive,
                    std::string& mixKey) {
    // Windows AudioEndpoint software devnodes use the opaque Core Audio ID as
    // their MMDEVAPI instance component. Do not parse fields from that ID: ask
    // PnP for the canonical child, its class, flow interface and actual ancestry.
    constexpr std::wstring_view prefix = L"SWD\\MMDEVAPI\\";
    if (endpointId.empty() || endpointId.size() + prefix.size() >= MAX_DEVICE_ID_LEN ||
        endpointId.find(L'\0') != std::wstring_view::npos)
        return false;
    std::wstring instance(prefix);
    instance.append(endpointId);
    DEVINST node{};
    if (CM_Locate_DevNodeW(&node, instance.data(), CM_LOCATE_DEVNODE_PHANTOM) != CR_SUCCESS)
        return false;
    std::array<wchar_t, MAX_DEVICE_ID_LEN> canonical{};
    if (CM_Get_Device_IDW(node, canonical.data(), static_cast<ULONG>(canonical.size()), 0) !=
            CR_SUCCESS ||
        _wcsicmp(canonical.data(), instance.c_str()) != 0)
        return false;
    // AudioEndpoint setup class; older Windows SDK devguid.h omits its symbol.
    constexpr GUID audioEndpointClass{
        0xc166523c, 0xfe0c, 0x4a94, {0xa5, 0x86, 0xf1, 0xa8, 0x0c, 0xfb, 0xbf, 0x3e}};
    GUID deviceClass{};
    DEVPROPTYPE type{};
    ULONG bytes = sizeof(deviceClass);
    if (CM_Get_DevNode_PropertyW(node, &DEVPKEY_Device_ClassGuid, &type,
                                 reinterpret_cast<PBYTE>(&deviceClass), &bytes, 0) != CR_SUCCESS ||
        type != DEVPROP_TYPE_GUID || bytes != sizeof(deviceClass) ||
        !IsEqualGUID(deviceClass, audioEndpointClass))
        return false;
    GUID category = flow == eRender ? DEVINTERFACE_AUDIO_RENDER : DEVINTERFACE_AUDIO_CAPTURE;
    const auto flags = includeInactive ? CM_GET_DEVICE_INTERFACE_LIST_ALL_DEVICES
                                       : CM_GET_DEVICE_INTERFACE_LIST_PRESENT;
    ULONG characters{};
    if (CM_Get_Device_Interface_List_SizeW(&characters, &category, instance.data(), flags) !=
            CR_SUCCESS ||
        characters < 2 || characters > 32768)
        return false;
    std::vector<wchar_t> paths(characters, 0);
    if (CM_Get_Device_Interface_ListW(&category, instance.data(), paths.data(), characters,
                                      flags) != CR_SUCCESS ||
        paths.front() == 0)
        return false;
    return ownedAdapter(instance, mixKey);
}
std::string utf8(const wchar_t* value) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size < 2)
        return {};
    std::string text(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, text.data(), size, nullptr, nullptr);
    text.pop_back();
    return text;
}
} // namespace
std::map<std::string, std::string> audioDefaultEndpoints() {
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        throw std::runtime_error("Cannot initialize audio default-device query");
    struct Apartment {
        HRESULT result;
        ~Apartment() {
            if (SUCCEEDED(result))
                CoUninitialize();
        }
    } apartment{initialized};
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator))))
        throw std::runtime_error("Cannot enumerate audio default devices");
    std::map<std::string, std::string> result;
    constexpr std::array<const char*, 3> roles{"console", "multimedia", "communications"};
    for (const auto flow : {eRender, eCapture})
        for (unsigned role = 0; role < roles.size(); ++role) {
            const auto key = std::string(flow == eRender ? "render." : "capture.") + roles[role];
            ComPtr<IMMDevice> device;
            const auto found =
                enumerator->GetDefaultAudioEndpoint(flow, static_cast<ERole>(role), &device);
            if (found == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
                result[key] = "";
                continue;
            }
            if (FAILED(found))
                throw std::runtime_error("Cannot query audio default device");
            LPWSTR id{};
            if (FAILED(device->GetId(&id)))
                throw std::runtime_error("Cannot identify audio default device");
            const auto value = utf8(id);
            CoTaskMemFree(id);
            if (value.empty())
                throw std::runtime_error("Audio default device returned an empty identity");
            result[key] = value;
        }
    return result;
}
std::map<std::string, DriverEndpoint> loopzEndpoints(bool includeInactive) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct Apartment {
        HRESULT result;
        ~Apartment() {
            if (SUCCEEDED(result))
                CoUninitialize();
        }
    } apartment{initialized};
    std::map<std::string, DriverEndpoint> roles;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator))) ||
        FAILED(enumerator->EnumAudioEndpoints(
            eAll, includeInactive ? DEVICE_STATEMASK_ALL : DEVICE_STATE_ACTIVE, &devices))) {
        if (includeInactive)
            throw std::runtime_error("Cannot enumerate all virtual endpoint states");
        return roles;
    }
    UINT count{};
    if (FAILED(devices->GetCount(&count))) {
        if (includeInactive)
            throw std::runtime_error("Cannot count virtual endpoints");
        return roles;
    }
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        ComPtr<IPropertyStore> properties;
        ComPtr<IMMEndpoint> endpoint;
        if (FAILED(devices->Item(i, &device))) {
            if (includeInactive)
                throw std::runtime_error("Cannot inspect virtual endpoint");
            continue;
        }
        // Win10 endpoints may omit PKEY_Device_InstanceId. Follow the documented
        // topology connector to the adapter instead of parsing the endpoint UID.
        ComPtr<IDeviceTopology> topology;
        ComPtr<IConnector> connector;
        LPWSTR adapterId{};
        bool owned = false;
        std::string mixKey;
        if (SUCCEEDED(
                device->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, &topology)) &&
            SUCCEEDED(topology->GetConnector(0, &connector)) &&
            SUCCEEDED(connector->GetDeviceIdConnectedTo(&adapterId))) {
            std::array<wchar_t, MAX_DEVICE_ID_LEN> instance{};
            ULONG bytes = static_cast<ULONG>(sizeof(instance));
            DEVPROPTYPE type{};
            if (CM_Get_Device_Interface_PropertyW(adapterId, &DEVPKEY_Device_InstanceId, &type,
                                                  reinterpret_cast<PBYTE>(instance.data()), &bytes,
                                                  0) == CR_SUCCESS &&
                type == DEVPROP_TYPE_STRING) {
                instance.back() = 0;
                owned = ownedAdapter(instance.data(), mixKey);
            } else {
                ComPtr<IMMDevice> adapter;
                if (SUCCEEDED(enumerator->GetDevice(adapterId, &adapter)) &&
                    SUCCEEDED(adapter->OpenPropertyStore(STGM_READ, &properties))) {
                    PROPVARIANT value;
                    PropVariantInit(&value);
                    if (SUCCEEDED(properties->GetValue(PKEY_Device_InstanceId, &value)) &&
                        value.vt == VT_LPWSTR && value.pwszVal)
                        owned = ownedAdapter(value.pwszVal, mixKey);
                    PropVariantClear(&value);
                }
            }
            CoTaskMemFree(adapterId);
        }
        if (FAILED(device.As(&endpoint)))
            continue;
        EDataFlow flow{};
        LPWSTR id{};
        if (FAILED(endpoint->GetDataFlow(&flow)) || (flow != eRender && flow != eCapture) ||
            FAILED(device->GetId(&id)))
            continue;
        const bool topologyOwned = owned;
        std::string parentKey;
        const bool parentOwned = endpointParent(id, flow, includeInactive, parentKey);
        // Conflicting LoopZ identities must never select an arbitrary adapter.
        if (parentOwned && (!owned || mixKey == parentKey)) {
            owned = true;
            mixKey = std::move(parentKey);
        } else if (parentOwned)
            owned = false;
        if (owned)
            roles[utf8(id)] = {flow == eRender ? EndpointRole::Feed : EndpointRole::Microphone,
                               mixKey, topologyOwned, parentOwned};
        CoTaskMemFree(id);
    }
    return roles;
}
std::map<std::string, EndpointRole> loopzEndpointRoles() {
    std::map<std::string, EndpointRole> roles;
    for (const auto& [id, endpoint] : loopzEndpoints())
        roles[id] = endpoint.role;
    return roles;
}
EndpointRole endpointRole(const std::map<std::string, EndpointRole>& roles, std::string_view uid) {
    if (uid.starts_with("wasapi:"))
        uid.remove_prefix(7);
    const auto found = roles.find(std::string(uid));
    return found == roles.end() ? EndpointRole::Other : found->second;
}
} // namespace loopz::windows
