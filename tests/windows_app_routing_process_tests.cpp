#include "app_routing_process.hpp"
#include "process_capture_group.hpp"
#include <fstream>
#include <algorithm>
#include <windows.h>
#include <objbase.h>
#include <filesystem>
#include <cstdlib>
#include <cwchar>
#include <limits>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void rejected(F&& operation, const char* message) {
    bool failed = false;
    try {
        operation();
    } catch (const std::exception&) {
        failed = true;
    }
    require(failed, message);
}
struct Fixture {
    std::filesystem::path root, first, second;
    Fixture() {
        GUID guid{};
        wchar_t text[40]{}, executable[32768]{};
        require(SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(guid, text, 40),
                "Create identity fixture ID");
        require(GetModuleFileNameW(nullptr, executable, 32768) != 0,
                "Locate identity fixture image");
        root = std::filesystem::temp_directory_path() / (std::wstring(L"LoopZ-process-") + text);
        first = root / L"\u043f\u0435\u0440\u0432\u044b\u0439-\u5e94\u7528" / L"marker.exe";
        second = root / L"second" / L"marker.exe";
        std::filesystem::create_directories(first.parent_path());
        std::filesystem::create_directories(second.parent_path());
        std::filesystem::copy_file(executable, first);
        std::filesystem::copy_file(executable, second);
    }
    ~Fixture() {
        const auto until = GetTickCount64() + 5000;
        do {
            std::error_code error;
            std::filesystem::remove_all(root, error);
            if (!error)
                break;
            Sleep(20);
        } while (GetTickCount64() < until);
    }
};
struct Child {
    PROCESS_INFORMATION process{};
    HANDLE job{};
    explicit Child(const std::filesystem::path& image, std::wstring arguments = L"--idle",
                   bool ownJob = true) {
        if (ownJob) {
            job = CreateJobObjectW(nullptr, nullptr);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                                 sizeof(limits))) {
                if (job)
                    CloseHandle(job);
                job = nullptr;
                throw std::runtime_error("Contain identity fixture child");
            }
        } else {
            BOOL contained = FALSE;
            require(IsProcessInJob(GetCurrentProcess(), nullptr, &contained) && contained,
                    "Surviving identity fixture requires its controller job");
        }
        auto command = L"\"" + image.wstring() + L"\" " + arguments;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        if (!CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                            &process)) {
            if (job)
                CloseHandle(job);
            job = nullptr;
            throw std::runtime_error("Start identity fixture child");
        }
        if ((job && !AssignProcessToJobObject(job, process.hProcess)) ||
            ResumeThread(process.hThread) == DWORD(-1)) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            if (job)
                CloseHandle(job);
            process = {};
            job = nullptr;
            throw std::runtime_error("Prepare identity fixture child");
        }
    }
    void stop() {
        require(TerminateProcess(process.hProcess, 0) &&
                    WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0,
                "Stop owned identity fixture child");
    }
    ~Child() {
        if (job)
            CloseHandle(job); // Stops only this exclusively owned child/job.
        if (process.hProcess) {
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hProcess);
        }
        if (process.hThread)
            CloseHandle(process.hThread);
    }
};
struct Impersonation {
    Impersonation() {
        require(ImpersonateSelf(SecurityImpersonation) != FALSE, "Impersonate test identity");
    }
    ~Impersonation() {
        if (!RevertToSelf())
            std::terminate();
    }
};
} // namespace
int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--idle") {
        Sleep(15000);
        return 0;
    }
    if (argc == 3 && (std::wstring_view(argv[1]) == L"--tree" ||
                      std::wstring_view(argv[1]) == L"--surviving-tree")) {
        wchar_t module[32768]{};
        require(GetModuleFileNameW(nullptr, module, 32768) != 0, "Locate tree fixture image");
        Child descendant(module, L"--idle", std::wstring_view(argv[1]) == L"--tree");
        {
            std::ofstream file{std::filesystem::path(argv[2])};
            file << descendant.process.dwProcessId;
        }
        Sleep(15000);
        return 0;
    }
    if (argc == 4 && std::wstring_view(argv[1]) == L"--inspect") {
        try {
            wchar_t* end{};
            const auto value = std::wcstoull(argv[2], &end, 10);
            require(value && value <= std::numeric_limits<uint32_t>::max() && end && *end == L'\0',
                    "Invalid inspection PID");
            loopz::windows::AppRoutingProcess observed(static_cast<uint32_t>(value), argv[3]);
            observed.validate();
            const auto& identity = observed.identity();
            loopz::windows::AppRoutingProcess rechecked(static_cast<uint32_t>(value), argv[3],
                                                        identity.application);
            rechecked.validate();
            std::cout << "{\"ok\":true,\"readOnly\":true,\"currentUserAndSessionVerified\":true,"
                         "\"recoveryKeyRechecked\":true,\"pid\":"
                      << identity.pid << ",\"session\":" << identity.session
                      << ",\"created100ns\":" << identity.created100ns
                      << ",\"packaged\":" << (!identity.packageFullName.empty() ? "true" : "false")
                      << ",\"appModelKey\":"
                      << (identity.application.find("|appmodel|") != std::string::npos ? "true"
                                                                                       : "false")
                      << ",\"hasApplicationUserModelId\":"
                      << (!identity.applicationUserModelId.empty() ? "true" : "false") << "}\n";
            return 0;
        } catch (const std::exception& error) {
            std::cout << "{\"ok\":false,\"readOnly\":true,\"rejected\":true}\n";
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    require(argc == 1, "Unexpected process identity test arguments");
    using loopz::windows::AppRoutingProcess;
    Fixture fixture;
    {
        const auto pidFile = fixture.root / L"descendant.pid";
        Child branch(fixture.first, L"--tree \"" + pidFile.wstring() + L"\"");
        Child independent(fixture.first);
        uint32_t descendant{};
        const auto deadline = GetTickCount64() + 5000;
        do {
            {
                std::ifstream file(pidFile);
                file >> descendant;
            }
            if (descendant)
                break;
            Sleep(20);
        } while (GetTickCount64() < deadline);
        require(descendant != 0, "Tree fixture did not publish descendant PID");
        std::vector<std::shared_ptr<AppRoutingProcess>> members{
            std::make_shared<AppRoutingProcess>(branch.process.dwProcessId, fixture.first),
            std::make_shared<AppRoutingProcess>(descendant, fixture.first),
            std::make_shared<AppRoutingProcess>(independent.process.dwProcessId, fixture.first)};
        const auto roots = loopz::windows::processCaptureRoots(members);
        require(
            roots.size() == 2 && std::none_of(roots.begin(), roots.end(),
                                              [&](const auto& root) {
                                                  return root.id == descendant;
                                              }),
            "Native capture planning duplicated a selected descendant or lost an independent root");
        auto duplicate = members;
        duplicate.push_back(members[0]);
        rejected(
            [&] {
                loopz::windows::processCaptureRoots(duplicate);
            },
            "Duplicate capture membership accepted");
        branch.stop();
        rejected(
            [&] {
                loopz::windows::processCaptureRoots(members);
            },
            "Dead capture membership accepted");
    }
    {
        const auto pidFile = fixture.root / L"surviving-descendant.pid";
        Child branch(fixture.first, L"--surviving-tree \"" + pidFile.wstring() + L"\"");
        Child independent(fixture.first);
        uint32_t descendant{};
        const auto until = GetTickCount64() + 5000;
        while (!descendant && GetTickCount64() < until) {
            std::ifstream file(pidFile);
            file >> descendant;
            if (!descendant)
                Sleep(10);
        }
        require(descendant != 0, "Surviving tree did not publish its child");
        std::vector<std::shared_ptr<loopz::windows::AppRoutingProcess>> members{
            std::make_shared<loopz::windows::AppRoutingProcess>(branch.process.dwProcessId,
                                                                fixture.first),
            std::make_shared<loopz::windows::AppRoutingProcess>(descendant, fixture.first),
            std::make_shared<loopz::windows::AppRoutingProcess>(independent.process.dwProcessId,
                                                                fixture.first)};
        auto original = std::make_unique<loopz::windows::ProcessCapturePlan>(members);
        loopz::windows::ProcessCapturePlan parentOnly({members[0]});
        require(original->roots().size() == 2, "Initial capture plan did not deduplicate child");
        branch.stop();
        members[1]->validate();
        const std::vector<std::shared_ptr<loopz::windows::AppRoutingProcess>> survivors{members[1],
                                                                                        members[2]};
        rejected(
            [&] {
                loopz::windows::ProcessCapturePlan unknown(survivors);
            },
            "Unknown missing ancestry was treated as independent capture roots");
        loopz::windows::ProcessCapturePlan recovered(survivors, original.get());
        loopz::windows::ProcessCapturePlan fromSingleRoot(survivors, &parentOnly);
        std::vector<loopz::ProcessInstance> expected;
        for (const auto& member : survivors)
            expected.push_back({member->identity().pid, member->identity().created100ns});
        std::sort(expected.begin(), expected.end(), [](const auto& a, const auto& b) {
            return a.id < b.id;
        });
        require(recovered.roots() == expected,
                "Verified surviving child was lost after parent exit");
        require(fromSingleRoot.roots() == expected,
                "Single-root history did not retain evidence for surviving descendants");
        original.reset();
        loopz::windows::ProcessCapturePlan next(survivors, &recovered);
        require(next.roots() == expected, "Ancestry evidence did not survive another refresh");
        loopz::windows::ProcessCapturePlan single({members[1]});
        require(single.roots().size() == 1 && single.roots()[0].id == descendant,
                "A single authenticated orphan requires no deduplication");
    }
    Child child(fixture.first);
    AppRoutingProcess identity(child.process.dwProcessId, fixture.first);
    identity.validate();
    const auto original = identity.identity();
    require(identity.livePid() == child.process.dwProcessId && original.created100ns &&
                original.ownerSid.starts_with(L"S-1-") && original.packageFullName.empty() &&
                original.application.find("|desktop|") != std::string::npos,
            "Invalid authenticated desktop process metadata");
    auto upper = fixture.first.wstring();
    require(CharUpperBuffW(upper.data(), static_cast<DWORD>(upper.size())) == upper.size(),
            "Uppercase selection alias");
    AppRoutingProcess alias(child.process.dwProcessId, upper, original.application);
    require(alias.identity().application == original.application,
            "Selection casing changed durable identity");
    rejected(
        [&] {
            AppRoutingProcess wrong(child.process.dwProcessId, fixture.second);
        },
        "Identical copied bytes at wrong selected path accepted");
    rejected(
        [&] {
            AppRoutingProcess wrong(child.process.dwProcessId, fixture.first,
                                    "another-application");
        },
        "Wrong recovery application accepted");
    rejected(
        [&] {
            AppRoutingProcess invalid(0, fixture.first);
        },
        "Invalid PID accepted");
    {
        Impersonation impersonation;
        rejected(
            [&] {
                identity.validate();
            },
            "Live process accepted under impersonation");
        rejected(
            [&] {
                AppRoutingProcess nested(child.process.dwProcessId, fixture.first);
            },
            "Process resolved under impersonation");
    }
    identity.validate();
    child.stop();
    rejected(
        [&] {
            identity.validate();
        },
        "Exited pinned process accepted");
    rejected(
        [&] {
            identity.livePid();
        },
        "Exited PID exposed as live");
    Child relaunched(upper);
    AppRoutingProcess next(relaunched.process.dwProcessId, fixture.first, original.application);
    require(next.identity().application == original.application &&
                next.identity().created100ns != original.created100ns &&
                next.livePid() != original.pid,
            "Relaunch did not preserve app identity with distinct process identity");
    rejected(
        [&] {
            identity.validate();
        },
        "Old process object followed relaunch");
    Child different(fixture.second);
    AppRoutingProcess other(different.process.dwProcessId, fixture.second);
    require(other.identity().application != original.application,
            "Same executable basename conflated different paths");
    rejected(
        [&] {
            AppRoutingProcess wrong(different.process.dwProcessId, fixture.second,
                                    original.application);
        },
        "Other executable rebound to old receipt");
    std::cout << "PASS: pinned user/session/process identity, Unicode/case selection, stable "
                 "relaunch key, "
                 "distinct paths, recovery key checks, dead process and impersonation rejection\n";
}
