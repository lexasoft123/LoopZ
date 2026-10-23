// Opt-in administrative runtime integration; device operations are simulated.
#include "broker_runtime.hpp"
#include "broker_handler.hpp"
#include "driver_identity.hpp"
#include <objbase.h>
#include <array>
#include <future>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <source_location>
#include <atomic>
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
struct Adapter final : BrokerDeviceAdapter {
    HANDLE entered{}, release{};
    std::atomic<unsigned> calls{0};
    BrokerApplyResult apply(std::wstring_view, const DeviceIntentRequest&) override {
        if (calls.fetch_add(1) == 0) {
            SetEvent(entered);
            require(WaitForSingleObject(release, 5000) == WAIT_OBJECT_0);
        }
        return BrokerApplyResult::Applied;
    }
};
int main() {
    std::filesystem::path root;
    try {
        root = deviceIntentStoreRoot().parent_path() /
               (L"LoopZBrokerRuntimeTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()));
        require(!std::filesystem::exists(root));
        provisionDeviceIntentStore(root);
        const auto owner = currentOwnerSid();
        DeviceIntentRequest request;
        request.operationId = "12345678-1234-1234-1234-123456789abc";
        request.mixId = "runtime-test";
        request.name = "Runtime acceptance";
        {
            DeviceIntentStore seed(root);
            require(seed.accept(owner, request) == 1);
        }
        Handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        Handle entered{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        Handle release{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        Handle applied{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        Handle removed{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        Adapter adapter;
        adapter.entered = entered.value;
        adapter.release = release.value;
        std::atomic<bool> staleReady{false};
        std::atomic<bool> readyNotified{false};
        const auto name = uniqueName();
        auto runtime = std::async(std::launch::async, [&] {
            runBrokerRuntime(
                root, name, adapter, stop.value,
                [&](const auto& update) {
                    if (update.phase == BrokerDevicePhase::Ready && update.revision == 1)
                        staleReady.store(true);
                    if (update.phase == BrokerDevicePhase::Ready && update.revision == 2)
                        SetEvent(applied.value);
                    if (update.phase == BrokerDevicePhase::Removed && update.revision == 3)
                        SetEvent(removed.value);
                },
                [&] {
                    readyNotified.store(true);
                });
        });
        StopOnExit cleanup{stop.value};
        struct ReleaseOnExit {
            HANDLE event;
            ~ReleaseOnExit() {
                SetEvent(event);
            }
        } unblock{release.value};
        require(WaitForSingleObject(entered.value, 5000) == WAIT_OBJECT_0);
        const auto call = [&](std::string bytes) {
            Handle client;
            connect(client, name);
            transfer(client.value, true, bytes.data(), static_cast<DWORD>(bytes.size()));
            std::array<char, 8192> response{};
            const auto count =
                transfer(client.value, false, response.data(), static_cast<DWORD>(response.size()));
            auto result = JSON::parse(std::string_view(response.data(), count));
            char ack = kDeviceBrokerReplyAck;
            transfer(client.value, true, &ack, 1);
            return result;
        };
        constexpr auto status = R"({"version":1,"action":"status","mixId":"runtime-test"})";
        require(call(status).at("device").at("phase") == "applying");
        require(readyNotified.load());
        auto newer = request;
        newer.operationId.back() = 'd';
        newer.expectedRevision = 1;
        newer.channels = 8;
        require(call(encodeDeviceIntent(newer).dump()).at("acceptedRevision") == 2);
        // Old applying/ready progress must not describe the newer desired edit.
        auto pending = call(status);
        require(pending.at("device").at("revision") == 2 &&
                pending.at("device").at("phase") == "queued");
        SetEvent(release.value);
        require(WaitForSingleObject(applied.value, 5000) == WAIT_OBJECT_0);
        require(!staleReady.load() && adapter.calls.load() == 2);
        require(call(status).at("device").at("phase") == "ready");
        require(call(encodeDeviceIntent(request).dump()).at("acceptedRevision") == 1);
        require(call(status).at("device").at("revision") == 2);
        require(adapter.calls.load() == 2); // Old retry wakes latest state, not old PnP.
        auto removal = newer;
        removal.operationId.back() = 'e';
        removal.expectedRevision = 2;
        removal.action = DeviceIntentAction::Remove;
        require(call(encodeDeviceIntent(removal).dump()).at("acceptedRevision") == 3);
        require(WaitForSingleObject(removed.value, 5000) == WAIT_OBJECT_0);
        require(call(status).at("device").at("phase") == "unrequested");
        SetEvent(stop.value);
        require(runtime.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        runtime.get();
        {
            DeviceIntentStore recovered(root);
            require(recovered.snapshot(owner).revision == 3 &&
                    recovered.snapshot(owner).records.empty());
        }
        {
            DeviceIntentStore seed(root);
            auto startupIntent = request;
            startupIntent.operationId.back() = 'f';
            startupIntent.expectedRevision = 3;
            require(seed.accept(owner, startupIntent) == 4);
        }
        {
            Handle duplicateStop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
            const auto occupiedName = uniqueName();
            BrokerPipe occupied(duplicateStop.value, occupiedName);
            Adapter unused;
            unused.entered = entered.value;
            unused.release = release.value;
            bool rejected = false;
            try {
                runBrokerRuntime(root, occupiedName, unused, duplicateStop.value);
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            require(rejected && unused.calls.load() == 0); // Bind before any startup device work.
        }
        {
            Handle blockedStop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
            Handle blockedEntered{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
            Handle blockedRelease{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
            Adapter blocked;
            blocked.entered = blockedEntered.value;
            blocked.release = blockedRelease.value;
            const auto blockedName = uniqueName();
            auto draining = std::async(std::launch::async, [&] {
                runBrokerRuntime(root, blockedName, blocked, blockedStop.value);
            });
            StopOnExit blockedCleanup{blockedStop.value};
            ReleaseOnExit blockedUnblock{blockedRelease.value};
            require(WaitForSingleObject(blockedEntered.value, 5000) == WAIT_OBJECT_0);
            SetEvent(blockedStop.value);
            require(draining.wait_for(std::chrono::milliseconds(50)) ==
                    std::future_status::timeout);
            SetEvent(blockedRelease.value);
            require(draining.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
            draining.get(); // Runtime cannot outlive an in-flight device operation.
        }
        std::filesystem::remove_all(root);
        std::cout << "PASS: startup recovery, responsive requests during PnP, no lost wake, "
                     "revision-aware status, retry caching, removal, exclusive startup and joined "
                     "in-flight shutdown\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::wcerr << L"Fixture retained: " << root << L'\n';
        return 1;
    }
}
