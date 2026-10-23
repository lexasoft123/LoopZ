// Opt-in administrative registry fixture; never changes SCM service settings.
#include "broker_installation.hpp"
#include <sddl.h>
#include <objbase.h>
#include <array>
#include <iostream>
#include <stdexcept>
#include <source_location>

using namespace loopz::windows;
namespace {
void require(bool value, std::source_location location = std::source_location::current()) {
    if (!value)
        throw std::runtime_error("Service installation regression failed at line " +
                                 std::to_string(location.line()) + "; Windows error " +
                                 std::to_string(GetLastError()));
}
template <class Operation>
void rejects(Operation operation, std::source_location location = std::source_location::current()) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, location);
}
struct Fixture {
    std::wstring path;
    HKEY key{};
    explicit Fixture(bool secure) {
        GUID guid{};
        require(SUCCEEDED(CoCreateGuid(&guid)));
        std::array<wchar_t, 40> text{};
        require(StringFromGUID2(guid, text.data(), static_cast<int>(text.size())) != 0);
        path = L"Software\\LoopZ\\Tests\\BrokerInstallation\\" + std::wstring(text.data());
        PSECURITY_DESCRIPTOR descriptor{};
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    L"O:BAG:BAD:P(A;;KA;;;SY)(A;;KA;;;BA)", SDDL_REVISION_1, &descriptor,
                    nullptr) != FALSE);
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
        DWORD disposition{};
        const auto opened =
            RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                            secure ? &attributes : nullptr, &key, &disposition);
        LocalFree(descriptor);
        require(opened == ERROR_SUCCESS);
        if (disposition != REG_CREATED_NEW_KEY) {
            RegCloseKey(key);
            key = nullptr;
            path.clear();
            throw std::runtime_error("Fixture already exists");
        }
    }
    ~Fixture() {
        if (key)
            RegCloseKey(key);
        if (!path.empty())
            RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    }
};
void restrictedDenied(const std::wstring& path) {
    struct Token {
        HANDLE value{};
        ~Token() {
            if (value)
                CloseHandle(value);
        }
    } token, restricted;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &token.value) !=
            FALSE);
    alignas(DWORD) std::array<BYTE, SECURITY_MAX_SID_SIZE> administrators{};
    DWORD size = static_cast<DWORD>(administrators.size());
    require(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators.data(),
                               &size) != FALSE);
    SID_AND_ATTRIBUTES disabled{administrators.data(), 0};
    require(CreateRestrictedToken(token.value, DISABLE_MAX_PRIVILEGE, 1, &disabled, 0, nullptr, 0,
                                  nullptr, &restricted.value) != FALSE);
    require(ImpersonateLoggedOnUser(restricted.value) != FALSE);
    HKEY key{};
    const auto opened = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_QUERY_VALUE, &key);
    if (opened == ERROR_SUCCESS)
        RegCloseKey(key);
    HKEY writable{};
    const auto writeOpened =
        RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_SET_VALUE, &writable);
    if (writeOpened == ERROR_SUCCESS)
        RegCloseKey(writable);
    if (!RevertToSelf())
        std::terminate();
    require(opened == ERROR_ACCESS_DENIED && writeOpened == ERROR_ACCESS_DENIED);
}
} // namespace
int main() {
    try {
        Fixture good(true), insecure(false);
        rejects([&] {
            (void)readBrokerInstallationKey(good.key);
        });
        writeBrokerInstallationKey(good.key, L"oem45.inf");
        require(readBrokerInstallationKey(good.key) == L"oem45.inf");
        writeBrokerInstallationKey(good.key, L"oem46.inf");
        for (const auto* bad :
             {L"", L"oem.inf", L"..\\oem1.inf", L"C:\\x.inf", L"oem-1.inf", L"OEM1.inf"}) {
            rejects([&] {
                writeBrokerInstallationKey(good.key, bad);
            });
            require(readBrokerInstallationKey(good.key) == L"oem46.inf");
        }
        rejects([&] {
            writeBrokerInstallationKey(good.key, std::wstring(L"oem1.inf\0x", 10));
        });
        restrictedDenied(good.path);
        rejects([&] {
            writeBrokerInstallationKey(insecure.key, L"oem45.inf");
        });
        DWORD bytes{};
        require(RegQueryValueExW(insecure.key, L"DriverInf", nullptr, nullptr, nullptr, &bytes) ==
                ERROR_FILE_NOT_FOUND);
        rejects([&] {
            (void)readBrokerInstallationKey(insecure.key);
        });
        const wchar_t valid[] = L"oem45.inf";
        require(RegSetValueExW(good.key, L"DriverInf", 0, REG_BINARY,
                               reinterpret_cast<const BYTE*>(valid),
                               sizeof(valid)) == ERROR_SUCCESS);
        rejects([&] {
            (void)readBrokerInstallationKey(good.key);
        });
        // Win32 can append a missing terminator. Use invalid content as well,
        // so this fixture does not assume the API preserves malformed raw bytes.
        const wchar_t invalidName[] = L"oem45.infx";
        require(RegSetValueExW(good.key, L"DriverInf", 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(invalidName),
                               sizeof(invalidName) - sizeof(wchar_t)) == ERROR_SUCCESS);
        rejects([&] {
            (void)readBrokerInstallationKey(good.key);
        });
        const std::wstring embedded(L"oem1.inf\0oem2.inf", 17);
        require(RegSetValueExW(good.key, L"DriverInf", 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(embedded.c_str()),
                               static_cast<DWORD>((embedded.size() + 1) * sizeof(wchar_t))) ==
                ERROR_SUCCESS);
        rejects([&] {
            (void)readBrokerInstallationKey(good.key);
        });
        const std::wstring oversized(100, L'x');
        require(RegSetValueExW(good.key, L"DriverInf", 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(oversized.c_str()),
                               static_cast<DWORD>((oversized.size() + 1) * sizeof(wchar_t))) ==
                ERROR_SUCCESS);
        rejects([&] {
            (void)readBrokerInstallationKey(good.key);
        });
        writeBrokerInstallationKey(good.key, L"oem45.inf");
        require(readBrokerInstallationKey(good.key) == L"oem45.inf");
        std::cout << "PASS: protected installer parameters, restricted access, package validation "
                     "and malformed configuration rejection\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
