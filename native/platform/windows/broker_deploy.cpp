#include "broker_deploy.hpp"
#include "broker_security.hpp"
#include "broker_payload.hpp"
#include "device_intent_store.hpp"
#include "device_operations.hpp"
#include <bcrypt.h>
#include <sddl.h>
#include <objbase.h>
#include <array>
#include <stdexcept>
#include <functional>
#include <algorithm>

namespace loopz::windows {
namespace {
constexpr wchar_t temporaryPrefix[] = L"LoopZDeviceBroker.pending-";
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    void close() {
        const auto old = value;
        value = INVALID_HANDLE_VALUE;
        if (old != INVALID_HANDLE_VALUE && !CloseHandle(old))
            brokerProtectedObjectFailure("Close service payload");
    }
};
void rewind(HANDLE file) {
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN))
        brokerProtectedObjectFailure("Seek service payload");
}
std::string digest(HANDLE file) {
    struct Hash {
        BCRYPT_ALG_HANDLE algorithm{};
        BCRYPT_HASH_HANDLE value{};
        ~Hash() {
            if (value)
                BCryptDestroyHash(value);
            if (algorithm)
                BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    } hash;
    if (BCryptOpenAlgorithmProvider(&hash.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(hash.algorithm, &hash.value, nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("Cannot initialize service payload verification");
    rewind(file);
    std::array<unsigned char, 65536> buffer{};
    for (;;) {
        DWORD bytes{};
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr))
            brokerProtectedObjectFailure("Read service payload");
        if (!bytes)
            break;
        if (BCryptHashData(hash.value, buffer.data(), bytes, 0) < 0)
            throw std::runtime_error("Cannot hash service payload");
    }
    std::array<unsigned char, 32> value{};
    if (BCryptFinishHash(hash.value, value.data(), static_cast<ULONG>(value.size()), 0) < 0)
        throw std::runtime_error("Cannot finish service payload verification");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : value) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}
bool temporary(std::wstring_view name) {
    if (!name.starts_with(temporaryPrefix))
        return false;
    name.remove_prefix(std::size(temporaryPrefix) - 1);
    if (name.size() != 38 || name.front() != L'{' || name.back() != L'}')
        return false;
    for (size_t i = 1; i < 37; ++i) {
        const auto c = name[i];
        if (i == 9 || i == 14 || i == 19 || i == 24) {
            if (c != L'-')
                return false;
        } else if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') ||
                     (c >= L'A' && c <= L'F')))
            return false;
    }
    return true;
}
void ordinarySource(HANDLE file) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(file) != FILE_TYPE_DISK || !GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        info.nNumberOfLinks != 1)
        throw std::runtime_error("Service payload must be an ordinary single-link file");
    const auto size = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (!size || size > 64 * 1024 * 1024)
        throw std::runtime_error("Service payload size is invalid");
}
std::string normalizeDigest(std::string_view expected) {
    if (expected.size() != 64 ||
        expected.find_first_not_of("0123456789abcdefABCDEF") != std::string_view::npos)
        throw std::invalid_argument("Invalid service payload digest");
    std::string wanted(expected);
    for (auto& c : wanted)
        if (c >= 'A' && c <= 'F')
            c = static_cast<char>(c - 'A' + 'a');
    return wanted;
}
std::string digest(std::span<const std::byte> bytes) {
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot initialize embedded service verification");
    struct Algorithm {
        BCRYPT_ALG_HANDLE value;
        ~Algorithm() {
            BCryptCloseAlgorithmProvider(value, 0);
        }
    } close{algorithm};
    std::array<unsigned char, 32> value{};
    if (BCryptHash(
            algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),
            static_cast<ULONG>(bytes.size()), value.data(), static_cast<ULONG>(value.size())) < 0)
        throw std::runtime_error("Cannot verify embedded service payload");
    std::string result;
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : value) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}
bool publishBroker(const std::string& wanted, const std::filesystem::path& root,
                   const std::function<void(HANDLE)>& copy) {
    DeviceSetupLock lock;
    provisionDeviceIntentStore(root);
    Handle directory{CreateFileW(root.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                 nullptr)};
    if (directory.value == INVALID_HANDLE_VALUE)
        brokerProtectedObjectFailure("Open protected service directory");
    validateBrokerProtectedObject(directory.value, true);
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        const auto name = entry.path().filename().wstring();
        if (_wcsicmp(name.c_str(), L"LoopZDeviceBroker.exe") == 0)
            continue;
        if (!temporary(name))
            throw std::runtime_error("Unexpected file in service installation directory");
        Handle abandoned{CreateFileW(entry.path().c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                                     FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (abandoned.value == INVALID_HANDLE_VALUE)
            brokerProtectedObjectFailure("Inspect abandoned service payload");
        validateBrokerProtectedObject(abandoned.value, false);
        abandoned.close();
        if (!DeleteFileW(entry.path().c_str()))
            brokerProtectedObjectFailure("Remove abandoned service payload");
    }
    const auto destination = root / L"LoopZDeviceBroker.exe";
    {
        Handle current{CreateFileW(
            destination.c_str(), GENERIC_READ | READ_CONTROL | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (current.value != INVALID_HANDLE_VALUE) {
            validateBrokerProtectedObject(current.value, false);
            if (digest(current.value) == wanted)
                return false;
        } else if (GetLastError() != ERROR_FILE_NOT_FOUND)
            brokerProtectedObjectFailure("Inspect installed service payload");
    }
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid)))
        throw std::runtime_error("Cannot create service staging identity");
    std::array<wchar_t, 40> text{};
    if (!StringFromGUID2(guid, text.data(), static_cast<int>(text.size())))
        throw std::runtime_error("Cannot encode service staging identity");
    const auto staging = root / (std::wstring(temporaryPrefix) + text.data());
    struct Pending {
        std::filesystem::path path;
        Handle file;
        bool created{};
        bool published{};
        ~Pending() {
            if (created && !published) {
                if (file.value != INVALID_HANDLE_VALUE) {
                    CloseHandle(file.value);
                    file.value = INVALID_HANDLE_VALUE;
                }
                DeleteFileW(path.c_str());
            }
        }
    } pending{staging, {}, false, false};
    PSECURITY_DESCRIPTOR descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr))
        brokerProtectedObjectFailure("Prepare service payload permissions");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    pending.file.value = CreateFileW(
        staging.c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL, 0, &attributes, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, nullptr);
    LocalFree(descriptor);
    if (pending.file.value == INVALID_HANDLE_VALUE)
        brokerProtectedObjectFailure("Create protected service staging file");
    pending.created = true;
    validateBrokerProtectedObject(pending.file.value, false);
    copy(pending.file.value);
    if (!FlushFileBuffers(pending.file.value))
        brokerProtectedObjectFailure("Flush staged service payload");
    if (digest(pending.file.value) != wanted)
        throw std::runtime_error("Copied service payload failed verification");
    pending.file.close();
    if (!MoveFileExW(staging.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        brokerProtectedObjectFailure(
            "Replace service executable; stop the service before updating");
    pending.published = true;
    return true;
}
} // namespace
bool stageBrokerExecutable(const std::filesystem::path& source, std::string_view expected,
                           const std::filesystem::path& root) {
    const auto wanted = normalizeDigest(expected);
    Handle input{CreateFileW(source.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (input.value == INVALID_HANDLE_VALUE)
        brokerProtectedObjectFailure("Open service payload");
    ordinarySource(input.value);
    if (digest(input.value) != wanted)
        throw std::runtime_error("Service payload digest does not match trusted package metadata");
    return publishBroker(wanted, root, [&](HANDLE destination) {
        rewind(input.value);
        std::array<unsigned char, 65536> buffer{};
        for (;;) {
            DWORD bytes{}, written{};
            if (!ReadFile(input.value, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes,
                          nullptr))
                brokerProtectedObjectFailure("Read staged service payload");
            if (!bytes)
                break;
            if (!WriteFile(destination, buffer.data(), bytes, &written, nullptr) ||
                written != bytes)
                brokerProtectedObjectFailure("Write staged service payload");
        }
    });
}
bool stageBrokerBytes(std::span<const std::byte> bytes, std::string_view expected,
                      const std::filesystem::path& root) {
    const auto wanted = normalizeDigest(expected);
    if (bytes.empty() || bytes.size() > 64 * 1024 * 1024)
        throw std::invalid_argument("Invalid embedded service payload size");
    if (digest(bytes) != wanted)
        throw std::runtime_error("Embedded service payload digest mismatch");
    return publishBroker(wanted, root, [&](HANDLE destination) {
        size_t offset{};
        while (offset < bytes.size()) {
            const auto count = static_cast<DWORD>(std::min<size_t>(65536, bytes.size() - offset));
            DWORD written{};
            if (!WriteFile(destination, bytes.data() + offset, count, &written, nullptr) ||
                written != count)
                brokerProtectedObjectFailure("Write embedded service payload");
            offset += count;
        }
    });
}
bool stageEmbeddedBroker() {
    const auto payload = embeddedBrokerPayload();
    return stageBrokerBytes(payload.bytes, payload.sha256);
}
} // namespace loopz::windows
