#include "device_operations.hpp"
#include "driver/adapter_configuration.hpp"
#include "driver_identity.hpp"
#include "render_only_package.hpp"
#include "endpoint_format.hpp"
#include <span>
#include <algorithm>
#include <loopz/sample_rates.hpp>
#include <newdev.h>
#include <sddl.h>
#include <vector>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <devguid.h>
#include <array>
#include <stdexcept>

namespace loopz::windows {
namespace {
[[noreturn]] void fail(const char* operation) {
    throw std::runtime_error(std::string(operation) + " (Windows error " +
                             std::to_string(GetLastError()) + ")");
}
struct RegistryKey {
    HKEY value{};
    ~RegistryKey() {
        if (value && value != INVALID_HANDLE_VALUE)
            RegCloseKey(value);
    }
};
std::wstring wide(std::string_view text) {
    if (text.empty() || text.size() > 4096 || text.find('\0') != std::string_view::npos)
        throw std::invalid_argument("Invalid microphone text");
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (!count)
        throw std::invalid_argument("Invalid microphone UTF-8 text");
    std::wstring result(static_cast<size_t>(count), 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        result.data(), count);
    return result;
}
std::vector<BYTE> ownerIdentity(std::wstring_view owner) {
    const std::wstring ownerText(owner);
    struct Sid {
        PSID value{};
        ~Sid() {
            if (value)
                LocalFree(value);
        }
    } parsed;
    struct SidText {
        LPWSTR value{};
        ~SidText() {
            if (value)
                LocalFree(value);
        }
    } canonical;
    if (!ConvertStringSidToSidW(ownerText.c_str(), &parsed.value) ||
        !ConvertSidToStringSidW(parsed.value, &canonical.value) || ownerText != canonical.value)
        throw std::invalid_argument("Microphone owner must be a canonical SID");
    std::vector<BYTE> ownerBytes(GetLengthSid(parsed.value));
    if (!CopySid(static_cast<DWORD>(ownerBytes.size()), ownerBytes.data(), parsed.value))
        fail("Copy microphone owner");
    return ownerBytes;
}
bool ownerMatches(HKEY key, std::span<const BYTE> ownerBytes) {
    std::vector<BYTE> recorded(ownerBytes.size());
    DWORD type{}, bytes = static_cast<DWORD>(recorded.size());
    return RegQueryValueExW(key, L"LoopZOwnerSid", nullptr, &type, recorded.data(), &bytes) ==
               ERROR_SUCCESS &&
           type == REG_BINARY && bytes == ownerBytes.size() &&
           std::equal(recorded.begin(), recorded.end(), ownerBytes.begin(), ownerBytes.end());
}
} // namespace
DeviceSetupLock::DeviceSetupLock() {
    PSECURITY_DESCRIPTOR descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr))
        fail("Secure microphone setup lock");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    value_ = CreateMutexW(&attributes, FALSE, L"Global\\LoopZDriverSetup");
    const auto error = GetLastError();
    LocalFree(descriptor);
    if (!value_) {
        SetLastError(error);
        fail("Open microphone setup lock");
    }
    const auto waited = WaitForSingleObject(value_, 0);
    if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED) {
        CloseHandle(value_);
        value_ = nullptr;
        throw std::runtime_error("Another microphone setup is running. Retry after it finishes.");
    }
}
DeviceSetupLock::~DeviceSetupLock() {
    if (value_) {
        ReleaseMutex(value_);
        CloseHandle(value_);
    }
}
bool recordDeviceFormat(HKEY key, loopz_driver::AudioFormat format) {
    if (!format.valid())
        throw std::invalid_argument("Unsupported Windows microphone format");
    loopz_driver::AudioFormatRecord previous{}, desired{1, 0, format};
    DWORD type{}, bytes = sizeof(previous), pending{}, pendingBytes = sizeof(pending);
    const auto read = RegQueryValueExW(key, L"LoopZAudioFormat", nullptr, &type,
                                       reinterpret_cast<BYTE*>(&previous), &bytes);
    if (read != ERROR_SUCCESS && read != ERROR_FILE_NOT_FOUND && read != ERROR_MORE_DATA)
        throw std::runtime_error("Cannot read adapter configuration before microphone setup");
    // A routing sink (or a newer adapter configuration) is not a malformed
    // microphone format to overwrite. Refuse it before changing pending state.
    if (type == REG_BINARY &&
        (read == ERROR_MORE_DATA ||
         (read == ERROR_SUCCESS && (bytes != sizeof(previous) || previous.version != 1))))
        throw std::runtime_error("Adapter configuration is not a supported microphone mix");
    bool changed = read != ERROR_SUCCESS && read != ERROR_FILE_NOT_FOUND;
    changed = changed || (read == ERROR_SUCCESS &&
                          (type != REG_BINARY || bytes != sizeof(previous) || !previous.valid()));
    changed = changed || previous.format.sampleRate != format.sampleRate ||
              previous.format.channels != format.channels;
    const auto pendingRead = RegQueryValueExW(key, L"LoopZFormatPending", nullptr, &type,
                                              reinterpret_cast<BYTE*>(&pending), &pendingBytes);
    if (pendingRead != ERROR_FILE_NOT_FOUND &&
        (pendingRead != ERROR_SUCCESS || type != REG_DWORD || pendingBytes != sizeof(pending)))
        changed = true;
    changed = changed || pending != 0;
    if (changed) {
        const DWORD one = 1;
        if (RegSetValueExW(key, L"LoopZFormatPending", 0, REG_DWORD,
                           reinterpret_cast<const BYTE*>(&one), sizeof(one)) != ERROR_SUCCESS)
            throw std::runtime_error("Cannot record pending microphone format");
    }
    if (RegSetValueExW(key, L"LoopZAudioFormat", 0, REG_BINARY,
                       reinterpret_cast<const BYTE*>(&desired), sizeof(desired)) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot record microphone format");
    return changed;
}
bool recordAppRoutingSinkConfiguration(HKEY key, bool newlyCreated) {
    const auto desired = loopz_driver::appRoutingSinkRecord();
    loopz_driver::AudioFormatRecord previous{};
    DWORD type{}, bytes = sizeof(previous);
    const auto read = RegQueryValueExW(key, L"LoopZAudioFormat", nullptr, &type,
                                       reinterpret_cast<BYTE*>(&previous), &bytes);
    const bool missing = read == ERROR_FILE_NOT_FOUND;
    loopz_driver::AdapterConfiguration configuration;
    if (!(missing && newlyCreated) &&
        (read != ERROR_SUCCESS || type != REG_BINARY || bytes != sizeof(previous) ||
         !loopz_driver::decodeAdapterConfiguration(previous, configuration) ||
         configuration.mode != loopz_driver::AdapterEndpointMode::AppRoutingSink))
        throw std::runtime_error("Existing adapter is not a supported app routing sink");
    DWORD pending{}, pendingType{}, pendingBytes = sizeof(pending);
    const auto pendingRead = RegQueryValueExW(key, L"LoopZFormatPending", nullptr, &pendingType,
                                              reinterpret_cast<BYTE*>(&pending), &pendingBytes);
    if (pendingRead != ERROR_SUCCESS && pendingRead != ERROR_FILE_NOT_FOUND)
        throw std::runtime_error("Cannot read app routing sink pending state");
    if (pendingRead == ERROR_SUCCESS &&
        (pendingType != REG_DWORD || pendingBytes != sizeof(pending)))
        throw std::runtime_error("Invalid app routing sink pending state");
    const bool changed = missing || pending != 0;
    if (changed) {
        const DWORD one = 1;
        if (RegSetValueExW(key, L"LoopZFormatPending", 0, REG_DWORD,
                           reinterpret_cast<const BYTE*>(&one), sizeof(one)) != ERROR_SUCCESS)
            throw std::runtime_error("Cannot record pending app routing sink setup");
    }
    if (RegSetValueExW(key, L"LoopZAudioFormat", 0, REG_BINARY,
                       reinterpret_cast<const BYTE*>(&desired), sizeof(desired)) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot record app routing sink configuration");
    return changed;
}
namespace {
void requireSinkPackage(std::wstring_view installedInf) {
    // Before registering any node, reject packages whose kernel predates the
    // render-only record. An old kernel could otherwise expose a microphone.
    std::array<wchar_t, MAX_PATH> directory{};
    const auto count = GetWindowsDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
    if (!count || count >= directory.size())
        throw std::runtime_error("Cannot locate the installed driver package");
    const auto path = std::wstring(directory.data()) + L"\\INF\\" + std::wstring(installedInf);
    const auto inf = SetupOpenInfFileW(path.c_str(), L"MEDIA", INF_STYLE_WIN4, nullptr);
    if (inf == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot inspect app routing sink driver package");
    struct Inf {
        HINF value;
        ~Inf() {
            SetupCloseInfFile(value);
        }
    } close{inf};
    INFCONTEXT context{};
    std::array<wchar_t, 128> version{};
    if (!SetupFindFirstLineW(inf, L"Version", L"DriverVer", &context) ||
        !SetupGetStringFieldW(&context, 2, version.data(), static_cast<DWORD>(version.size()),
                              nullptr) ||
        !supportsRenderOnlyPackage(version.data()))
        throw std::runtime_error(
            "App routing sink requires the supported render-only driver package");
}
} // namespace
std::wstring installedDevicePackage(HDEVINFO set, SP_DEVINFO_DATA& device) {
    std::array<wchar_t, 1024> text{};
    DEVPROPTYPE type{};
    DWORD bytes{};
    if (!SetupDiGetDevicePropertyW(set, &device, &DEVPKEY_Device_DriverInfPath, &type,
                                   reinterpret_cast<PBYTE>(text.data()),
                                   static_cast<DWORD>(sizeof(text)), &bytes, 0)) {
        const auto error = GetLastError();
        if (error == ERROR_NOT_FOUND || error == ERROR_INVALID_DATA)
            return {};
        fail("Read installed microphone package");
    }
    if (type != DEVPROP_TYPE_STRING || bytes < sizeof(wchar_t) || bytes > sizeof(text) ||
        bytes % sizeof(wchar_t) || text[bytes / sizeof(wchar_t) - 1] != 0)
        throw std::runtime_error("Invalid installed microphone package property");
    return text.data();
}
bool deviceStarted(const SP_DEVINFO_DATA& device) {
    ULONG status{}, problem{};
    return CM_Get_DevNode_Status(&status, &problem, device.DevInst, 0) == CR_SUCCESS &&
           !(status & DN_HAS_PROBLEM) && (status & DN_STARTED);
}
bool isLoopZDevice(HDEVINFO set, SP_DEVINFO_DATA& device) {
    std::array<wchar_t, 2048> ids{};
    DWORD type{};
    if (!SetupDiGetDeviceRegistryPropertyW(set, &device, SPDRP_HARDWAREID, &type,
                                           reinterpret_cast<PBYTE>(ids.data()),
                                           static_cast<DWORD>(sizeof(ids)), nullptr) ||
        type != REG_MULTI_SZ)
        return false;
    for (size_t offset = 0; offset < ids.size() && ids[offset];) {
        const auto length = wcsnlen(ids.data() + offset, ids.size() - offset);
        if (length == ids.size() - offset)
            return false;
        if (_wcsicmp(ids.data() + offset, L"ROOT\\LoopZAudio") == 0)
            return true;
        offset += length + 1;
    }
    return false;
}
bool removeLoopZDevice(HDEVINFO set, SP_DEVINFO_DATA& device) {
    if (!isLoopZDevice(set, device))
        throw std::runtime_error("Refusing to remove an unrelated device");
    SP_REMOVEDEVICE_PARAMS params{};
    params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    params.ClassInstallHeader.InstallFunction = DIF_REMOVE;
    params.Scope = DI_REMOVEDEVICE_GLOBAL;
    if (!SetupDiSetClassInstallParamsW(set, &device, &params.ClassInstallHeader, sizeof(params)) ||
        !SetupDiCallClassInstaller(DIF_REMOVE, set, &device))
        fail("Remove LoopZ microphone");
    SP_DEVINSTALL_PARAMS_W installed{sizeof(installed)};
    if (!SetupDiGetDeviceInstallParamsW(set, &device, &installed))
        fail("Check microphone removal restart");
    return (installed.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)) != 0;
}
namespace {
DeviceUpsertResult upsertAdapter(HDEVINFO set, std::wstring_view owner,
                                 const VirtualDeviceDefinition& mix, std::wstring_view installedInf,
                                 bool allowPackageChange, bool sink) {
    const std::wstring publishedInf(installedInf), ownerText(owner);
    if (!publishedInf.starts_with(L"oem") || !publishedInf.ends_with(L".inf") ||
        publishedInf.size() < 8 ||
        publishedInf.substr(3, publishedInf.size() - 7).find_first_not_of(L"0123456789") !=
            std::wstring::npos)
        throw std::invalid_argument("Invalid installed driver package name");
    const auto ownerBytes = ownerIdentity(ownerText);
    if (sink)
        requireSinkPackage(publishedInf);
    const auto name = L"LoopZ — " + wide(mix.name);
    (void)wide(mix.id);
    constexpr wchar_t hardware[] = L"ROOT\\LoopZAudio\0";
    SP_DEVINFO_DATA device{sizeof(device)};
    bool created = false, infoCreated = false;
    BOOL reboot{};
    try {
        const auto instance = driverInstance(sink ? driverAppRoutingSinkKey(ownerText)
                                                  : driverMixKey(ownerText, mix.id));
        if (!SetupDiOpenDeviceInfoW(set, instance.c_str(), nullptr, 0, &device)) {
            if (GetLastError() != ERROR_NO_SUCH_DEVINST)
                fail("Open microphone");
            if (!SetupDiCreateDeviceInfoW(set, instance.c_str(), &GUID_DEVCLASS_MEDIA,
                                          sink ? L"LoopZ App routing" : L"LoopZ Microphone",
                                          nullptr, 0, &device))
                fail("Create microphone device information");
            infoCreated = true;
            if (!SetupDiSetDeviceRegistryPropertyW(set, &device, SPDRP_HARDWAREID,
                                                   reinterpret_cast<const BYTE*>(hardware),
                                                   sizeof(hardware)) ||
                !SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &device))
                fail("Register microphone");
            created = true;
        }
        if (!isLoopZDevice(set, device))
            throw std::runtime_error("Microphone identity belongs to another driver");
        // This protected per-instance value lets the kernel authorize
        // private transport without exposing other users' mix audio.
        RegistryKey deviceKey{SetupDiCreateDevRegKeyW(set, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV,
                                                      nullptr, nullptr)};
        if (deviceKey.value == INVALID_HANDLE_VALUE)
            fail("Open microphone ownership record");
        if (!created && !ownerMatches(deviceKey.value, ownerBytes))
            throw std::runtime_error(
                "Microphone belongs to another owner or has no valid owner record");
        const auto installed = installedDevicePackage(set, device);
        const bool differentPackage = _wcsicmp(installed.c_str(), publishedInf.c_str()) != 0;
        if (!allowPackageChange && !installed.empty() && differentPackage)
            throw std::runtime_error("Microphone driver maintenance is required before setup");
        const auto ownerRecorded =
            RegSetValueExW(deviceKey.value, L"LoopZOwnerSid", 0, REG_BINARY, ownerBytes.data(),
                           static_cast<DWORD>(ownerBytes.size()));
        if (ownerRecorded != ERROR_SUCCESS)
            throw std::runtime_error("Cannot record microphone ownership");
        const bool formatChanged =
            sink ? recordAppRoutingSinkConfiguration(deviceKey.value, created)
                 : recordDeviceFormat(deviceKey.value, {static_cast<unsigned>(mix.sampleRate),
                                                        mix.routing.outputChannels});
        if (!SetupDiSetDeviceRegistryPropertyW(
                set, &device, SPDRP_FRIENDLYNAME, reinterpret_cast<const BYTE*>(name.c_str()),
                static_cast<DWORD>((name.size() + 1) * sizeof(wchar_t))))
            fail("Name microphone");
        // Do not reinstall every ROOT\\LoopZAudio device when one mix
        // is added. Rebinding healthy adapters can invalidate recording
        // clients and regenerate their Windows endpoint IDs.
        if (installed.empty() ||
            (allowPackageChange && (differentPackage || !deviceStarted(device)))) {
            BOOL deviceReboot{};
            // The staged package participates in normal Windows ranking;
            // no selected-driver override or downgrade force is used.
            if (!DiInstallDevice(nullptr, set, &device, nullptr, 0, &deviceReboot))
                fail("Bind LoopZ microphone");
            reboot = reboot || deviceReboot;
        } else if (formatChanged || !deviceStarted(device)) {
            SP_PROPCHANGE_PARAMS change{};
            change.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
            change.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
            change.StateChange = DICS_PROPCHANGE;
            change.Scope = DICS_FLAG_GLOBAL;
            if (!SetupDiSetClassInstallParamsW(set, &device, &change.ClassInstallHeader,
                                               sizeof(change)) ||
                !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &device))
                fail("Apply microphone format");
            SP_DEVINSTALL_PARAMS_W parameters{};
            parameters.cbSize = sizeof(parameters);
            if (!SetupDiGetDeviceInstallParamsW(set, &device, &parameters))
                fail("Check microphone format restart");
            reboot = reboot || (parameters.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART));
        }

        if (!reboot &&
            _wcsicmp(installedDevicePackage(set, device).c_str(), publishedInf.c_str()) != 0)
            throw std::runtime_error("Windows selected a different microphone driver package");
        return {device, created, reboot != FALSE};
    } catch (...) {
        if (created) {
            try {
                removeLoopZDevice(set, device);
            } catch (...) {
            }
        } else if (infoCreated) {
            SetupDiDeleteDeviceInfo(set, &device);
        }
        throw;
    }
}
} // namespace
DeviceUpsertResult upsertMixDevice(HDEVINFO set, std::wstring_view owner,
                                   const VirtualDeviceDefinition& mix,
                                   std::wstring_view installedInf, bool allowPackageChange) {
    if (!supportedVirtualSampleRate(mix.sampleRate) ||
        !loopz_driver::AudioFormat{static_cast<unsigned>(mix.sampleRate),
                                   mix.routing.outputChannels}
             .valid() ||
        mix.id.empty() || mix.id.size() > 4096 || mix.name.size() > 4096)
        throw std::invalid_argument("Invalid Windows microphone definition");
    return upsertAdapter(set, owner, mix, installedInf, allowPackageChange, false);
}
DeviceUpsertResult upsertAppRoutingSink(HDEVINFO set, std::wstring_view owner,
                                        std::wstring_view installedInf, bool allowPackageChange) {
    VirtualDeviceDefinition sink;
    sink.id = "app-routing-sink"; // Display/setup text only; never used for identity.
    sink.name = "App routing";
    sink.sampleRate = 48000;
    sink.routing.outputChannels = 2;
    return upsertAdapter(set, owner, sink, installedInf, allowPackageChange, true);
}
void finalizeAppRoutingSink(HDEVINFO set, std::wstring_view owner) {
    const auto ownerBytes = ownerIdentity(owner);
    const auto key = driverAppRoutingSinkKey(owner);
    const auto instance = driverInstance(key);
    SP_DEVINFO_DATA device{sizeof(device)};
    if (!SetupDiOpenDeviceInfoW(set, instance.c_str(), nullptr, 0, &device))
        fail("Open started app routing sink");
    if (!isLoopZDevice(set, device))
        throw std::runtime_error("App routing sink identity belongs to another driver");
    RegistryKey registry{SetupDiOpenDevRegKey(set, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV,
                                              KEY_QUERY_VALUE | KEY_SET_VALUE)};
    if (registry.value == INVALID_HANDLE_VALUE || !ownerMatches(registry.value, ownerBytes))
        throw std::runtime_error("Cannot authorize app routing sink finalization");
    loopz_driver::AudioFormatRecord record{};
    loopz_driver::AdapterConfiguration configuration;
    DWORD type{}, bytes = sizeof(record);
    if (RegQueryValueExW(registry.value, L"LoopZAudioFormat", nullptr, &type,
                         reinterpret_cast<BYTE*>(&record), &bytes) != ERROR_SUCCESS ||
        type != REG_BINARY || bytes != sizeof(record) ||
        !loopz_driver::decodeAdapterConfiguration(record, configuration) ||
        configuration.exposesMicrophone())
        throw std::runtime_error("App routing sink configuration changed before finalization");
    if (!deviceStarted(device))
        throw std::runtime_error("Windows could not start the app routing sink driver");
    ensureAppRoutingSinkEndpointFormat(key);
    const DWORD zero{};
    if (RegSetValueExW(registry.value, L"LoopZFormatPending", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&zero), sizeof(zero)) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot complete app routing sink setup");
}
bool deviceOwnerMatches(HKEY key, std::wstring_view owner) {
    return ownerMatches(key, ownerIdentity(owner));
}
void finalizeMixDevice(HDEVINFO set, std::wstring_view owner, const VirtualDeviceDefinition& mix) {
    if (!supportedVirtualSampleRate(mix.sampleRate) ||
        !loopz_driver::AudioFormat{static_cast<unsigned>(mix.sampleRate),
                                   mix.routing.outputChannels}
             .valid())
        throw std::invalid_argument("Unsupported Windows microphone format");
    const auto ownerBytes = ownerIdentity(owner);
    (void)wide(mix.id);
    const auto key = driverMixKey(owner, mix.id);
    const auto instance = driverInstance(key);
    SP_DEVINFO_DATA device{sizeof(device)};
    if (!SetupDiOpenDeviceInfoW(set, instance.c_str(), nullptr, 0, &device))
        fail("Open started microphone");
    if (!isLoopZDevice(set, device))
        throw std::runtime_error("Microphone identity belongs to another driver");
    RegistryKey registry{SetupDiOpenDevRegKey(set, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV,
                                              KEY_QUERY_VALUE | KEY_SET_VALUE)};
    if (registry.value == INVALID_HANDLE_VALUE || !ownerMatches(registry.value, ownerBytes))
        throw std::runtime_error("Cannot authorize microphone format finalization");
    loopz_driver::AudioFormatRecord record{};
    DWORD type{}, bytes = sizeof(record);
    if (RegQueryValueExW(registry.value, L"LoopZAudioFormat", nullptr, &type,
                         reinterpret_cast<BYTE*>(&record), &bytes) != ERROR_SUCCESS ||
        type != REG_BINARY || bytes != sizeof(record) || !record.valid() ||
        record.format.sampleRate != mix.sampleRate ||
        record.format.channels != mix.routing.outputChannels)
        throw std::runtime_error("Microphone format changed before finalization");
    if (!deviceStarted(device))
        throw std::runtime_error("Windows could not start the microphone driver");
    ensureMixEndpointFormat(key, record.format);
    const DWORD zero{};
    if (RegSetValueExW(registry.value, L"LoopZFormatPending", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&zero), sizeof(zero)) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot complete microphone format change");
}
namespace {
DeviceRemovalResult removeAdapter(std::wstring_view owner, const std::string& key, bool sink) {
    const auto ownerBytes = ownerIdentity(owner);
    const auto instance = driverInstance(key);
    struct Inventory {
        HDEVINFO value{SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_MEDIA, nullptr)};
        ~Inventory() {
            if (value != INVALID_HANDLE_VALUE)
                SetupDiDestroyDeviceInfoList(value);
        }
    } inventory;
    if (inventory.value == INVALID_HANDLE_VALUE)
        fail("Open microphone removal device set");
    SP_DEVINFO_DATA device{sizeof(device)};
    if (!SetupDiOpenDeviceInfoW(inventory.value, instance.c_str(), nullptr, 0, &device)) {
        if (GetLastError() == ERROR_NO_SUCH_DEVINST)
            return {};
        fail("Open microphone for removal");
    }
    if (!isLoopZDevice(inventory.value, device))
        throw std::runtime_error("Microphone identity belongs to another driver");
    {
        RegistryKey registry{SetupDiOpenDevRegKey(inventory.value, &device, DICS_FLAG_GLOBAL, 0,
                                                  DIREG_DEV, KEY_QUERY_VALUE)};
        if (registry.value == INVALID_HANDLE_VALUE || !ownerMatches(registry.value, ownerBytes))
            throw std::runtime_error("Cannot authorize microphone removal");
        if (sink) {
            loopz_driver::AudioFormatRecord record{};
            loopz_driver::AdapterConfiguration configuration;
            DWORD type{}, bytes = sizeof(record);
            if (RegQueryValueExW(registry.value, L"LoopZAudioFormat", nullptr, &type,
                                 reinterpret_cast<BYTE*>(&record), &bytes) != ERROR_SUCCESS ||
                type != REG_BINARY || bytes != sizeof(record) ||
                !loopz_driver::decodeAdapterConfiguration(record, configuration) ||
                configuration.exposesMicrophone())
                throw std::runtime_error("Cannot remove an adapter with an unexpected sink role");
        }
    }
    return {true, removeLoopZDevice(inventory.value, device)};
}
} // namespace
DeviceRemovalResult removeMixDevice(std::wstring_view owner, std::string_view mixId) {
    (void)wide(mixId);
    return removeAdapter(owner, driverMixKey(owner, mixId), false);
}
DeviceRemovalResult removeAppRoutingSink(std::wstring_view owner) {
    return removeAdapter(owner, driverAppRoutingSinkKey(owner), true);
}
} // namespace loopz::windows
