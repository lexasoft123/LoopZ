#include "ipc.hpp"
#include "runtime.hpp"
#include <windows.h>
#include <objbase.h>
#include <fstream>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(apartment))
        return 1;
    int result = 0;
    try {
        using namespace loopz;
        using namespace loopz::windows;
        const std::wstring_view command = argc > 1 ? argv[1] : L"--state";
        JSON reply;
        if (command == L"--self-test" && argc == 3) {
            const auto folder = std::filesystem::path(argv[2]);
            std::filesystem::create_directories(folder);
            const auto path = folder / L"test-routing.json";
            if (std::filesystem::exists(path))
                throw std::runtime_error("Self-test requires a fresh directory");
            {
                Runtime runtime(path);
                const auto before = runtime.request({{"version", 1}, {"operation", "state"}});
                JSON request{{"version", 1},
                             {"operation", "edit"},
                             {"request",
                              {{"version", 1},
                               {"expected", before.at("snapshot").at("revision")},
                               {"commands", JSON::array({{{"type", "createDevice"},
                                                          {"id", "mix"},
                                                          {"name", "Windows test"},
                                                          {"passThrough", false}}})}}}};
                if (runtime.request(request).value("ok", false) != true)
                    throw std::runtime_error("Create failed");
                if (runtime.request(request).value("code", "") != "conflict")
                    throw std::runtime_error("Stale revision was accepted");
                auto state = runtime.request({{"version", 1}, {"operation", "state"}});
                request["request"] = {{"version", 1},
                                      {"expected", state.at("snapshot").at("revision")},
                                      {"history", "undo"}};
                if (runtime.request(request).value("ok", false) != true)
                    throw std::runtime_error("Undo failed");
                state = runtime.request({{"version", 1}, {"operation", "state"}});
                if (!state.at("snapshot").at("document").at("devices").empty())
                    throw std::runtime_error("Undo did not restore document");
                request["request"]["expected"] = state.at("snapshot").at("revision");
                request["request"]["history"] = "redo";
                if (runtime.request(request).value("ok", false) != true)
                    throw std::runtime_error("Redo failed");
                const auto editPlayback = [&](JSON body) {
                    const auto current = runtime.request({{"version", 1}, {"operation", "state"}});
                    body["version"] = 1;
                    body["expected"] = current.at("snapshot").at("revision");
                    const auto answer =
                        runtime.request({{"version", 1}, {"operation", "edit"}, {"request", body}});
                    if (!answer.value("ok", false))
                        throw std::runtime_error("Playback edit failed: " + answer.dump());
                    return runtime.request({{"version", 1}, {"operation", "state"}});
                };
                editPlayback(
                    {{"commands", JSON::array({{{"type", "addSource"},
                                                {"device", "mix"},
                                                {"id", "app"},
                                                {"kind", "application"},
                                                {"selector", "C:\\LoopZ-unavailable-fixture.exe"},
                                                {"channels", 2},
                                                {"muteOriginal", false},
                                                {"connectByDefault", false}}})}});
                const auto held =
                    editPlayback({{"commands", JSON::array({{{"type", "setSourceControls"},
                                                             {"device", "mix"},
                                                             {"source", "app"},
                                                             {"controls", {{"muted", true}}},
                                                             {"muteOriginal", true}}})}});
                if (!held.at("snapshot")
                         .at("document")
                         .at("devices")[0]
                         .at("bindings")[0]
                         .at("muteOriginal") ||
                    !held.at("snapshot")
                         .at("document")
                         .at("devices")[0]
                         .at("routing")
                         .at("sources")[0]
                         .at("muted") ||
                    held.at("snapshot")
                            .at("runtime")
                            .at("sources")[0]
                            .at("playbackRouting")
                            .at("verified") != false)
                    throw std::runtime_error("Offline playback edit did not preserve intent or "
                                             "claimed verified routing");
                const auto undone = editPlayback({{"history", "undo"}});
                if (undone.at("snapshot")
                        .at("document")
                        .at("devices")[0]
                        .at("bindings")[0]
                        .at("muteOriginal") != false)
                    throw std::runtime_error("Playback edit undo failed");
                const auto redone = editPlayback({{"history", "redo"}});
                if (redone.at("snapshot")
                        .at("document")
                        .at("devices")[0]
                        .at("bindings")[0]
                        .at("muteOriginal") != true)
                    throw std::runtime_error("Playback edit redo failed");
            }
            Runtime reopened(path);
            reply = reopened.request({{"version", 1}, {"operation", "state"}});
            if (reply.at("snapshot").at("document").at("devices").size() != 1)
                throw std::runtime_error("Saved document was not restored");
            auto unsupported = reply.at("snapshot").at("document");
            unsupported["devices"][0]["sampleRate"] = 200000;
            const auto recoveryPath = folder / L"unsupported-routing.json";
            const auto originalBytes = unsupported.dump();
            {
                std::ofstream file(recoveryPath, std::ios::binary);
                file << originalBytes;
                if (!file)
                    throw std::runtime_error("Cannot write recovery fixture");
            }
            {
                Runtime recovery(recoveryPath);
                const auto state = recovery.request({{"version", 1}, {"operation", "state"}});
                if (state.at("diagnostics").at("runtime").get<std::string>().empty() ||
                    state.at("snapshot").at("restoration").at("phase") != "retry" ||
                    state.at("snapshot").at("document") != unsupported)
                    throw std::runtime_error("Unsupported saved format was not recoverable");
                {
                    std::ifstream saved(recoveryPath, std::ios::binary);
                    if (std::string(std::istreambuf_iterator<char>(saved), {}) != originalBytes)
                        throw std::runtime_error("Loading rewrote unsupported saved routing");
                }
                const auto edited =
                    recovery.request({{"version", 1},
                                      {"operation", "edit"},
                                      {"request",
                                       {{"version", 1},
                                        {"expected", state.at("snapshot").at("revision")},
                                        {"commands", JSON::array({{{"type", "setDeviceRate"},
                                                                   {"device", "mix"},
                                                                   {"sampleRate", 48000}}})}}}});
                if (!edited.value("ok", false) ||
                    !recovery.request({{"version", 1}, {"operation", "state"}})
                         .at("diagnostics")
                         .at("runtime")
                         .get<std::string>()
                         .empty())
                    throw std::runtime_error("Cannot repair unsupported saved format by editing");
            }
            reply = {{"ok", true},
                     {"checks",
                      {"create", "stale revision", "undo", "redo", "save", "reopen",
                       "recover unsupported format", "app playback edits"}}};
        } else if (command == L"--stop-engine")
            reply = engineRequest({{"version", 1}, {"operation", "shutdown"}});
        else if (command == L"--state")
            reply = engineRequest({{"version", 1}, {"operation", "state"}});
        else if (command == L"--request" && argc == 3) {
            const auto path = std::filesystem::path(argv[2]);
            if (std::filesystem::file_size(path) > 1024 * 1024)
                throw std::runtime_error("Request is too large");
            std::ifstream file(path, std::ios::binary);
            reply = engineRequest(
                parseControlJSON(std::string(std::istreambuf_iterator<char>(file), {})));
        } else
            throw std::invalid_argument(
                "Use --state, --request JSON-file, or --self-test fresh-directory");
        std::cout << reply.dump() << '\n';
        if (reply.contains("ok") && !reply.value("ok", false))
            result = 2;
    } catch (const std::exception& e) {
        std::cerr << loopz::JSON{{"error", e.what()}}.dump() << '\n';
        result = 1;
    }
    CoUninitialize();
    return result;
}
