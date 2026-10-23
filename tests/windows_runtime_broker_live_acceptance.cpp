// Opt-in adjunct: parent owns an empty production store and one temporary mix.
#include "runtime.hpp"
#include "broker_client.hpp"
#include "driver_identity.hpp"
#include "endpoint_roles.hpp"
#include <fstream>
#include <thread>
#include <iostream>
#include <stdexcept>

namespace loopz::windows {
namespace {
void require(bool value, const char* error) {
    if (!value)
        throw std::runtime_error(error);
}
JSON state(Runtime& runtime) {
    return runtime.request({{"version", 1}, {"operation", "state"}});
}
template <class Predicate> JSON await(Runtime& runtime, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;) {
        auto result = state(runtime);
        if (predicate(result))
            return result;
        require(std::chrono::steady_clock::now() < deadline,
                "Runtime microphone convergence timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
void edit(Runtime& runtime, const JSON& command) {
    const auto before = state(runtime);
    const auto reply = runtime.request({{"version", 1},
                                        {"operation", "edit"},
                                        {"request",
                                         {{"version", 1},
                                          {"expected", before.at("snapshot").at("revision")},
                                          {"commands", JSON::array({command})}}}});
    require(reply.value("ok", false), "Runtime fixture edit failed");
}
struct Directory {
    std::filesystem::path path;
    ~Directory() {
        std::error_code ignored;
        std::filesystem::remove(path / L"routing.json", ignored);
        std::filesystem::remove(path / L"routing.json.bak", ignored);
        std::filesystem::remove(path, ignored);
    }
};
} // namespace
void runRuntimeBrokerAcceptance(const std::string& mixId) {
    const auto owner = currentOwnerSid();
    const auto endpoints = [&] {
        std::map<std::string, EndpointRole> result;
        for (const auto& [uid, endpoint] : loopzEndpoints())
            if (endpoint.mixKey == driverMixKey(owner, mixId))
                result.emplace(uid, endpoint.role);
        return result;
    };
    HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    require(cancel != nullptr, "Cannot query runtime fixture baseline");
    struct Close {
        HANDLE value;
        ~Close() {
            CloseHandle(value);
        }
    } close{cancel};
    require(readDeviceBrokerSnapshot(cancel).records.empty() && endpoints().empty(),
            "Runtime fixture requires empty parent store and absent temporary mix");
    const auto folder = std::filesystem::temp_directory_path() / ("LoopZ-runtime-" + mixId);
    require(std::filesystem::create_directory(folder), "Runtime fixture path already exists");
    Directory cleanup{folder};
    const auto path = folder / L"routing.json";
    std::map<std::string, EndpointRole> initial;
    const auto connected = [&](const JSON& result) {
        const auto& setup = result.at("microphoneSetup");
        if (!setup.value("accepted", false) || !setup.at("devices").contains(mixId) ||
            setup.at("devices").at(mixId).at("phase") != "ready")
            return false;
        for (const auto& device : result.at("snapshot").at("runtime").at("devices"))
            if (device.at("id").get<std::string>() == mixId)
                return device.at("microphone").value("connected", false);
        return false;
    };
    {
        Runtime runtime(path, true);
        const auto missing = await(runtime, [](const auto& result) {
            return result.at("microphoneSetup").value("examined", false);
        });
        require(!missing.at("microphoneSetup").value("authoritative", true) &&
                    !missing.at("microphoneSetup").value("accepted", true) &&
                    readDeviceBrokerSnapshot(cancel).records.empty(),
                "Missing document authorized microphone mutations");
        edit(runtime, {{"type", "createDevice"},
                       {"id", mixId},
                       {"name", "Runtime live acceptance"},
                       {"passThrough", false}});
        const auto ready = await(runtime, connected);
        require(ready.at("microphoneSetup").value("authoritative", false),
                "Saved edit did not become authoritative");
        initial = endpoints();
        require(initial.size() == 2, "Runtime did not create both microphone endpoints");
        edit(runtime,
             {{"type", "renameDevice"}, {"device", mixId}, {"name", "Runtime saved latest"}});
        await(runtime, connected);
        require(endpoints() == initial, "Runtime rename changed endpoint IDs");
    }
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "invalid-primary";
        require(static_cast<bool>(file), "Cannot prepare backup recovery fixture");
    }
    {
        Runtime recovered(path, true);
        const auto paused = await(recovered, [](const auto& result) {
            return result.at("microphoneSetup").value("examined", false);
        });
        require(!paused.at("microphoneSetup").value("authoritative", true) &&
                    !paused.at("microphoneSetup").value("accepted", true) && endpoints() == initial,
                "Backup recovery authorized microphone mutations");
        edit(recovered, {{"type", "renameDevice"},
                         {"device", mixId},
                         {"name", "Runtime recovered saved edit"}});
        await(recovered, connected);
        require(endpoints() == initial, "Runtime recovery changed endpoint IDs");
        edit(recovered, {{"type", "deleteDevice"}, {"device", mixId}});
        await(recovered, [](const auto& result) {
            const auto& setup = result.at("microphoneSetup");
            return setup.value("accepted", false) && setup.at("devices").empty();
        });
    }
    require(readDeviceBrokerSnapshot(cancel).records.empty() && endpoints().empty(),
            "Runtime deletion left temporary state or endpoints");
    std::cout << "PASS: Runtime durable edits, endpoint attachment, backup authority and removal\n";
}
} // namespace loopz::windows
