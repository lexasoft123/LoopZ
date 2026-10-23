#include <loopz/app_routing_journal.hpp>
#include <sqlite3.h>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace loopz;
namespace fs = std::filesystem;
template <class F> void rejects(F action) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    assert(rejected);
}
std::string bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
void rawSql(const fs::path& path, const char* sql) {
    sqlite3* db{};
    const auto name = path.u8string();
    assert(sqlite3_open(reinterpret_cast<const char*>(name.c_str()), &db) == SQLITE_OK);
    assert(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
    assert(sqlite3_close(db) == SQLITE_OK);
}
void crashPrepare(const fs::path& path) {
    AppRoutingJournal journal(path, true);
    journal.prepare("crashed-app", {"", "media-original", "communications-original"}, "feed-A");
    std::_Exit(0); // No C++ destructors/SQLite connection close after durable commit.
}
void committedProcessExit(const fs::path& path) {
#if defined(_WIN32)
    wchar_t executable[32768]{};
    assert(GetModuleFileNameW(nullptr, executable, 32768));
    auto command =
        L"\"" + std::wstring(executable) + L"\" --crash-prepare \"" + path.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    assert(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                          nullptr, nullptr, &startup, &child));
    const auto waited = WaitForSingleObject(child.hProcess, 10000);
    if (waited != WAIT_OBJECT_0)
        TerminateProcess(child.hProcess, 1);
    assert(waited == WAIT_OBJECT_0);
    DWORD code{};
    assert(GetExitCodeProcess(child.hProcess, &code) && code == 0);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
#else
    const auto child = fork();
    assert(child >= 0);
    if (!child)
        crashPrepare(path);
    int status{};
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
}
int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--crash-prepare")
        crashPrepare(fs::path(argv[2]));
    assert(argc == 1);
    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto folder =
        fs::canonical(fs::temp_directory_path()) / ("loopz-routing-journal-" + unique);
    assert(fs::create_directory(folder));
    const auto path = folder / fs::path(u8"журнал.sqlite3");
    {
        const auto legacy = folder / "legacy.sqlite3";
        rawSql(legacy,
               "PRAGMA application_id=1280983378; PRAGMA user_version=1;"
               "CREATE TABLE metadata(singleton INTEGER PRIMARY KEY, revision INTEGER) STRICT;"
               "INSERT INTO metadata VALUES(1,7);"
               "CREATE TABLE leases(application TEXT PRIMARY KEY,revision INTEGER,original0 TEXT,"
               "original1 TEXT,original2 TEXT) STRICT;"
               "CREATE TABLE targets(application TEXT REFERENCES leases(application) ON DELETE "
               "CASCADE,"
               "target TEXT,PRIMARY KEY(application,target)) STRICT;"
               "INSERT INTO leases VALUES('legacy',7,'','old','old');"
               "INSERT INTO targets VALUES('legacy','feed-A');");
        AppRoutingJournal migrated(legacy);
        const auto receipt = migrated.snapshot().at(0);
        assert(receipt.phase == AppRoutingReceiptPhase::Applied && receipt.revision == 7);
        assert(receipt.original[1] == "old" && receipt.attemptedTargets.at(0) == "feed-A");
    }

    const std::array<std::string, 3> original{"", "media-original", "communications-original"};
    uint64_t latest{};
    {
        rejects([&] {
            AppRoutingJournal missing(path);
        });
        assert(!fs::exists(path));
        AppRoutingJournal journal(path, true);
        assert(journal.snapshot().empty());
        const auto a = journal.prepare("application", original, "feed-A");
        const auto b = journal.prepare("application", original, "feed-B");
        const auto againA = journal.prepare("application", original, "feed-A");
        assert(a.revision < b.revision && b.revision < againA.revision);
        assert(!journal.forget("application", a.revision));
        assert(!journal.forget("application", b.revision));
        assert(againA.attemptedTargets == (std::vector<std::string>{"feed-A", "feed-B"}));
        latest = againA.revision;
        rejects([&] {
            journal.prepare("application", {"new", "", ""}, "feed-C");
        });
        rejects([&] {
            journal.prepare("application", original, "");
        });
        rejects([&] {
            journal.prepare(std::string("a\0b", 3), original, "feed-C");
        });
        assert(journal.snapshot().front().revision == latest);
        rejects([&] {
            journal.prepare("application", original, std::string(32769, 'x'));
        });
        const auto actions = appRoutingRestoration(againA, {"feed-A", "user-choice", original[2]});
        assert(actions[0] == AppRoutingRestore::RestoreOriginal);
        assert(actions[1] == AppRoutingRestore::PreserveUserChange);
        assert(actions[2] == AppRoutingRestore::Unchanged);
        assert(appRoutingRestoration(againA, {"feed-B", original[1], original[2]})[0] ==
               AppRoutingRestore::RestoreOriginal); // Ambiguous retarget setter outcome.
        assert(!journal.confirm("application", a.revision, "feed-A"));
        rejects([&] {
            journal.confirm("application", 0, "feed-A");
        });
        rejects([&] {
            journal.confirm("application", latest, "never-owned");
        });
        assert(journal.confirm("application", latest, "feed-A"));
        const auto confirmed = journal.snapshot().front();
        assert(confirmed.attemptedTargets == std::vector<std::string>{"feed-A"});
        assert(appRoutingRestoration(confirmed, {"feed-B", original[1], original[2]})[0] ==
               AppRoutingRestore::PreserveUserChange);
    }
    {
        AppRoutingJournal reopened(path);
        const auto saved = reopened.snapshot();
        assert(saved.size() == 1 && saved[0].revision == latest && saved[0].original == original);
        assert(reopened.forget("application", latest));
        assert(reopened.snapshot().empty());
        const auto next = reopened.prepare("application", original, "feed-A");
        assert(next.revision > latest && !reopened.forget("application", latest));
        assert(reopened.forget("application", next.revision));
    }
    const auto crashed = folder / "crash.sqlite3";
#if !defined(_WIN32)
    const auto linked = folder / "linked.sqlite3";
    fs::create_symlink(path, linked);
    const auto originalBytes = bytes(path);
    rejects([&] {
        AppRoutingJournal alias(linked);
    });
    assert(bytes(path) == originalBytes);
#endif
    committedProcessExit(crashed);
    {
        AppRoutingJournal recovered(crashed);
        const auto saved = recovered.snapshot();
        assert(saved.size() == 1 && saved[0].application == "crashed-app");
        assert(saved[0].original == original && saved[0].attemptedTargets[0] == "feed-A");
        const auto beforeSetter = appRoutingRestoration(saved[0], original);
        for (const auto action : beforeSetter)
            assert(action == AppRoutingRestore::Unchanged);
    }
    const auto foreign = folder / "foreign.sqlite3";
    rawSql(foreign, "PRAGMA application_id=1234; CREATE TABLE keep(value TEXT); INSERT INTO keep "
                    "VALUES('preserve');");
    const auto foreignBytes = bytes(foreign);
    rejects([&] {
        AppRoutingJournal wrong(foreign, true);
    });
    assert(bytes(foreign) == foreignBytes);
    rawSql(crashed, "UPDATE metadata SET revision=0");
    rejects([&] {
        AppRoutingJournal invalid(crashed);
    });
    fs::remove_all(folder);
    std::puts("App routing journal: durable receipts, retarget/late-ack safety, user overrides and "
              "foreign-file preservation passed");
}
