#include <loopz/device_intent_database.hpp>
#include <sqlite3.h>
#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
using namespace loopz;
constexpr auto owner = "uid:501";
DeviceIntentRequest request(unsigned id, uint64_t revision, std::string mix = "mix",
                            DeviceIntentAction action = DeviceIntentAction::Upsert) {
    char uuid[37]{};
    std::snprintf(uuid, sizeof(uuid), "12345678-1234-1234-1234-%012u", id);
    DeviceIntentRequest result;
    result.operationId = uuid;
    result.expectedRevision = revision;
    result.mixId = std::move(mix);
    result.name = "Microphone";
    result.action = action;
    return result;
}
template <class F> void rejects(F&& action) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    assert(rejected);
}
JSON snapshot(const DeviceIntentSnapshot& state) {
    JSON result{{"revision", state.revision}, {"records", JSON::array()}};
    for (const auto& item : state.records)
        result["records"].push_back(encodeDeviceIntent(item));
    return result;
}
void raw(const std::filesystem::path& path, const char* sql) {
    sqlite3* db{};
    const auto name = path.u8string();
    assert(sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()), &db, SQLITE_OPEN_READWRITE,
                           nullptr) == SQLITE_OK);
    const auto code = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    sqlite3_close(db);
    assert(code == SQLITE_OK);
}
std::string queryText(const std::filesystem::path& path, const char* sql) {
    sqlite3* db{};
    const auto name = path.u8string();
    assert(sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()), &db, SQLITE_OPEN_READONLY,
                           nullptr) == SQLITE_OK);
    sqlite3_stmt* statement{};
    assert(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW);
    const std::string result(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)));
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return result;
}
[[noreturn]] void crashWrite(const std::filesystem::path& path) {
    sqlite3* db{};
    const auto name = path.u8string();
    if (sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()), &db, SQLITE_OPEN_READWRITE,
                        nullptr) != SQLITE_OK)
        std::_Exit(3);
    if (sqlite3_exec(
            db, "BEGIN IMMEDIATE; UPDATE owners SET revision=zeroblob(8); DELETE FROM desired;",
            nullptr, nullptr, nullptr) != SQLITE_OK ||
        sqlite3_db_cacheflush(db) != SQLITE_OK)
        std::_Exit(4);
    std::_Exit(0); // No rollback, destructors or connection close.
}
void crashedProcess(const std::filesystem::path& self, const std::filesystem::path& path) {
#if defined(_WIN32)
    auto command = L"\"" + self.wstring() + L"\" --crash \"" + path.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    assert(CreateProcessW(self.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                          nullptr, &startup, &process));
    assert(WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0);
    DWORD code{};
    assert(GetExitCodeProcess(process.hProcess, &code) && code == 0);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
#else
    (void)self;
    const auto child = fork();
    assert(child >= 0);
    if (!child)
        crashWrite(path);
    int status{};
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
}
int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--crash")
        crashWrite(argv[2]);
    const auto root = std::filesystem::temp_directory_path() /
                      ("loopz-sqlite-test-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    assert(std::filesystem::create_directory(root));
    const auto path = root / "setup.sqlite3";
    {
        const auto typedPath = root / "typed.sqlite3";
        DeviceIntentRequest sink;
        sink.operationId = request(100, 0).operationId;
        sink.kind = DeviceIntentKind::AppRoutingSink;
        {
            DeviceIntentDatabase typed(typedPath, true);
            assert(typed.accept(owner, sink) == 1);
            assert(typed.accept(owner, sink) == 1); // Idempotent retry.
            for (unsigned i = 0; i < 32; ++i)
                assert(typed.accept(owner, request(101 + i, i + 1, "mix-" + std::to_string(i))) ==
                       i + 2);
            rejects([&] {
                typed.accept(owner, request(200, 33, "overflow"));
            });
            auto removed = sink;
            removed.operationId = request(201, 33).operationId;
            removed.expectedRevision = 33;
            removed.action = DeviceIntentAction::Remove;
            assert(typed.accept(owner, removed) == 34);
            assert(!typed.removalApplied(owner, "LoopZ.AppRoutingSink.v1", 34));
            assert(!typed.removalApplied(owner, "", 33, DeviceIntentKind::AppRoutingSink));
        }
        DeviceIntentDatabase reopened(typedPath);
        assert(reopened.snapshot(owner).records.size() == 33);
        assert(reopened.removalApplied(owner, "", 34, DeviceIntentKind::AppRoutingSink));
        assert(reopened.snapshot(owner).records.size() == 32);
        assert(reopened.snapshot("different-owner").records.empty());
    }
    JSON committed;
    {
        rejects([&] {
            DeviceIntentDatabase missing(path);
        });
        DeviceIntentDatabase db(path, true);
        assert(db.snapshot(owner).revision == 0 && db.snapshot(owner).records.empty());
        const auto first = request(1, 0);
        assert(db.accept(owner, first) == 1);
        committed = snapshot(db.snapshot(owner));
        assert(db.accept(owner, first) == 1 && snapshot(db.snapshot(owner)) == committed);
        auto forged = first;
        forged.name = "changed";
        rejects([&] {
            (void)db.accept(owner, forged);
        });
        rejects([&] {
            (void)db.accept(owner, request(2, 0));
        });
        assert(snapshot(db.snapshot(owner)) == committed);
        assert(db.accept("uid:502", first) == 1); // Independent owner/revision/operation scope.
        assert(db.accept(owner, request(2, 1, "mix", DeviceIntentAction::Remove)) == 2);
        assert(!db.removalApplied("uid:502", "mix", 2));
        assert(db.accept(owner, request(3, 2)) == 3);
        assert(!db.removalApplied(owner, "mix", 2)); // Late removal after undo.
        assert(db.accept(owner, request(4, 3, "mix", DeviceIntentAction::Remove)) == 4);
        assert(db.removalApplied(owner, "mix", 4));
        assert(db.snapshot(owner).records.empty());
        assert(db.accept(owner, request(2, 1, "mix", DeviceIntentAction::Remove)) == 2);
        assert(db.snapshot(owner).records.empty());
        assert(db.accept(owner, request(5, 4, "keep")) == 5);
        for (unsigned id = 6; id < 80; ++id)
            assert(db.accept(owner, request(id, id - 1, "updated")) == id);
        assert(db.accept(owner, request(5, 4, "keep")) ==
               5); // Desired record outlives receipt retention.
        rejects([&] {
            (void)db.accept(owner, first);
        });
        committed = snapshot(db.snapshot(owner));
        DeviceIntentDatabase second(path);
        rejects([&] {
            (void)second.accept(owner, request(100, 78));
        });
        assert(snapshot(second.snapshot(owner)) == committed);
        std::atomic<unsigned> ready{}, accepted{}, rejected{};
        const auto race = [&](DeviceIntentDatabase& connection, unsigned id) {
            ready.fetch_add(1);
            while (ready.load() != 2)
                std::this_thread::yield();
            try {
                (void)connection.accept("uid:concurrent", request(id, 0));
                accepted.fetch_add(1);
            } catch (const std::exception&) {
                rejected.fetch_add(1);
            }
        };
        std::thread a(race, std::ref(db), 1), b(race, std::ref(second), 2);
        a.join();
        b.join();
        assert(accepted == 1 && rejected == 1 && db.snapshot("uid:concurrent").revision == 1);
    }
    crashedProcess(std::filesystem::absolute(argv[0]), path);
    {
        DeviceIntentDatabase recovered(path);
        assert(snapshot(recovered.snapshot(owner)) == committed);
        assert(recovered.accept(owner, request(79, 78, "updated")) == 79);
    }
    raw(path, "CREATE TRIGGER fail_receipt BEFORE INSERT ON receipts BEGIN SELECT "
              "RAISE(ABORT,'test'); END;");
    {
        DeviceIntentDatabase db(path);
        rejects([&] {
            (void)db.accept(owner, request(80, 79));
        });
        assert(snapshot(db.snapshot(owner)) == committed); // All earlier statements rolled back.
    }
    raw(path, "DROP TRIGGER fail_receipt;");
    {
        DeviceIntentDatabase db(path);
        for (unsigned id = 1; id <= kMaximumVirtualDevices; ++id)
            (void)db.accept("uid:capacity", request(id, id - 1, std::to_string(id)));
        rejects([&] {
            (void)db.accept("uid:capacity", request(100, 32, "extra"));
        });
        assert(db.accept("uid:capacity", request(100, 32, "1")) == 33);
        for (unsigned id = 1; id <= 256; ++id)
            (void)db.accept("uid:backlog",
                            request(id, id - 1, std::to_string(id), DeviceIntentAction::Remove));
        rejects([&] {
            (void)db.accept("uid:backlog", request(300, 256, "extra", DeviceIntentAction::Remove));
        });
        assert(db.removalApplied("uid:backlog", "1", 1));
        assert(db.accept("uid:backlog", request(300, 256, "extra", DeviceIntentAction::Remove)) ==
               257);
        auto odd = request(1, 0);
        odd.channels = 3;
        assert(db.accept("mac:odd", odd) == 1 && db.snapshot("mac:odd").records[0].channels == 3);
    }
    // Startup recovery must discover desired work without a connected client,
    // including deletion tombstones. Receipts alone are not desired devices.
    const auto recoveryPath = root / "owner-recovery.sqlite3";
    {
        DeviceIntentDatabase db(recoveryPath, true);
        assert(db.ownersWithDesiredState().empty());
        assert(db.accept("account:b", request(1, 0)) == 1);
        assert(db.accept("account:a", request(1, 0)) == 1);
        assert((db.ownersWithDesiredState() == std::vector<std::string>{"account:a", "account:b"}));
        assert(db.accept("account:a", request(2, 1, "mix", DeviceIntentAction::Remove)) == 2);
        assert(db.ownersWithDesiredState().size() == 2);
    }
    {
        DeviceIntentDatabase db(recoveryPath);
        assert((db.ownersWithDesiredState() == std::vector<std::string>{"account:a", "account:b"}));
        assert(!db.removalApplied("account:a", "mix", 1));
        assert(db.ownersWithDesiredState().size() == 2);
        assert(db.removalApplied("account:a", "mix", 2));
        assert((db.ownersWithDesiredState() == std::vector<std::string>{"account:b"}));
        assert(db.snapshot("account:a").revision == 2);
        assert(db.accept("account:b", request(2, 1, "mix", DeviceIntentAction::Remove)) == 2);
        assert(db.removalApplied("account:b", "mix", 2));
        assert(db.ownersWithDesiredState().empty());
        assert(db.snapshot("account:b").revision == 2);
    }
    // Big-endian BLOB revisions retain the complete uint64_t range in SQLite.
    raw(path, "UPDATE owners SET revision=X'F000000000000001' WHERE owner='mac:odd';");
    {
        DeviceIntentDatabase db(path);
        const auto high = uint64_t(15) << 60;
        auto large = request(2, high + 1);
        large.channels = 1;
        assert(db.accept("mac:odd", large) == high + 2);
        assert(db.snapshot("mac:odd").revision == high + 2);
    }
    raw(path, "PRAGMA journal_mode=WAL; PRAGMA user_version=99;");
    rejects([&] {
        DeviceIntentDatabase unsupported(path);
    });
    assert(queryText(path, "PRAGMA journal_mode") == "wal");
    const auto corrupt = root / "corrupt.sqlite3";
    {
        std::ofstream out(corrupt);
        out << "not a database";
    }
    rejects([&] {
        DeviceIntentDatabase bad(corrupt, true);
    });
    std::filesystem::remove_all(root);
    std::cout << "PASS: SQLite transactions, real process crash recovery, injected rollback, "
                 "CAS, retries, owner isolation, removal races, capacity and exact revisions\n";
}
