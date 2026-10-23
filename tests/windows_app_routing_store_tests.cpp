#include "app_routing_store.hpp"
#include <windows.h>
#include <objbase.h>
#include <winioctl.h>
#include <sddl.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <typename F> void rejected(F&& operation, const char* message) {
    bool threw = false;
    try {
        operation();
    } catch (const std::exception&) {
        threw = true;
    }
    require(threw, message);
}
struct Fixture {
    std::filesystem::path root;
    Fixture() {
        GUID id{};
        wchar_t text[40]{};
        require(SUCCEEDED(CoCreateGuid(&id)) && StringFromGUID2(id, text, 40), "Fixture identity");
        root = std::filesystem::temp_directory_path() / (std::wstring(L"LoopZ-store-") + text);
        require(std::filesystem::create_directory(root), "Fixture directory");
    }
    ~Fixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};
void junction(const std::filesystem::path& path, const std::filesystem::path& target) {
    require(std::filesystem::create_directory(path), "Junction directory");
    const HANDLE handle =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "Open junction fixture");
    struct MountPoint {
        DWORD tag;
        WORD dataLength, reserved, substituteOffset, substituteLength, printOffset, printLength;
        wchar_t names[2048];
    } data{};
    const auto substitute = std::wstring(L"\\??\\") + target.wstring();
    const auto printable = target.wstring();
    require(substitute.size() + printable.size() + 2 < 2048, "Junction fixture path length");
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substituteLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.printOffset = static_cast<WORD>(data.substituteLength + sizeof(wchar_t));
    data.printLength = static_cast<WORD>(printable.size() * sizeof(wchar_t));
    data.dataLength = static_cast<WORD>(8 + data.printOffset + data.printLength + sizeof(wchar_t));
    std::copy(substitute.begin(), substitute.end(), data.names);
    std::copy(printable.begin(), printable.end(), data.names + substitute.size() + 1);
    DWORD returned{};
    const bool ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data, 8 + data.dataLength,
                                    nullptr, 0, &returned, nullptr) != FALSE;
    CloseHandle(handle);
    require(ok, "Create junction fixture without elevation");
}
void permissive(const std::filesystem::path& path) {
    PSECURITY_DESCRIPTOR descriptor{};
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;OICI;FA;;;WD)", SDDL_REVISION_1, &descriptor, nullptr) != FALSE,
            "Fixture access descriptor");
    const bool ok =
        SetFileSecurityW(path.c_str(),
                         DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                         descriptor) != FALSE;
    LocalFree(descriptor);
    require(ok, "Set permissive fixture access");
}
void crashPrepare(const std::filesystem::path& root) {
    wchar_t executable[32768]{};
    require(GetModuleFileNameW(nullptr, executable, 32768) != 0, "Locate crash fixture");
    auto command =
        L"\"" + std::wstring(executable) + L"\" --crash-prepare \"" + root.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    require(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &startup, &child) != FALSE,
            "Start crash fixture");
    const auto waited = WaitForSingleObject(child.hProcess, 10000);
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(child.hProcess, 1);
        WaitForSingleObject(child.hProcess, 5000);
    }
    DWORD code{};
    const bool completed = GetExitCodeProcess(child.hProcess, &code) != FALSE;
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    require(waited == WAIT_OBJECT_0 && completed && code == 0, "Crash fixture did not commit");
}
struct Impersonation {
    Impersonation() {
        require(ImpersonateSelf(SecurityImpersonation) != FALSE, "Impersonate own test identity");
    }
    ~Impersonation() {
        RevertToSelf();
    }
};
} // namespace
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--crash-prepare") {
        loopz::windows::AppRoutingStore store(argv[2]);
        store.journal().prepare("crashed-owned-image", {"", "media", "communications"},
                                "virtual-feed");
        std::_Exit(0); // The OS closes pins; SQLite and C++ destructors do not run.
    }
    require(argc == 1, "Unexpected storage test arguments");
    using loopz::windows::AppRoutingStore;
    require(loopz::windows::appRoutingStoreRoot().filename() == L"LoopZRoutingRecovery",
            "Unexpected user storage location");
    Fixture fixture;
    const auto root = fixture.root / L"secure";
    const auto database = root / L"routing.sqlite3";
    uint64_t revision{};
    {
        AppRoutingStore store(root);
        require(store.journal().snapshot().empty(), "New journal not empty");
        const auto receipt = store.journal().prepare("owned-image", {"a", "", "c"}, "virtual-feed");
        revision = receipt.revision;
        require(!MoveFileExW(root.c_str(), (fixture.root / L"moved").c_str(), 0),
                "Live storage directory was replaceable");
        require(!MoveFileExW(database.c_str(), (root / L"moved.sqlite3").c_str(), 0),
                "Live database was replaceable");
        AppRoutingStore second(root);
        require(second.journal().snapshot().size() == 1, "Second connection lost receipt");
        require(second.journal().confirm("owned-image", revision, "virtual-feed"),
                "Concurrent journal confirmation failed");
    }
    {
        AppRoutingStore reopened(root, false);
        const auto records = reopened.journal().snapshot();
        require(records.size() == 1 &&
                    records[0].original == std::array<std::string, 3>{"a", "", "c"},
                "Original preference baseline did not persist");
        require(reopened.journal().forget("owned-image", revision), "Verified receipt cleanup");
    }
    const auto missing = fixture.root / L"missing-recovery";
    rejected(
        [&] {
            AppRoutingStore existing(missing, false);
        },
        "Recovery created a missing storage directory");
    require(!std::filesystem::exists(missing), "Recovery mutated a missing directory");
    {
        const auto empty = fixture.root / L"empty-recovery";
        {
            AppRoutingStore created(empty);
        }
        require(DeleteFileW((empty / L"routing.sqlite3").c_str()) != FALSE,
                "Remove empty recovery database");
        rejected(
            [&] {
                AppRoutingStore existing(empty, false);
            },
            "Recovery recreated a missing database");
        require(!std::filesystem::exists(empty / L"routing.sqlite3"),
                "Recovery mutated missing database");
    }
    const auto crashRoot = fixture.root / L"crashed";
    crashPrepare(crashRoot);
    {
        AppRoutingStore recovered(crashRoot);
        const auto records = recovered.journal().snapshot();
        require(records.size() == 1 && records[0].application == "crashed-owned-image" &&
                    records[0].original ==
                        std::array<std::string, 3>{"", "media", "communications"} &&
                    records[0].attemptedTargets == std::vector<std::string>{"virtual-feed"},
                "Committed receipt lost after process exit without destructors");
    }
    {
        Impersonation impersonation;
        rejected(
            [&] {
                AppRoutingStore store(root);
            },
            "Impersonating thread accepted");
    }
    const auto alias = fixture.root / L"database-alias";
    require(CreateHardLinkW(alias.c_str(), database.c_str(), nullptr) != FALSE, "Create hard link");
    rejected(
        [&] {
            AppRoutingStore store(root);
        },
        "Hard-linked database accepted");
    require(DeleteFileW(alias.c_str()) != FALSE, "Remove hard link");
    const auto redirect = fixture.root / L"redirect";
    junction(redirect, root);
    rejected(
        [&] {
            AppRoutingStore store(redirect);
        },
        "Root junction accepted");
    rejected(
        [&] {
            AppRoutingStore store(redirect / L"child");
        },
        "Ancestor junction accepted");
    require(!std::filesystem::exists(root / L"child"), "Redirected ancestor mutated target");
    // Avoid a generic recursive cleanup ever traversing a junction.
    require(RemoveDirectoryW(redirect.c_str()) != FALSE, "Remove junction fixture");
    const auto loose = fixture.root / L"loose";
    std::filesystem::create_directory(loose);
    permissive(loose);
    rejected(
        [&] {
            AppRoutingStore store(loose);
        },
        "Permissive directory accepted");
    require(!std::filesystem::exists(loose / L"routing.sqlite3"), "Rejected directory was mutated");
    const auto sidecar = root / L"routing.sqlite3-journal";
    {
        std::ofstream file(sidecar);
        file << "untrusted";
    }
    permissive(sidecar);
    rejected(
        [&] {
            AppRoutingStore store(root);
        },
        "Permissive recovery sidecar accepted");
    require(DeleteFileW(sidecar.c_str()) != FALSE, "Remove sidecar fixture");
    permissive(database);
    rejected(
        [&] {
            AppRoutingStore store(root);
        },
        "Permissive database accepted");
    rejected(
        [&] {
            AppRoutingStore store(root / L"..");
        },
        "Traversal path accepted");
    rejected(
        [&] {
            AppRoutingStore store(root.wstring() + L":stream");
        },
        "Alternate stream accepted");
    rejected(
        [&] {
            AppRoutingStore store(root.wstring() + L".");
        },
        "Aliased path accepted");
    const auto foreign = fixture.root / L"foreign";
    {
        AppRoutingStore store(foreign);
    }
    const auto foreignDatabase = foreign / L"routing.sqlite3";
    {
        std::ofstream file(foreignDatabase, std::ios::binary | std::ios::trunc);
        file << "foreign-data";
    }
    rejected(
        [&] {
            AppRoutingStore store(foreign);
        },
        "Foreign database accepted");
    {
        std::ifstream file(foreignDatabase, std::ios::binary);
        std::string text;
        std::getline(file, text);
        require(text == "foreign-data", "Foreign database overwritten");
    }
    std::cout
        << "PASS: user journal persistence, concurrent handles, rename pins, hard links, "
           "root/ancestor junctions, access policy, path aliases, foreign data, crash recovery and "
           "impersonation rejection\n";
}
