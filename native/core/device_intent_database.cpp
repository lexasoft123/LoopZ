#include <loopz/device_intent_database.hpp>
#include <sqlite3.h>
#include <array>
#include <stdexcept>

namespace loopz {
namespace {
constexpr int applicationId = 0x4c5a4442;
constexpr size_t recordLimit = 256;
[[noreturn]] void fail(int code) {
    throw std::runtime_error(std::string("Microphone setup database: ") + sqlite3_errstr(code));
}
void check(int code) {
    if (code != SQLITE_OK)
        fail(code);
}
void exec(sqlite3* db, const char* sql) {
    check(sqlite3_exec(db, sql, nullptr, nullptr, nullptr));
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
        if (text.size() > kMaximumDeviceIntentBytes)
            throw std::invalid_argument("Setup field too large");
        check(sqlite3_bind_text(value, index, text.data(), static_cast<int>(text.size()),
                                SQLITE_TRANSIENT));
    }
    void revision(int index, uint64_t number) {
        std::array<unsigned char, 8> bytes{};
        for (unsigned i = 0; i < 8; ++i)
            bytes[7 - i] = static_cast<unsigned char>(number >> (8 * i));
        check(sqlite3_bind_blob(value, index, bytes.data(), 8, SQLITE_TRANSIENT));
    }
    bool row() {
        const auto code = sqlite3_step(value);
        if (code == SQLITE_ROW)
            return true;
        if (code != SQLITE_DONE)
            fail(code);
        return false;
    }
    void done() {
        if (row())
            throw std::runtime_error("Unexpected setup database result");
    }
    uint64_t revision(int column) const {
        if (sqlite3_column_type(value, column) != SQLITE_BLOB ||
            sqlite3_column_bytes(value, column) != 8)
            throw std::runtime_error("Invalid stored setup revision");
        const auto* bytes = static_cast<const unsigned char*>(sqlite3_column_blob(value, column));
        uint64_t number = 0;
        for (unsigned i = 0; i < 8; ++i)
            number = (number << 8) | bytes[i];
        return number;
    }
    std::string text(int column) const {
        if (sqlite3_column_type(value, column) != SQLITE_TEXT)
            throw std::runtime_error("Invalid stored setup text");
        const auto size = sqlite3_column_bytes(value, column);
        if (size < 0 || static_cast<size_t>(size) > kMaximumDeviceIntentBytes)
            throw std::runtime_error("Invalid stored setup text size");
        const auto* bytes = reinterpret_cast<const char*>(sqlite3_column_text(value, column));
        if (!bytes)
            fail(SQLITE_NOMEM);
        return {bytes, static_cast<size_t>(size)};
    }
};
struct Transaction {
    sqlite3* db;
    bool committed{};
    explicit Transaction(sqlite3* database, bool write) : db(database) {
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
void owner(std::string_view sid) {
    if (sid.empty() || sid.size() > 4096 || sid.find('\0') != std::string_view::npos)
        throw std::invalid_argument("Invalid authenticated setup owner");
}
int scalar(sqlite3* db, const char* sql) {
    Statement query(db, sql);
    if (!query.row())
        throw std::runtime_error("Missing setup database metadata");
    return sqlite3_column_int(query.value, 0);
}
uint64_t currentRevision(sqlite3* db, std::string_view sid) {
    Statement query(db, "SELECT revision FROM owners WHERE owner=?1");
    query.text(1, sid);
    return query.row() ? query.revision(0) : 0;
}
bool emptyDatabase(sqlite3* db, bool initialize) {
    const auto id = scalar(db, "PRAGMA application_id");
    const auto version = scalar(db, "PRAGMA user_version");
    if (id == applicationId && version == 1)
        return false;
    if (id == 0 && version == 0 && initialize &&
        scalar(db, "SELECT count(*) FROM sqlite_schema") == 0)
        return true;
    throw std::runtime_error("Unsupported microphone setup database; repair required");
}
DeviceIntentRequest storedRequest(Statement& query) {
    const auto request = decodeDeviceIntent(query.text(1));
    if (request.expectedRevision + 1 != query.revision(0))
        throw std::runtime_error("Stored setup operation revision disagrees");
    return request;
}
} // namespace
struct DeviceIntentDatabase::Impl {
    sqlite3* db{};
    ~Impl() {
        if (db)
            sqlite3_close_v2(db);
    }
};
DeviceIntentDatabase::DeviceIntentDatabase(const std::filesystem::path& path, bool initialize)
    : impl_(std::make_unique<Impl>()) {
    const auto utf8 = path.u8string();
    check(sqlite3_open_v2(reinterpret_cast<const char*>(utf8.c_str()), &impl_->db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX |
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
    // Validate identity before changing persistent settings of an existing file.
    (void)emptyDatabase(db, initialize);
    exec(db, "PRAGMA journal_mode=DELETE");
    Transaction transaction(db, true);
    // Recheck under the writer lock if another initializer won the race.
    if (emptyDatabase(db, initialize)) {
        exec(db,
             "CREATE TABLE owners(owner TEXT PRIMARY KEY, revision BLOB NOT NULL "
             "CHECK(length(revision)=8)) STRICT;"
             "CREATE TABLE receipts(owner TEXT NOT NULL REFERENCES owners(owner), operation TEXT "
             "NOT NULL,"
             " revision BLOB NOT NULL CHECK(length(revision)=8), request TEXT NOT NULL "
             "CHECK(length(request)<=65536),"
             " PRIMARY KEY(owner,operation), UNIQUE(owner,revision)) STRICT;"
             "CREATE TABLE desired(owner TEXT NOT NULL REFERENCES owners(owner), mix TEXT NOT NULL,"
             " operation TEXT NOT NULL, revision BLOB NOT NULL CHECK(length(revision)=8),"
             " action INTEGER NOT NULL CHECK(action IN(0,1)), request TEXT NOT NULL "
             "CHECK(length(request)<=65536),"
             " PRIMARY KEY(owner,mix), UNIQUE(owner,operation), UNIQUE(owner,revision)) STRICT;"
             "PRAGMA application_id=1280984130; PRAGMA user_version=1;");
    }
    if (scalar(db, "PRAGMA synchronous") != 3 || scalar(db, "PRAGMA foreign_keys") != 1)
        throw std::runtime_error("Setup database durability configuration failed");
    Statement integrity(db, "PRAGMA quick_check(1)");
    if (!integrity.row() || integrity.text(0) != "ok")
        throw std::runtime_error("Microphone setup database is corrupt");
    Statement foreignKeys(db, "PRAGMA foreign_key_check");
    if (foreignKeys.row())
        throw std::runtime_error("Microphone setup database relationships are corrupt");
    transaction.commit();
}
DeviceIntentDatabase::~DeviceIntentDatabase() = default;
uint64_t DeviceIntentDatabase::accept(std::string_view sid, const DeviceIntentRequest& input) {
    owner(sid);
    const auto body = encodeDeviceIntent(input).dump();
    const auto request = decodeDeviceIntent(body);
    const auto storageKey = deviceIntentStorageKey(request.kind, request.mixId);
    auto* db = impl_->db;
    Transaction transaction(db, true);
    for (const auto* sql :
         {"SELECT revision,request FROM receipts WHERE owner=?1 AND operation=?2",
          "SELECT revision,request FROM desired WHERE owner=?1 AND operation=?2"}) {
        Statement query(db, sql);
        query.text(1, sid);
        query.text(2, request.operationId);
        if (query.row()) {
            const auto stored = storedRequest(query);
            if (encodeDeviceIntent(stored).dump() != body)
                throw std::invalid_argument("Microphone setup operation ID was reused");
            const auto accepted = query.revision(0);
            transaction.commit();
            return accepted;
        }
    }
    const auto current = currentRevision(db, sid);
    if (request.expectedRevision != current)
        throw std::invalid_argument("Microphone setup revision changed");
    const auto accepted = current + 1; // Decoder rejects UINT64_MAX expected revisions.
    Statement counts(
        db,
        "SELECT count(*),coalesce(sum(action=0 AND mix!=?3),0),"
        "coalesce(max(CASE WHEN mix=?2 THEN action+1 ELSE 0 END),0) FROM desired WHERE owner=?1");
    counts.text(1, sid);
    counts.text(2, storageKey);
    counts.text(3, deviceIntentStorageKey(DeviceIntentKind::AppRoutingSink, {}));
    if (!counts.row())
        throw std::runtime_error("Cannot count configured microphones");
    const auto total = sqlite3_column_int64(counts.value, 0);
    const auto live = sqlite3_column_int64(counts.value, 1);
    const auto existing = sqlite3_column_int(counts.value, 2);
    if ((!existing && total >= static_cast<sqlite3_int64>(recordLimit)) ||
        (request.action == DeviceIntentAction::Upsert && existing != 1 &&
         request.kind == DeviceIntentKind::MicrophoneMix && live >= kMaximumVirtualDevices))
        throw std::invalid_argument("Microphone setup capacity reached");
    Statement updateOwner(db, "INSERT INTO owners(owner,revision) VALUES(?1,?2) "
                              "ON CONFLICT(owner) DO UPDATE SET revision=excluded.revision");
    updateOwner.text(1, sid);
    updateOwner.revision(2, accepted);
    updateOwner.done();
    Statement update(db, "INSERT INTO desired(owner,mix,operation,revision,action,request) "
                         "VALUES(?1,?2,?3,?4,?5,?6) "
                         "ON CONFLICT(owner,mix) DO UPDATE SET "
                         "operation=excluded.operation,revision=excluded.revision,"
                         "action=excluded.action,request=excluded.request");
    update.text(1, sid);
    update.text(2, storageKey);
    update.text(3, request.operationId);
    update.revision(4, accepted);
    check(sqlite3_bind_int(update.value, 5, request.action == DeviceIntentAction::Upsert ? 0 : 1));
    update.text(6, body);
    update.done();
    Statement receipt(db,
                      "INSERT INTO receipts(owner,operation,revision,request) VALUES(?1,?2,?3,?4)");
    receipt.text(1, sid);
    receipt.text(2, request.operationId);
    receipt.revision(3, accepted);
    receipt.text(4, body);
    receipt.done();
    Statement prune(
        db, "DELETE FROM receipts WHERE owner=?1 AND operation NOT IN "
            "(SELECT operation FROM receipts WHERE owner=?1 ORDER BY revision DESC LIMIT 64)");
    prune.text(1, sid);
    prune.done();
    transaction.commit();
    return accepted;
}
DeviceIntentSnapshot DeviceIntentDatabase::snapshot(std::string_view sid) const {
    owner(sid);
    auto* db = impl_->db;
    Transaction transaction(db, false);
    DeviceIntentSnapshot result;
    result.revision = currentRevision(db, sid);
    Statement query(
        db,
        "SELECT revision,request,mix,operation,action FROM desired WHERE owner=?1 ORDER BY mix");
    query.text(1, sid);
    size_t live = 0;
    while (query.row()) {
        auto request = storedRequest(query);
        if (query.revision(0) > result.revision ||
            deviceIntentStorageKey(request.kind, request.mixId) != query.text(2) ||
            request.operationId != query.text(3) ||
            sqlite3_column_int(query.value, 4) !=
                (request.action == DeviceIntentAction::Upsert ? 0 : 1) ||
            result.records.size() == recordLimit ||
            (request.action == DeviceIntentAction::Upsert &&
             request.kind == DeviceIntentKind::MicrophoneMix && ++live > kMaximumVirtualDevices))
            throw std::runtime_error("Stored microphone setup records disagree");
        result.records.push_back(std::move(request));
    }
    transaction.commit();
    return result;
}
std::vector<std::string> DeviceIntentDatabase::ownersWithDesiredState() const {
    auto* db = impl_->db;
    Transaction transaction(db, false);
    Statement query(db, "SELECT DISTINCT owner FROM desired ORDER BY owner");
    std::vector<std::string> result;
    while (query.row()) {
        auto identity = query.text(0);
        owner(identity);
        result.push_back(std::move(identity));
    }
    transaction.commit();
    return result;
}
bool DeviceIntentDatabase::removalApplied(std::string_view sid, std::string_view mixId,
                                          uint64_t revision, DeviceIntentKind kind) {
    owner(sid);
    auto* db = impl_->db;
    Transaction transaction(db, true);
    Statement remove(db,
                     "DELETE FROM desired WHERE owner=?1 AND mix=?2 AND revision=?3 AND action=1");
    remove.text(1, sid);
    remove.text(2, deviceIntentStorageKey(kind, mixId));
    remove.revision(3, revision);
    remove.done();
    const bool changed = sqlite3_changes(db) != 0;
    transaction.commit();
    return changed;
}
} // namespace loopz
