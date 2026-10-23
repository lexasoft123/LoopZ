#include "broker_deploy.hpp"
#include <windows.h>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void rejects(F operation) {
    bool rejected{};
    try {
        operation();
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "Unsafe deployment accepted");
}
void write(const std::filesystem::path& path, const char* value) {
    std::ofstream stream(path, std::ios::binary);
    stream << value;
    require(bool(stream), "Cannot write test payload");
}
std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
void restrictedAccessDenied(const std::filesystem::path& binary) {
    struct Token {
        HANDLE value{};
        ~Token() {
            if (value)
                CloseHandle(value);
        }
    } token, restricted;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &token.value),
            "Cannot read administrative token");
    alignas(DWORD) std::array<BYTE, SECURITY_MAX_SID_SIZE> administrators{};
    DWORD size = static_cast<DWORD>(administrators.size());
    require(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators.data(), &size),
            "Cannot create administrator SID");
    SID_AND_ATTRIBUTES disabled{administrators.data(), 0};
    require(CreateRestrictedToken(token.value, DISABLE_MAX_PRIVILEGE, 1, &disabled, 0, nullptr, 0,
                                  nullptr, &restricted.value),
            "Cannot restrict token");
    for (const DWORD access :
         {DWORD(GENERIC_READ), DWORD(GENERIC_WRITE), DWORD(DELETE), DWORD(WRITE_DAC)}) {
        require(ImpersonateLoggedOnUser(restricted.value), "Cannot enter restricted context");
        const auto file = CreateFileW(binary.c_str(), access,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, 0, nullptr);
        const auto error = GetLastError();
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
        if (!RevertToSelf())
            std::terminate();
        require(file == INVALID_HANDLE_VALUE && error == ERROR_ACCESS_DENIED,
                "Restricted token accessed protected executable");
    }
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    using namespace loopz::windows;
    require(argc == 2, "Expected unique fixture directory");
    const std::filesystem::path fixture(argv[1]);
    require(!std::filesystem::exists(fixture), "Fixture already exists");
    std::filesystem::create_directory(fixture);
    const auto source = fixture / L"payload";
    const auto root = fixture / L"protected";
    const auto binary = root / L"LoopZDeviceBroker.exe";
    constexpr auto abc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    constexpr auto abcd = "88d4266fd4e6338d13b845fcf289579d209c897823b9217da3e161936f031589";
    const auto memoryRoot = fixture / L"protected-memory";
    const auto bytes = [](std::string_view text) {
        return std::as_bytes(std::span(text.data(), text.size()));
    };
    rejects([&] {
        stageBrokerBytes(bytes("abc"), abcd, memoryRoot);
    });
    rejects([&] {
        stageBrokerBytes({}, abc, memoryRoot);
    });
    require(!std::filesystem::exists(memoryRoot), "Invalid memory payload mutated destination");
    require(stageBrokerBytes(bytes("abc"), abc, memoryRoot), "Memory payload not installed");
    require(!stageBrokerBytes(bytes("abc"), abc, memoryRoot), "Memory no-op failed");
    require(stageBrokerBytes(bytes("abcd"), abcd, memoryRoot), "Memory replacement failed");
    require(read(memoryRoot / L"LoopZDeviceBroker.exe") == "abcd", "Memory copy corrupt");
    restrictedAccessDenied(memoryRoot / L"LoopZDeviceBroker.exe");
    write(source, "abc");
    rejects([&] {
        stageBrokerExecutable(source, abcd, root);
    });
    require(!std::filesystem::exists(root), "Bad digest mutated destination");
    require(stageBrokerExecutable(source, abc, root), "First copy did not occur");
    require(read(binary) == "abc", "First copy corrupt");
    restrictedAccessDenied(binary);
    require(!stageBrokerExecutable(source, abc, root), "Identical copy was replaced");
    write(source, "abcd");
    const auto held = CreateFileW(binary.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
    require(held != INVALID_HANDLE_VALUE, "Cannot hold installed payload");
    rejects([&] {
        stageBrokerExecutable(source, abcd, root);
    });
    CloseHandle(held);
    require(read(binary) == "abc", "Locked update damaged installed payload");
    size_t count{};
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        (void)entry;
        ++count;
    }
    require(count == 1, "Failed update leaked staging payload");
    write(root / L"shadow.dll", "unexpected");
    rejects([&] {
        stageBrokerExecutable(source, abcd, root);
    });
    require(read(binary) == "abc", "Unknown file refusal damaged installed payload");
    std::filesystem::remove(root / L"shadow.dll");
    require(stageBrokerExecutable(source, abcd, root), "Update did not occur");
    require(read(binary) == "abcd", "Updated payload corrupt");
    require(!stageBrokerExecutable(source, abcd, root), "Updated no-op failed");
    const auto alias = fixture / L"alias";
    std::filesystem::create_hard_link(binary, alias);
    rejects([&] {
        stageBrokerExecutable(source, abcd, root);
    });
    require(read(binary) == "abcd", "Hard-link refusal damaged payload");
    std::filesystem::remove(alias);
    restrictedAccessDenied(binary);
    std::filesystem::remove_all(fixture);
    std::cout << "PASS: digest-before-mutation, protected copy, no-op, locked update, unknown-file "
                 "refusal, restricted-token denial, hard-link refusal, replacement and cleanup\n";
}
