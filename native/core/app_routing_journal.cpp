#include <loopz/app_routing_journal.hpp>
#include <sqlite3.h>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace loopz {
namespace {
constexpr int applicationId = 0x4c5a4152;
constexpr size_t maximumText = 32768, maximumReceipts = 256, maximumTargets = 256;
void check(int code) {
    if (code != SQLITE_OK)
        throw std::runtime_error(std::string("App routing journal: ") + sqlite3_errstr(code));
}
void exec(sqlite3* db, const char* sql) {
    check(sqlite3_exec(db, sql, nullptr, nullptr, nullptr));
}
void validate(std::string_view value, bool empty = false) {
    if ((!empty && value.empty()) || value.size() > maximumText ||
        value.find('\0') != std::string_view::npos)
        throw std::invalid_argument("Invalid app routing journal field");
}
struct Statement {
    sqlite3_stmt* value{};
    Statement(sqlite3* db, const char* sql) {
        check(sqlite3_prepare_v2(db, sql, -1, &value, nullptr));
    }
    ~Statement() {
        sqlite3_finalize(value);
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    void text(int index, std::string_view text) {
        check(sqlite3_bind_text(value, index, text.data(), static_cast<int>(text.size()),
                                SQLITE_TRANSIENT));
    }
    bool row() {
        const auto code = sqlite3_step(value);
        if (code == SQLITE_ROW)
            return true;
        if (code != SQLITE_DONE)
            check(code);
        return false;
    }
    void done() {
        if (row())
            throw std::runtime_error("Unexpected app routing journal result");
    }
    std::string text(int column) const {
        if (sqlite3_column_type(value, column) != SQLITE_TEXT)
            throw std::runtime_error("Invalid stored app routing field");
        const auto bytes = sqlite3_column_bytes(value, column);
        if (bytes < 0 || static_cast<size_t>(bytes) > maximumText)
            throw std::runtime_error("Invalid stored app routing field size");
        const auto* data = reinterpret_cast<const char*>(sqlite3_column_text(value, column));
        if (!data)
            check(SQLITE_NOMEM);
        std::string result(data, static_cast<size_t>(bytes));
        validate(result, true);
        return result;
    }
};
struct Transaction {
    sqlite3* db;
    bool committed{};
    Transaction(sqlite3* value, bool write) : db(value) {
        exec(db, write ? "BEGIN IMMEDIATE" : "BEGIN");
    }
    ~Transaction() {
        if (!committed)
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    void commit() {
        exec(db, "COMMIT");
        committed = true;
    }
};
sqlite3_int64 scalar(sqlite3* db, const char* sql) {
    Statement query(db, sql);
    if (!query.row() || sqlite3_column_type(query.value, 0) != SQLITE_INTEGER)
        throw std::runtime_error("Invalid app routing journal metadata");
    return sqlite3_column_int64(query.value, 0);
}
bool emptyDatabase(sqlite3* db, bool initialize) {
    const auto id = scalar(db, "PRAGMA application_id");
    const auto version = scalar(db, "PRAGMA user_version");
    if (id == applicationId && (version == 1 || version == 2))
        return false;
    if (initialize && id == 0 && version == 0 &&
        scalar(db, "SELECT count(*) FROM sqlite_schema") == 0)
        return true;
    throw std::runtime_error("Unsupported app routing journal; preserve it for recovery");
}
std::vector<AppRoutingReceipt> read(sqlite3* db) {
    const auto sequence = scalar(db, "SELECT revision FROM metadata WHERE singleton=1");
    if (sequence < 0)
        throw std::runtime_error("Invalid app routing journal sequence");
    Statement query(db,
                    "SELECT application,revision,original0,original1,original2,phase FROM leases "
                    "ORDER BY application");
    std::vector<AppRoutingReceipt> result;
    while (query.row()) {
        if (result.size() >= maximumReceipts ||
            sqlite3_column_type(query.value, 1) != SQLITE_INTEGER ||
            sqlite3_column_int64(query.value, 1) <= 0 ||
            sqlite3_column_int64(query.value, 1) > sequence)
            throw std::runtime_error("Invalid stored app routing receipt");
        if (sqlite3_column_type(query.value, 5) != SQLITE_INTEGER ||
            sqlite3_column_int64(query.value, 5) < 0 || sqlite3_column_int64(query.value, 5) > 3)
            throw std::runtime_error("Invalid app routing receipt phase");
        AppRoutingReceipt receipt;
        receipt.phase = static_cast<AppRoutingReceiptPhase>(sqlite3_column_int(query.value, 5));
        receipt.application = query.text(0);
        validate(receipt.application);
        receipt.revision = static_cast<uint64_t>(sqlite3_column_int64(query.value, 1));
        for (int role = 0; role < 3; ++role)
            receipt.original[role] = query.text(role + 2);
        Statement targets(db, "SELECT target FROM targets WHERE application=?1 ORDER BY target");
        targets.text(1, receipt.application);
        while (targets.row()) {
            if (receipt.attemptedTargets.size() >= maximumTargets)
                throw std::runtime_error("Too many stored app routing targets");
            auto target = targets.text(0);
            validate(target);
            receipt.attemptedTargets.push_back(std::move(target));
        }
        if (receipt.attemptedTargets.empty())
            throw std::runtime_error("App routing receipt has no owned target");
        result.push_back(std::move(receipt));
    }
    return result;
}
} // namespace
struct AppRoutingJournal::Impl {
    sqlite3* db{};
    ~Impl() {
        if (db)
            sqlite3_close_v2(db);
    }
};
AppRoutingJournal::AppRoutingJournal(const std::filesystem::path& path, bool initialize)
    : impl_(std::make_unique<Impl>()) {
    const auto utf8 = path.u8string();
    if (utf8.empty() || utf8.find(u8'\0') != std::u8string::npos)
        throw std::invalid_argument("Invalid app routing journal path");
    check(sqlite3_open_v2(reinterpret_cast<const char*>(utf8.c_str()), &impl_->db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW |
                              (initialize ? SQLITE_OPEN_CREATE : 0),
                          nullptr));
    auto* db = impl_->db;
    check(sqlite3_busy_timeout(db, 1000));
    check(sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr));
    check(sqlite3_db_config(db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr));
    sqlite3_limit(db, SQLITE_LIMIT_LENGTH, 128 * 1024);
    sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, 16 * 1024);
    sqlite3_limit(db, SQLITE_LIMIT_ATTACHED, 0);
    exec(db, "PRAGMA foreign_keys=ON; PRAGMA synchronous=EXTRA");
    (void)emptyDatabase(db, initialize);
    exec(db, "PRAGMA journal_mode=DELETE");
    Transaction transaction(db, true);
    if (emptyDatabase(db, initialize)) {
        exec(db, "CREATE TABLE metadata(singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
                 "revision INTEGER NOT NULL CHECK(revision>=0)) STRICT;"
                 "INSERT INTO metadata VALUES(1,0);"
                 "CREATE TABLE leases(application TEXT PRIMARY KEY,revision INTEGER NOT NULL "
                 "CHECK(revision>0),"
                 "original0 TEXT NOT NULL,original1 TEXT NOT NULL,original2 TEXT NOT NULL,phase "
                 "INTEGER NOT NULL CHECK(phase IN(0,1,2,3))) STRICT;"
                 "CREATE TABLE targets(application TEXT NOT NULL REFERENCES leases(application) ON "
                 "DELETE CASCADE,"
                 "target TEXT NOT NULL,PRIMARY KEY(application,target)) STRICT;");
        const auto identity =
            "PRAGMA application_id=" + std::to_string(applicationId) + "; PRAGMA user_version=2";
        exec(db, identity.c_str());
    }
    if (scalar(db, "PRAGMA user_version") == 1)
        exec(db, "ALTER TABLE leases ADD COLUMN phase INTEGER NOT NULL DEFAULT 1 "
                 "CHECK(phase IN(0,1,2,3)); PRAGMA user_version=2");
    if (scalar(db, "SELECT revision FROM metadata WHERE singleton=1") < 0)
        throw std::runtime_error("Invalid app routing journal sequence");
    Statement integrity(db, "PRAGMA quick_check(1)");
    if (!integrity.row() || integrity.text(0) != "ok" || integrity.row())
        throw std::runtime_error("App routing journal integrity check failed");
    Statement foreignKeys(db, "PRAGMA foreign_key_check");
    if (foreignKeys.row())
        throw std::runtime_error("App routing journal has orphan ownership records");
    (void)read(db);
    transaction.commit();
}
AppRoutingJournal::~AppRoutingJournal() = default;
std::vector<AppRoutingReceipt> AppRoutingJournal::snapshot() const {
    Transaction transaction(impl_->db, false);
    auto result = read(impl_->db);
    transaction.commit();
    return result;
}
AppRoutingReceipt AppRoutingJournal::prepare(std::string_view application,
                                             const std::array<std::string, 3>& original,
                                             std::string_view target) {
    validate(application);
    validate(target);
    for (const auto& role : original)
        validate(role, true);
    auto* db = impl_->db;
    Transaction transaction(db, true);
    const auto records = read(db);
    const auto found = std::find_if(records.begin(), records.end(), [&](const auto& receipt) {
        return receipt.application == application;
    });
    if (found != records.end()) {
        if (found->original != original)
            throw std::invalid_argument("Cannot replace an owned app routing baseline");
        if (found->attemptedTargets.size() >= maximumTargets &&
            std::find(found->attemptedTargets.begin(), found->attemptedTargets.end(), target) ==
                found->attemptedTargets.end())
            throw std::runtime_error(
                "App routing target history is full; restore before reacquiring");
    } else if (records.size() >= maximumReceipts) {
        throw std::runtime_error("App routing journal is full");
    }
    const auto sequence = scalar(db, "SELECT revision FROM metadata WHERE singleton=1");
    if (sequence < 0 || sequence == std::numeric_limits<sqlite3_int64>::max())
        throw std::runtime_error("App routing journal sequence exhausted");
    exec(db, "UPDATE metadata SET revision=revision+1 WHERE singleton=1");
    Statement upsert(db,
                     "INSERT INTO leases(application,revision,original0,original1,original2,phase) "
                     "VALUES(?1,?2,?3,?4,?5,0) ON CONFLICT(application) "
                     "DO UPDATE SET revision=excluded.revision,phase=0");
    upsert.text(1, application);
    check(sqlite3_bind_int64(upsert.value, 2, sequence + 1));
    for (int role = 0; role < 3; ++role)
        upsert.text(role + 3, original[role]);
    upsert.done();
    Statement add(db,
                  "INSERT INTO targets VALUES(?1,?2) ON CONFLICT(application,target) DO NOTHING");
    add.text(1, application);
    add.text(2, target);
    add.done();
    auto all = read(db);
    const auto prepared = std::find_if(all.begin(), all.end(), [&](const auto& receipt) {
        return receipt.application == application;
    });
    if (prepared == all.end())
        throw std::runtime_error("Prepared app routing receipt is missing");
    auto result = *prepared;
    transaction.commit();
    return result;
}
bool AppRoutingJournal::confirm(std::string_view application, uint64_t revision,
                                std::string_view target) {
    validate(application);
    validate(target);
    if (!revision || revision > static_cast<uint64_t>(std::numeric_limits<sqlite3_int64>::max()))
        throw std::invalid_argument("Invalid app routing confirmation revision");
    Transaction transaction(impl_->db, true);
    const auto records = read(impl_->db);
    const auto found = std::find_if(records.begin(), records.end(), [&](const auto& receipt) {
        return receipt.application == application && receipt.revision == revision;
    });
    if (found == records.end() || found->phase == AppRoutingReceiptPhase::UserOverride ||
        found->phase == AppRoutingReceiptPhase::Releasing) {
        transaction.commit();
        return false;
    }
    if (std::find(found->attemptedTargets.begin(), found->attemptedTargets.end(), target) ==
        found->attemptedTargets.end())
        throw std::invalid_argument("Cannot confirm an unowned app routing target");
    Statement applied(impl_->db, "UPDATE leases SET phase=1 WHERE application=?1");
    applied.text(1, application);
    applied.done();
    Statement prune(impl_->db, "DELETE FROM targets WHERE application=?1 AND target<>?2");
    prune.text(1, application);
    prune.text(2, target);
    prune.done();
    transaction.commit();
    return true;
}
std::optional<AppRoutingReceipt> AppRoutingJournal::beginRelease(std::string_view application,
                                                                 uint64_t revision,
                                                                 AppRoutingReceiptPhase phase) {
    validate(application);
    if (phase != AppRoutingReceiptPhase::UserOverride && phase != AppRoutingReceiptPhase::Releasing)
        throw std::invalid_argument("Invalid app routing release phase");
    auto* db = impl_->db;
    Transaction transaction(db, true);
    const auto records = read(db);
    const auto found = std::find_if(records.begin(), records.end(), [&](const auto& receipt) {
        return receipt.application == application && receipt.revision == revision;
    });
    if (found == records.end()) {
        transaction.commit();
        return {};
    }
    auto result = *found;
    if (result.phase != phase) {
        const auto sequence = scalar(db, "SELECT revision FROM metadata WHERE singleton=1");
        if (sequence == std::numeric_limits<sqlite3_int64>::max())
            throw std::runtime_error("App routing journal sequence exhausted");
        exec(db, "UPDATE metadata SET revision=revision+1 WHERE singleton=1");
        Statement update(db, "UPDATE leases SET revision=?2,phase=?3 WHERE application=?1");
        update.text(1, application);
        check(sqlite3_bind_int64(update.value, 2, sequence + 1));
        check(sqlite3_bind_int(update.value, 3, static_cast<int>(phase)));
        update.done();
        result.revision = static_cast<uint64_t>(sequence + 1);
        result.phase = phase;
    }
    transaction.commit();
    return result;
}
bool AppRoutingJournal::forget(std::string_view application, uint64_t revision) {
    validate(application);
    if (!revision || revision > static_cast<uint64_t>(std::numeric_limits<sqlite3_int64>::max()))
        throw std::invalid_argument("Invalid app routing acknowledgment revision");
    Transaction transaction(impl_->db, true);
    Statement remove(impl_->db, "DELETE FROM leases WHERE application=?1 AND revision=?2");
    remove.text(1, application);
    check(sqlite3_bind_int64(remove.value, 2, static_cast<sqlite3_int64>(revision)));
    remove.done();
    const bool removed = sqlite3_changes(impl_->db) != 0;
    transaction.commit();
    return removed;
}
std::array<AppRoutingRestore, 3> appRoutingRestoration(const AppRoutingReceipt& receipt,
                                                       const std::array<std::string, 3>& current) {
    std::array<AppRoutingRestore, 3> result{};
    for (size_t role = 0; role < result.size(); ++role)
        result[role] = current[role] == receipt.original[role] ? AppRoutingRestore::Unchanged
                       : std::find(receipt.attemptedTargets.begin(), receipt.attemptedTargets.end(),
                                   current[role]) != receipt.attemptedTargets.end()
                           ? AppRoutingRestore::RestoreOriginal
                           : AppRoutingRestore::PreserveUserChange;
    return result;
}
} // namespace loopz
