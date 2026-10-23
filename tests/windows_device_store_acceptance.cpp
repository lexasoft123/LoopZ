#include "device_intent_store.hpp"
#include "broker_handler.hpp"
#include <windows.h>
#include <array>
#include <iostream>
#include <stdexcept>

using namespace loopz;
using namespace loopz::windows;
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(std::string(message) + " (Windows error " +
                                 std::to_string(GetLastError()) + ")");
}
template <class F> void rejects(F&& operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "Expected unsafe or conflicting operation to be rejected");
}
struct Handle {
    HANDLE value{};
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
void restrictedAccessDenied(const std::filesystem::path& database) {
    Handle token, restricted;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &token.value),
            "Read administrative test token");
    alignas(DWORD) std::array<BYTE, SECURITY_MAX_SID_SIZE> administrators{};
    DWORD size = static_cast<DWORD>(administrators.size());
    require(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators.data(), &size),
            "Create administrative group SID");
    SID_AND_ATTRIBUTES disabled{administrators.data(), 0};
    require(CreateRestrictedToken(token.value, DISABLE_MAX_PRIVILEGE, 1, &disabled, 0, nullptr, 0,
                                  nullptr, &restricted.value),
            "Create restricted test token");
    require(ImpersonateLoggedOnUser(restricted.value), "Enter restricted test context");
    const auto file = CreateFileW(database.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
    const auto error = GetLastError();
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    // Always leave impersonation before reporting a failed assertion.
    if (!RevertToSelf())
        std::terminate();
    require(file == INVALID_HANDLE_VALUE && error == ERROR_ACCESS_DENIED,
            "Restricted token must not read or modify the setup database");
}
} // namespace
int main() {
    // This executable is deliberately excluded from automatic CTest. It needs
    // elevation and creates only this unique test folder, never the real store.
    const auto root = deviceIntentStoreRoot().parent_path() /
                      (L"LoopZDeviceBrokerTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                       std::to_wstring(GetTickCount64()));
    std::wcout << L"Isolated store: " << root << L'\n';
    try {
        require(GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES &&
                    GetLastError() == ERROR_FILE_NOT_FOUND,
                "Test directory must be new");
        provisionDeviceIntentStore(root);
        constexpr auto sid = L"S-1-5-21-1-2-3-1001";
        constexpr auto other = L"S-1-5-21-1-2-3-1002";
        DeviceIntentRequest request;
        request.operationId = "12345678-1234-1234-1234-123456789abc";
        request.mixId = "test";
        request.name = "Test microphone";
        {
            DeviceIntentStore first(root);
            const auto initial =
                handleBrokerMessage(first, {sid, R"({"version":1,"action":"status"})"});
            require(initial.reply.at("ok") && initial.reply.at("revision") == 0 &&
                        !initial.reconcileRequested,
                    "Read durable status before any setup intent");
            require(first.ownersWithDesiredState().empty(),
                    "Fresh protected store has no desired owners");
            require(first.accept(sid, request) == 1, "Persist first setup intent");
            require(first.ownersWithDesiredState() == std::vector<std::wstring>{sid},
                    "Discover owner without a connected app");
            restrictedAccessDenied(root / L"setup.sqlite3");
            require(!MoveFileExW(root.c_str(), (root.wstring() + L"-moved").c_str(), 0),
                    "Open protected store directory must resist replacement");
            for (const auto* invalid : {L"..\\escape", L"BA", L"not-a-sid", L""})
                rejects([&] {
                    (void)first.snapshot(invalid);
                });
            auto odd = request;
            odd.channels = 3;
            odd.expectedRevision = 1;
            odd.operationId.back() = 'd';
            rejects([&] {
                (void)first.accept(sid, odd);
            });
            require(first.snapshot(sid).revision == 1, "Rejected format must not advance revision");
            DeviceIntentStore second(root);
            require(second.accept(sid, request) == 1, "Retry through a second connection");
            require(second.accept(other, request) == 1, "Scope operations to authenticated owner");
            require(first.ownersWithDesiredState() == std::vector<std::wstring>{sid, other},
                    "Discover competing connection owner in canonical order");
            auto next = request;
            next.operationId.back() = 'e';
            next.expectedRevision = 1;
            next.sampleRate = 44100;
            require(second.accept(sid, next) == 2, "Commit through competing connection");
            next.expectedRevision = 1;
            next.operationId.back() = 'f';
            rejects([&] {
                (void)first.accept(sid, next);
            });
            require(first.snapshot(sid).revision == 2, "Read committed peer revision");
            constexpr auto wireOwner = L"S-1-5-21-1-2-3-1003";
            const auto wire = encodeDeviceIntent(request).dump();
            auto accepted = handleBrokerMessage(first, {wireOwner, wire});
            require(accepted.reply.at("ok") && accepted.reply.at("acceptedRevision") == 1 &&
                        accepted.reconcileRequested,
                    "Broker handler accepts only after durable commit");
            accepted = handleBrokerMessage(second, {wireOwner, wire});
            require(accepted.reply.at("ok") && accepted.reply.at("acceptedRevision") == 1 &&
                        accepted.reconcileRequested,
                    "Lost reply retry remains idempotent and wakes latest-state reconciliation");
            auto forged = encodeDeviceIntent(request);
            forged["owner"] = "S-1-5-18";
            require(!handleBrokerMessage(first, {wireOwner, forged.dump()}).reply.at("ok"),
                    "Reject caller-supplied owner identity");
            require(!handleBrokerMessage(
                         first, {wireOwner, R"({"version":1,"version":1,"action":"status"})"})
                         .reply.at("ok"),
                    "Reject duplicate status fields");
            require(
                !handleBrokerMessage(
                     first, {wireOwner, R"({"version":1,"action":"status","path":"arbitrary"})"})
                     .reply.at("ok"),
                "Reject unknown status query fields");
            auto stale = request;
            stale.operationId.back() = 'd';
            auto failed = handleBrokerMessage(first, {wireOwner, encodeDeviceIntent(stale).dump()});
            require(!failed.reply.at("ok") && !failed.reconcileRequested,
                    "Rejected stale edit must not wake device operations");
            require(first.snapshot(wireOwner).revision == 1 &&
                        first.snapshot(L"S-1-5-18").revision == 0,
                    "Wire requests cannot mutate a forged owner's records");
            auto removal = request;
            removal.operationId.back() = 'e';
            removal.expectedRevision = 1;
            removal.action = DeviceIntentAction::Remove;
            accepted = handleBrokerMessage(first, {wireOwner, encodeDeviceIntent(removal).dump()});
            require(accepted.reply.at("ok") && accepted.reply.at("acceptedRevision") == 2,
                    "Broker removal is durable explicit intent");
            require(first.removalApplied(wireOwner, request.mixId, 2),
                    "Acknowledge isolated wire removal");
            constexpr auto pageOwner = L"S-1-5-21-1-2-3-1010";
            DeviceIntentRequest a;
            a.operationId = "11111111-1111-1111-1111-111111111111";
            a.mixId = "a" + std::string(4095, '\x01');
            a.name = std::string(4096, '\x01');
            require(first.accept(pageOwner, a) == 1, "Seed first escaped snapshot record");
            auto b = a;
            b.operationId = "22222222-2222-2222-2222-222222222222";
            b.mixId[0] = 'b';
            b.expectedRevision = 1;
            require(first.accept(pageOwner, b) == 2, "Seed second escaped snapshot record");
            const auto readPage = [&](const JSON& query) {
                auto result = handleBrokerMessage(first, {pageOwner, query.dump()});
                require(!result.reconcileRequested,
                        "Snapshot read must not wake device operations");
                require(result.reply.dump().size() <= kMaximumDeviceIntentBytes,
                        "Snapshot reply exceeded pipe bound");
                return result.reply;
            };
            const JSON snapshotQuery{{"version", 1}, {"action", "snapshot"}};
            auto page = readPage(snapshotQuery);
            require(page.at("ok") && page.at("revision") == 2 && page.at("records").size() == 1 &&
                        page.at("nextOffset") == 1,
                    "Large escaped records were not paginated");
            JSON continuation{{"version", 1},
                              {"action", "snapshot"},
                              {"offset", page.at("nextOffset")},
                              {"revision", page.at("revision")},
                              {"tag", page.at("tag")}};
            auto tail = readPage(continuation);
            require(tail.at("ok") && tail.at("records").size() == 1 &&
                        tail.at("nextOffset").is_null() &&
                        decodeDeviceIntent(tail.at("records").front().dump()).mixId == b.mixId,
                    "Snapshot continuation lost or duplicated a record");
            b.operationId = "33333333-3333-3333-3333-333333333333";
            b.expectedRevision = 2;
            b.channels = 8;
            require(first.accept(pageOwner, b) == 3, "Seed concurrent snapshot edit");
            require(readPage(continuation).at("code") == "snapshot_changed",
                    "Snapshot did not detect concurrent edit");
            a.operationId = "44444444-4444-4444-4444-444444444444";
            a.expectedRevision = 3;
            a.action = DeviceIntentAction::Remove;
            require(first.accept(pageOwner, a) == 4, "Seed snapshot removal tombstone");
            page = readPage(snapshotQuery);
            continuation["revision"] = page.at("revision");
            continuation["tag"] = page.at("tag");
            continuation["offset"] = page.at("nextOffset");
            require(first.removalApplied(pageOwner, a.mixId, 4),
                    "Acknowledge tombstone between pages");
            require(first.snapshot(pageOwner).revision == 4,
                    "Removal acknowledgement unexpectedly advanced owner revision");
            require(readPage(continuation).at("code") == "snapshot_changed",
                    "Snapshot tag missed tombstone pruning without revision change");
            const auto isolated = handleBrokerMessage(first, {L"S-1-5-18", snapshotQuery.dump()});
            require(isolated.reply.at("ok") && isolated.reply.at("records").empty(),
                    "Snapshot leaked another owner's desired state");
            require(!readPage(JSON{{"version", 1}, {"action", "snapshot"}, {"owner", "S-1-5-18"}})
                         .at("ok"),
                    "Snapshot accepted forged owner");
            continuation["offset"] = -1;
            require(!readPage(continuation).at("ok"), "Snapshot accepted a negative cursor");
            require(!handleBrokerMessage(
                         first, {pageOwner, R"({"version":1,"version":1,"action":"snapshot"})"})
                         .reply.at("ok"),
                    "Snapshot accepted duplicate keys");
            b.operationId = "55555555-5555-5555-5555-555555555555";
            b.expectedRevision = 4;
            b.action = DeviceIntentAction::Remove;
            require(first.accept(pageOwner, b) == 5 && first.removalApplied(pageOwner, b.mixId, 5),
                    "Remove remaining synthetic pagination record before recovery checks");
        }
        const auto alias = root / L"hard-link.sqlite3";
        require(CreateHardLinkW(alias.c_str(), (root / L"setup.sqlite3").c_str(), nullptr),
                "Create isolated hard-link rejection fixture");
        rejects([&] {
            DeviceIntentStore linked(root);
        });
        require(DeleteFileW(alias.c_str()), "Remove hard-link fixture");
        {
            DeviceIntentStore recovered(root);
            require(recovered.snapshot(sid).revision == 2, "Reopen the protected database");
            require(recovered.ownersWithDesiredState() == std::vector<std::wstring>{sid, other},
                    "Recover owners after all connections close");
            auto removal = request;
            removal.operationId.back() = 'd';
            removal.expectedRevision = 1;
            removal.action = DeviceIntentAction::Remove;
            require(recovered.accept(other, removal) == 2, "Persist explicit removal for recovery");
            require(recovered.ownersWithDesiredState().size() == 2,
                    "Removal tombstone remains discoverable");
            require(!recovered.removalApplied(other, request.mixId, 1),
                    "Stale removal stays pending");
            require(recovered.removalApplied(other, request.mixId, 2),
                    "Acknowledge current removal");
            require(recovered.ownersWithDesiredState() == std::vector<std::wstring>{sid},
                    "Receipt-only owner has no desired devices");
            require(recovered.snapshot(other).revision == 2, "Keep removal retry revision");
        }
        {
            constexpr auto typedOwner = L"S-1-5-21-1-2-3-1009";
            DeviceIntentStore store(root);
            DeviceIntentRequest sink;
            sink.kind = DeviceIntentKind::AppRoutingSink;
            sink.operationId = "12345678-1234-1234-1234-123456789abc";
            const auto accepted =
                handleBrokerMessage(store, {typedOwner, encodeDeviceIntent(sink).dump()});
            require(accepted.reply.at("ok") && accepted.reconcileRequested &&
                        accepted.reply.at("acceptedRevision") == 1,
                    "Typed sink request was not accepted through the authenticated-owner handler");
            require(store.accept(typedOwner, sink) == 1, "Sink retry was not idempotent");
            auto invalid = sink;
            invalid.channels = 4;
            rejects([&] {
                store.accept(typedOwner, invalid);
            });
            invalid = sink;
            invalid.mixId = "forged-mix";
            rejects([&] {
                store.accept(typedOwner, invalid);
            });
            auto forged = encodeDeviceIntent(sink);
            forged["ownerSid"] = "S-1-5-18";
            require(!handleBrokerMessage(store, {typedOwner, forged.dump()}).reply.at("ok"),
                    "Typed request accepted a forged owner field");
            require(store.snapshot(typedOwner).revision == 1,
                    "Rejected sink requests advanced durable revision");
            const BrokerMessage query{typedOwner,
                                      R"({"version":2,"action":"status","kind":"appRoutingSink"})"};
            auto status = handleBrokerMessage(store, query);
            require(status.reply.at("device").at("phase") == "queued", "Sink starts queued");
            require(!status.reply.at("device").contains("id"), "Sink has no public mix identity");
            const auto progress = [&](DeviceIntentKind reportedKind, uint64_t revision) {
                return BrokerProgressQuery(
                    [=](std::wstring_view owner, std::string_view id, DeviceIntentKind kind) {
                        require(owner == typedOwner && id.empty() &&
                                    kind == DeviceIntentKind::AppRoutingSink,
                                "Query retains typed identity");
                        return std::optional<BrokerDeviceProgress>{{std::wstring(owner),
                                                                    std::string(id),
                                                                    revision,
                                                                    BrokerDevicePhase::Ready,
                                                                    {},
                                                                    reportedKind}};
                    });
            };
            status =
                handleBrokerMessage(store, query, progress(DeviceIntentKind::MicrophoneMix, 1));
            require(status.reply.at("device").at("phase") == "queued",
                    "Wrong kind cannot report readiness");
            status =
                handleBrokerMessage(store, query, progress(DeviceIntentKind::AppRoutingSink, 2));
            require(status.reply.at("device").at("phase") == "queued",
                    "Wrong revision cannot report readiness");
            status =
                handleBrokerMessage(store, query, progress(DeviceIntentKind::AppRoutingSink, 1));
            require(status.reply.at("device").at("phase") == "ready",
                    "Exact typed revision reports ready");
            require(
                !handleBrokerMessage(
                     store,
                     {typedOwner,
                      R"({"version":2,"action":"status","kind":"appRoutingSink","mixId":"forged"})"})
                     .reply.at("ok"),
                "Sink status refuses mix identity injection");
        }
        const auto insecure = root / L"inherited-security";
        require(CreateDirectoryW(insecure.c_str(), nullptr), "Create inherited-permission fixture");
        rejects([&] {
            provisionDeviceIntentStore(insecure);
        });
        rejects([&] {
            DeviceIntentStore unprotected(insecure);
        });
        std::filesystem::remove_all(root);
        std::cout << "PASS: protected SQLite store, restricted-token access denial, owner scopes, "
                     "peer revisions, SID/format rejection, hard links and insecure directories\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::wcerr << L"Failed fixture retained for inspection: " << root << L'\n';
        return 1;
    }
}
