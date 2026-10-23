// Opt-in administrative integration fixture; no device operations occur.
#include "broker_handler.hpp"
#include "driver_identity.hpp"
#include <objbase.h>
#include <array>
#include <future>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <source_location>
#include <filesystem>
using namespace loopz;
using namespace loopz::windows;
namespace {
void require(bool value, std::source_location location = std::source_location::current()) {
    if (!value)
        throw std::runtime_error("Broker pipe regression failed at line " +
                                 std::to_string(location.line()) + "; Windows error " +
                                 std::to_string(GetLastError()));
}
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
struct StopOnExit {
    HANDLE event;
    ~StopOnExit() {
        SetEvent(event);
    }
};
DWORD transfer(HANDLE pipe, bool write, void* buffer, DWORD size) {
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    require(event.value != nullptr);
    OVERLAPPED operation{};
    operation.hEvent = event.value;
    DWORD count{};
    const bool done = write ? WriteFile(pipe, buffer, size, &count, &operation) != FALSE
                            : ReadFile(pipe, buffer, size, &count, &operation) != FALSE;
    if (done)
        return count;
    require(GetLastError() == ERROR_IO_PENDING);
    if (WaitForSingleObject(event.value, 3000) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &operation);
        GetOverlappedResult(pipe, &operation, &count, TRUE);
        throw std::runtime_error("Broker fixture I/O timed out");
    }
    require(GetOverlappedResult(pipe, &operation, &count, FALSE) != FALSE);
    return count;
}
void connect(Handle& client, const std::wstring& name, DWORD level = SECURITY_IDENTIFICATION) {
    for (unsigned attempt = 0; attempt < 10; ++attempt) {
        client.value =
            CreateFileW(name.c_str(), kDeviceBrokerClientAccess, 0, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | level, nullptr);
        if (client.value != INVALID_HANDLE_VALUE)
            break;
        require(GetLastError() == ERROR_PIPE_BUSY);
        WaitNamedPipeW(name.c_str(), 500);
    }
    require(client.value != INVALID_HANDLE_VALUE);
    DWORD mode = PIPE_READMODE_MESSAGE;
    require(SetNamedPipeHandleState(client.value, &mode, nullptr, nullptr) != FALSE);
}
std::wstring uniqueName() {
    GUID guid{};
    require(SUCCEEDED(CoCreateGuid(&guid)));
    std::array<wchar_t, 40> text{};
    require(StringFromGUID2(guid, text.data(), static_cast<int>(text.size())) != 0);
    return L"\\\\.\\pipe\\LoopZDeviceBrokerTest-" + std::wstring(text.data());
}
} // namespace
int main() {
    std::filesystem::path root;
    try {
        root = deviceIntentStoreRoot().parent_path() /
               (L"LoopZBrokerLoopTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()));
        require(!std::filesystem::exists(root));
        provisionDeviceIntentStore(root);
        Handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        Handle changed{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        require(stop.value && changed.value);
        const auto name = uniqueName();
        const auto owner = currentOwnerSid();
        BrokerPipe pipe(stop.value, name);
        auto server = std::async(std::launch::async, [&] {
            DeviceIntentStore store(root);
            runBrokerRequests(pipe, store, changed.value);
        });
        StopOnExit cleanup{stop.value};
        const auto call = [&](std::string bytes) {
            Handle client;
            connect(client, name);
            require(transfer(client.value, true, bytes.data(), static_cast<DWORD>(bytes.size())) ==
                    bytes.size());
            std::array<char, 4096> response{};
            const auto count =
                transfer(client.value, false, response.data(), static_cast<DWORD>(response.size()));
            const auto result = JSON::parse(std::string_view(response.data(), count));
            char ack = kDeviceBrokerReplyAck;
            require(transfer(client.value, true, &ack, 1) == 1);
            return result;
        };
        constexpr auto status = R"({"version":1,"action":"status"})";
        require(call(status).at("revision") == 0);
        require(WaitForSingleObject(changed.value, 0) == WAIT_TIMEOUT);
        DeviceIntentRequest request;
        request.operationId = "12345678-1234-1234-1234-123456789abc";
        request.mixId = "wire-loop-test";
        request.name = "Wire loop acceptance";
        auto wire = encodeDeviceIntent(request).dump();
        {
            Handle lost;
            connect(lost, name);
            transfer(lost.value, true, wire.data(),
                     static_cast<DWORD>(wire.size()));
            // Observe commit, then close without reading or acknowledging the
            // reply. The next connection retries the exact operation ID.
            require(WaitForSingleObject(changed.value, 5000) == WAIT_OBJECT_0);
        }
        ResetEvent(changed.value);
        auto reply = call(wire);
        require(reply.at("ok") && reply.at("acceptedRevision") == 1);
        require(WaitForSingleObject(changed.value, 0) == WAIT_OBJECT_0);
        ResetEvent(changed.value);
        auto stale = request;
        stale.operationId.back() = 'd';
        require(!call(encodeDeviceIntent(stale).dump()).at("ok"));
        auto forged = encodeDeviceIntent(request);
        forged["owner"] = "S-1-5-18";
        require(!call(forged.dump()).at("ok"));
        require(WaitForSingleObject(changed.value, 0) == WAIT_TIMEOUT);
        require(call(status).at("revision") == 1);
        auto removal = request;
        removal.operationId.back() = 'e';
        removal.expectedRevision = 1;
        removal.action = DeviceIntentAction::Remove;
        reply = call(encodeDeviceIntent(removal).dump());
        require(reply.at("ok") && reply.at("acceptedRevision") == 2);
        require(WaitForSingleObject(changed.value, 0) == WAIT_OBJECT_0);
        require(call(status).at("desiredRecordCount") == 1); // Explicit tombstone survives.
        SetEvent(stop.value);
        require(server.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        server.get();
        {
            DeviceIntentStore recovered(root);
            const auto snapshot = recovered.snapshot(owner);
            require(snapshot.revision == 2 && snapshot.records.size() == 1);
            require(snapshot.records[0].action == DeviceIntentAction::Remove &&
                    snapshot.records[0].mixId == request.mixId);
            require(recovered.snapshot(L"S-1-5-18").revision == 0);
            require(recovered.ownersWithDesiredState() == std::vector<std::wstring>{owner});
        }
        std::filesystem::remove_all(root);
        std::cout << "PASS: authenticated pipe to protected SQLite, lost-reply retry, stale/forged "
                     "rejection, wake events, tombstone recovery and clean stop\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::wcerr << L"Fixture retained: " << root << L'\n';
        return 1;
    }
}
