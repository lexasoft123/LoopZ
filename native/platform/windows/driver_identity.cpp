#include "driver_identity.hpp"
#include <windows.h>
#include <sddl.h>
#include <bcrypt.h>
#include <array>
#include <stdexcept>
#include <vector>
namespace loopz::windows {
std::wstring currentOwnerSid() {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        throw std::runtime_error("Cannot identify microphone owner");
    DWORD size{};
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> bytes(size);
    const bool read = GetTokenInformation(token, TokenUser, bytes.data(), size, &size) != FALSE;
    CloseHandle(token);
    LPWSTR sid{};
    if (!read ||
        !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid))
        throw std::runtime_error("Cannot identify microphone owner");
    const std::wstring result(sid);
    LocalFree(sid);
    return result;
}
namespace {
std::string ownerPrefix(std::wstring_view ownerSid) {
    if (ownerSid.empty() || ownerSid.size() > 4096)
        throw std::invalid_argument("Invalid owner SID");
    // SIDs contain ASCII only. Include a delimiter to avoid ambiguous concatenation.
    std::string bytes;
    for (const auto c : ownerSid) {
        if (!c || c > 127)
            throw std::invalid_argument("Invalid owner SID");
        bytes.push_back(static_cast<char>(c));
    }
    bytes.push_back('\0');
    return bytes;
}
std::string identityHash(std::string_view bytes) {
    std::array<unsigned char, 32> digest{};
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot create microphone identity");
    const auto status = BCryptHash(
        algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
        static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0)
        throw std::runtime_error("Cannot create microphone identity");
    constexpr char hex[] = "0123456789abcdef";
    std::string key;
    for (const auto byte : digest) {
        key += hex[byte >> 4];
        key += hex[byte & 15];
    }
    return key;
}
} // namespace
std::string driverMixKey(std::wstring_view ownerSid, std::string_view mixId) {
    if (mixId.empty() || mixId.size() > 4096 || mixId.find('\0') != std::string_view::npos)
        throw std::invalid_argument("Invalid microphone mix identity");
    auto bytes = ownerPrefix(ownerSid);
    bytes.append(mixId);
    return identityHash(bytes);
}
std::string driverAppRoutingSinkKey(std::wstring_view ownerSid) {
    auto bytes = ownerPrefix(ownerSid);
    // Mix IDs reject NUL, so this cannot equal owner + NUL + a valid mix ID.
    bytes.push_back('\0');
    bytes.append("LoopZ.AppRoutingSink.v1");
    return identityHash(bytes);
}
std::wstring driverInstance(std::string_view key) {
    if (key.size() != 64 || key.find_first_not_of("0123456789abcdef") != std::string_view::npos)
        throw std::invalid_argument("Invalid microphone identity");
    return L"ROOT\\LOOPZAUDIO\\LZ_" + std::wstring(key.begin(), key.end());
}
} // namespace loopz::windows
