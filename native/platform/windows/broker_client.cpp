#include "broker_client.hpp"
#include "broker_binary.hpp"
#include "broker_installation.hpp"
#include "broker_pipe.hpp"
#include <loopz/device_intent.hpp>
#include <array>
#include <stdexcept>
#include <vector>
#include <set>

namespace loopz::windows {
namespace {
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
struct Service {
    SC_HANDLE value{};
    ~Service() {
        if (value)
            CloseServiceHandle(value);
    }
};
[[noreturn]] void failure(const char* message) {
    throw std::runtime_error(std::string(message) + " (Windows error " +
                             std::to_string(GetLastError()) + ")");
}
void verifyServer(HANDLE pipe, Handle& process) {
    ULONG pipePid{};
    if (!GetNamedPipeServerProcessId(pipe, &pipePid) || !pipePid)
        failure("Cannot identify microphone setup server");
    process.value = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pipePid);
    if (!process.value)
        failure("Cannot inspect microphone setup server");
    std::array<wchar_t, 32768> image{};
    DWORD size = static_cast<DWORD>(image.size());
    const auto directory = brokerServiceDirectory();
    const auto expected = directory / L"LoopZDeviceBroker.exe";
    if (!QueryFullProcessImageNameW(process.value, 0, image.data(), &size) ||
        _wcsicmp(std::wstring(image.data(), size).c_str(), expected.c_str()) != 0)
        failure("Unexpected microphone setup server executable");
    Service manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!manager.value)
        failure("Cannot inspect installed microphone setup service");
    Service service{OpenServiceW(manager.value, kDeviceBrokerServiceName,
                                 SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG)};
    if (!service.value)
        failure("Cannot query installed microphone setup service");
    DWORD bytes{};
    QueryServiceConfigW(service.value, nullptr, 0, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(QUERY_SERVICE_CONFIGW) ||
        bytes > 65536)
        failure("Invalid microphone setup service configuration");
    std::vector<BYTE> storage(bytes);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(storage.data());
    if (!QueryServiceConfigW(service.value, config, bytes, &bytes))
        failure("Cannot read microphone setup service configuration");
    const auto command = L"\"" + expected.wstring() + L"\"";
    if (config->dwServiceType != SERVICE_WIN32_OWN_PROCESS || !config->lpBinaryPathName ||
        _wcsicmp(config->lpBinaryPathName, command.c_str()) != 0 || !config->lpServiceStartName ||
        _wcsicmp(config->lpServiceStartName, L"LocalSystem") != 0)
        failure("Unexpected microphone setup service identity");
    SERVICE_STATUS_PROCESS status{};
    ULONG confirmedPid{};
    DWORD exitCode{};
    if (!QueryServiceStatusEx(service.value, SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes) ||
        status.dwCurrentState != SERVICE_RUNNING || status.dwProcessId != pipePid ||
        !GetNamedPipeServerProcessId(pipe, &confirmedPid) || confirmedPid != pipePid ||
        !GetExitCodeProcess(process.value, &exitCode) || exitCode != STILL_ACTIVE)
        failure("Microphone setup server is not the running installed service");
}
DWORD remaining(ULONGLONG deadline) {
    const auto now = GetTickCount64();
    if (now >= deadline)
        failure("Microphone setup request timed out");
    return static_cast<DWORD>(deadline - now);
}
void cancelled(HANDLE cancel) {
    if (WaitForSingleObject(cancel, 0) != WAIT_TIMEOUT)
        failure("Microphone setup request cancelled");
}
DWORD transfer(HANDLE pipe, HANDLE cancel, ULONGLONG deadline, bool write, void* data, DWORD size) {
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event.value)
        failure("Cannot prepare microphone setup request");
    OVERLAPPED operation{};
    operation.hEvent = event.value;
    DWORD count{};
    // Determine timeout before initiating I/O so exceptions cannot abandon it.
    const auto timeout = remaining(deadline);
    cancelled(cancel);
    const auto done = write ? WriteFile(pipe, data, size, &count, &operation)
                            : ReadFile(pipe, data, size, &count, &operation);
    if (done)
        return count;
    if (GetLastError() != ERROR_IO_PENDING)
        failure("Microphone setup transfer failed");
    const std::array<HANDLE, 2> events{cancel, event.value};
    const auto result =
        WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, timeout);
    if (result != WAIT_OBJECT_0 + 1) {
        CancelIoEx(pipe, &operation);
        GetOverlappedResult(pipe, &operation, &count, TRUE);
        failure("Microphone setup request cancelled or timed out");
    }
    if (!GetOverlappedResult(pipe, &operation, &count, FALSE))
        failure("Microphone setup reply is incomplete or exceeds its limit");
    return count;
}
} // namespace
std::string callDeviceBroker(std::string_view request, HANDLE cancel) {
    if (request.empty() || request.size() > kMaximumDeviceIntentBytes || !cancel ||
        cancel == INVALID_HANDLE_VALUE || WaitForSingleObject(cancel, 0) == WAIT_FAILED)
        throw std::invalid_argument("Invalid microphone setup request or cancellation event");
    const auto deadline = GetTickCount64() + 5000;
    Handle pipe;
    for (;;) {
        cancelled(cancel);
        const auto timeout = remaining(deadline);
        pipe.value = CreateFileW(
            kDeviceBrokerPipe, kDeviceBrokerClientAccess, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe.value != INVALID_HANDLE_VALUE)
            break;
        if (GetLastError() != ERROR_PIPE_BUSY)
            failure("Microphone setup service is unavailable");
        WaitNamedPipeW(kDeviceBrokerPipe, timeout < 100 ? timeout : 100);
    }
    Handle server;
    verifyServer(pipe.value, server); // No request bytes sent to an unverified server.
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe.value, &mode, nullptr, nullptr))
        failure("Cannot configure microphone setup connection");
    std::string outgoing(request);
    if (transfer(pipe.value, cancel, deadline, true, outgoing.data(),
                 static_cast<DWORD>(outgoing.size())) != outgoing.size())
        failure("Microphone setup request was incomplete");
    std::string reply(kMaximumDeviceIntentBytes, '\0');
    const auto count = transfer(pipe.value, cancel, deadline, false, reply.data(),
                                static_cast<DWORD>(reply.size()));
    if (!count)
        failure("Microphone setup service returned an empty reply");
    reply.resize(count);
    char ack = kDeviceBrokerReplyAck;
    if (transfer(pipe.value, cancel, deadline, true, &ack, 1) != 1)
        failure("Cannot acknowledge microphone setup reply");
    return reply;
}
DeviceIntentSnapshot readDeviceBrokerSnapshot(HANDLE cancel) {
    const auto deadline = GetTickCount64() + 15000;
    for (unsigned retry = 0; retry < 3; ++retry) {
        DeviceIntentSnapshot result;
        std::string tag;
        size_t offset{};
        for (;;) {
            cancelled(cancel);
            (void)remaining(deadline);
            JSON query{{"version", 1}, {"action", "snapshot"}};
            if (offset) {
                query["offset"] = offset;
                query["revision"] = result.revision;
                query["tag"] = tag;
            }
            std::array<std::set<std::string>, 10> keys;
            const auto reply = JSON::parse(
                callDeviceBroker(query.dump(), cancel),
                [&](int depth, JSON::parse_event_t event, JSON& value) {
                    if (depth < 0 || depth > 8)
                        throw std::invalid_argument("Setup snapshot reply is nested too deeply");
                    if (event == JSON::parse_event_t::object_start)
                        keys[static_cast<size_t>(depth) + 1].clear();
                    else if (event == JSON::parse_event_t::key &&
                             !keys[static_cast<size_t>(depth)]
                                  .insert(value.get<std::string>())
                                  .second)
                        throw std::invalid_argument("Duplicate setup snapshot reply field");
                    return true;
                });
            if (!reply.is_object() || !reply.contains("ok") || !reply.at("ok").is_boolean())
                failure("Invalid microphone setup snapshot reply");
            if (!reply.at("ok").get<bool>()) {
                if (reply.value("code", std::string{}) == "snapshot_changed")
                    break;
                failure("Cannot read microphone setup desired state");
            }
            if (reply.size() != 5 || !reply.contains("revision") ||
                !reply.at("revision").is_number_unsigned() || !reply.contains("tag") ||
                !reply.at("tag").is_string() || !reply.contains("records") ||
                !reply.at("records").is_array() || !reply.contains("nextOffset"))
                failure("Invalid microphone setup snapshot fields");
            const auto pageTag = reply.at("tag").get<std::string>();
            const auto revision = reply.at("revision").get<uint64_t>();
            if (pageTag.size() != 64 ||
                pageTag.find_first_not_of("0123456789abcdef") != std::string::npos)
                failure("Invalid microphone setup snapshot tag");
            if (offset && (revision != result.revision || pageTag != tag))
                failure("Microphone setup snapshot changed without invalidation");
            result.revision = revision;
            tag = pageTag;
            const auto& records = reply.at("records");
            if (records.size() > 256 - result.records.size())
                failure("Microphone setup snapshot exceeds record limit");
            for (const auto& record : records) {
                auto decoded = decodeDeviceIntent(record.dump());
                if (decoded.expectedRevision >= result.revision ||
                    (!result.records.empty() && result.records.back().mixId >= decoded.mixId))
                    failure("Invalid or unordered microphone setup snapshot record");
                result.records.push_back(std::move(decoded));
            }
            const auto& next = reply.at("nextOffset");
            if (next.is_null())
                return result;
            if (!next.is_number_unsigned() || records.empty() ||
                next.get<uint64_t>() != result.records.size() || next.get<uint64_t>() >= 256)
                failure("Invalid microphone setup snapshot cursor");
            offset = result.records.size();
        }
    }
    failure("Microphone setup kept changing while reading desired state");
}
} // namespace loopz::windows
