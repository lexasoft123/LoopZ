// Opt-in normal-user installed-service test. No audio rendering or preferences.
#include "broker_client.hpp"
#include "driver_identity.hpp"
#include "endpoint_roles.hpp"
#include "app_routing_sink_lifetime.hpp"
#include <loopz/document_json.hpp>
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace loopz;
using namespace loopz::windows;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
std::string operation() {
    GUID id{};
    wchar_t text[40]{};
    require(SUCCEEDED(CoCreateGuid(&id)) && StringFromGUID2(id, text, 40),
            "Create service operation identity");
    std::string result;
    for (size_t i = 1; i < 37; ++i) {
        const auto ch = text[i];
        result.push_back(static_cast<char>(ch >= L'A' && ch <= L'F' ? ch - L'A' + L'a' : ch));
    }
    return result;
}
const DeviceIntentRequest* sinkRecord(const DeviceIntentSnapshot& snapshot) {
    const DeviceIntentRequest* found{};
    for (const auto& record : snapshot.records)
        if (record.kind == DeviceIntentKind::AppRoutingSink) {
            require(!found, "Duplicate internal endpoint intent");
            found = &record;
        }
    return found;
}
JSON mixes(const DeviceIntentSnapshot& snapshot) {
    JSON result = JSON::array();
    for (const auto& record : snapshot.records)
        if (record.kind == DeviceIntentKind::MicrophoneMix)
            result.push_back(encodeDeviceIntent(record));
    return result;
}
struct Event {
    HANDLE value{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    ~Event() {
        if (value)
            CloseHandle(value);
    }
};
void normalToken() {
    HANDLE token{};
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE,
            "Read desktop token");
    TOKEN_ELEVATION elevation{};
    DWORD bytes{};
    const bool queried =
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes) != FALSE;
    CloseHandle(token);
    require(queried && !elevation.TokenIsElevated,
            "Installed-service acceptance requires a normal desktop token");
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    JSON report{{"ok", false}, {"cleanup", false}, {"fullWindowsPortComplete", false}};
    if (argc != 2)
        return 2;
    try {
        wchar_t flag[2]{};
        require(GetEnvironmentVariableW(L"LOOPZ_BROKER_SINK_ACCEPTANCE", flag, 2) == 1 &&
                    flag[0] == L'1',
                "Requires explicit installed-service fixture opt-in");
        normalToken();
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)),
                "Initialize service fixture MTA");
        struct Apartment {
            ~Apartment() {
                CoUninitialize();
            }
        } apartment;
        Event cancel;
        require(cancel.value != nullptr, "Create service cancellation event");
        const auto owner = currentOwnerSid();
        const auto beforeDefaults = audioDefaultEndpoints();
        const auto beforeEndpoints = loopzEndpoints();
        const auto before = readDeviceBrokerSnapshot(cancel.value);
        require(!sinkRecord(before) && !appRoutingSinkRootPresent(owner),
                "An internal endpoint already exists; fixture will not claim it");
        const auto originals = mixes(before);
        DeviceIntentRequest upsert;
        upsert.kind = DeviceIntentKind::AppRoutingSink;
        upsert.operationId = operation();
        upsert.expectedRevision = before.revision;
        DeviceIntentRequest removal;
        bool attempted = false;
        const auto removeOwn = [&] {
            auto current = readDeviceBrokerSnapshot(cancel.value);
            const auto* stored = sinkRecord(current);
            if (stored) {
                require(stored->operationId == upsert.operationId ||
                            (!removal.operationId.empty() &&
                             stored->operationId == removal.operationId),
                        "Newer internal endpoint intent is not this fixture's property");
                if (stored->action != DeviceIntentAction::Remove) {
                    removal.kind = DeviceIntentKind::AppRoutingSink;
                    removal.action = DeviceIntentAction::Remove;
                    removal.operationId = operation();
                    removal.expectedRevision = current.revision;
                    const auto reply = JSON::parse(
                        callDeviceBroker(encodeDeviceIntent(removal).dump(), cancel.value));
                    require(reply.value("ok", false) &&
                                reply.at("acceptedRevision") == current.revision + 1,
                            "Service did not accept internal endpoint removal");
                }
            }
            const auto deadline = GetTickCount64() + 60000;
            do {
                current = readDeviceBrokerSnapshot(cancel.value);
                if (!sinkRecord(current) && !appRoutingSinkRootPresent(owner))
                    return;
                Sleep(100);
            } while (GetTickCount64() < deadline);
            throw std::runtime_error("Service internal endpoint removal remains pending");
        };
        try {
            attempted = true;
            const auto reply =
                JSON::parse(callDeviceBroker(encodeDeviceIntent(upsert).dump(), cancel.value));
            require(reply.value("ok", false) && reply.at("acceptedRevision") == before.revision + 1,
                    "Normal user internal endpoint request was not accepted");
            report["acceptedRevision"] = reply.at("acceptedRevision");
            const auto duplicate =
                JSON::parse(callDeviceBroker(encodeDeviceIntent(upsert).dump(), cancel.value));
            require(duplicate.value("ok", false) &&
                        duplicate.at("acceptedRevision") == reply.at("acceptedRevision"),
                    "Installed service did not replay the same operation idempotently");
            const auto deadline = GetTickCount64() + 60000;
            bool ready = false;
            JSON status;
            do {
                status = JSON::parse(callDeviceBroker(
                    R"({"version":2,"action":"status","kind":"appRoutingSink"})", cancel.value));
                require(status.value("ok", false), "Installed service typed status failed");
                ready = status.at("device").value("phase", std::string{}) == "ready" &&
                        status.at("device").at("revision") == before.revision + 1;
                if (!ready)
                    Sleep(100);
            } while (!ready && GetTickCount64() < deadline);
            require(ready,
                    ("Installed service did not finish internal endpoint setup: " + status.dump())
                        .c_str());
            const auto observed = observeAppRoutingSink(owner);
            require(!observed.endpoint.empty() && observed.endpointsExamined &&
                        observed.defaultsExamined && !observed.defaultEndpoint &&
                        !observed.occupied && observed.error.empty(),
                    "Installed service endpoint identity or idle/default observations failed");
            unsigned count = 0;
            for (const auto& [uid, endpoint] : loopzEndpoints(true)) {
                (void)uid;
                if (endpoint.mixKey == driverAppRoutingSinkKey(owner)) {
                    require(endpoint.role == EndpointRole::Feed &&
                                (endpoint.parentVerified || endpoint.topologyVerified),
                            "Service internal endpoint exposed a microphone or lacked hardware "
                            "identity");
                    ++count;
                }
            }
            require(count == 1,
                    "Service internal endpoint does not have exactly one render endpoint");
            report["renderEndpoint"] = observed.endpoint;
            report["normalTokenProvisioned"] = true;
            report["idempotentReplay"] = true;
            auto stale = upsert;
            stale.operationId = operation();
            stale.action = DeviceIntentAction::Remove;
            const auto staleReply =
                JSON::parse(callDeviceBroker(encodeDeviceIntent(stale).dump(), cancel.value));
            require(!staleReply.value("ok", false),
                    "Installed service accepted stale removal revision");
            auto forged = encodeDeviceIntent(upsert);
            forged["ownerSid"] = "S-1-5-18";
            require(!JSON::parse(callDeviceBroker(forged.dump(), cancel.value)).value("ok", false),
                    "Installed service accepted a forged owner field");
            require(readDeviceBrokerSnapshot(cancel.value).revision == before.revision + 1,
                    "Rejected service requests changed durable revision");
            removeOwn();
            require(mixes(readDeviceBrokerSnapshot(cancel.value)) == originals,
                    "Service fixture changed ordinary mix intents");
            require(audioDefaultEndpoints() == beforeDefaults,
                    "Service fixture changed default roles");
            const auto afterEndpoints = loopzEndpoints();
            for (const auto& [uid, endpoint] : beforeEndpoints) {
                const auto after = afterEndpoints.find(uid);
                require(after != afterEndpoints.end() && after->second.role == endpoint.role &&
                            after->second.mixKey == endpoint.mixKey,
                        "Service fixture replaced a pre-existing LoopZ endpoint");
            }
            report["normalTokenRemoved"] = true;
            report["ordinaryMixesPreserved"] = true;
            report["defaultsPreserved"] = true;
            report["cleanup"] = true;
            report["ok"] = true;
        } catch (const std::exception& error) {
            report["error"] = error.what();
            if (attempted)
                try {
                    removeOwn();
                    report["cleanup"] = true;
                } catch (const std::exception& cleanup) {
                    report["cleanupError"] = cleanup.what();
                }
        }
    } catch (const std::exception& error) {
        report["error"] = error.what();
    }
    std::ofstream output(std::filesystem::path(argv[1]), std::ios::binary);
    output << report.dump(2) << '\n';
    output.close();
    return output && report.at("ok").get<bool>() && report.at("cleanup").get<bool>() ? 0 : 1;
}
