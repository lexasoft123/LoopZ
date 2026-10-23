#include "ipc.hpp"
#include "runtime.hpp"
#include "driver_identity.hpp"
#include "microphone_selection.hpp"
#include <array>
#include <algorithm>
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <WebView2.h>
#include <wrl.h>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>

namespace {
using namespace loopz;
using namespace loopz::windows;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
constexpr UINT kReply = WM_APP + 1;
constexpr wchar_t kOrigin[] = L"https://loopz.local/";
struct Request {
    uint64_t id;
    JSON message;
};
struct Reply {
    uint64_t id;
    JSON result;
    std::string error;
    bool closeWindow{false};
};
struct Desktop {
    HWND window{};
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> view;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Request> requests;
    bool closing{false};
    bool navigationReady{false}, bridgeReady{false};
    std::thread worker;
    ~Desktop() {
        stopWorker();
    }
    void writeStatus() noexcept {
        try {
            std::ofstream file(userDirectory() / L"desktop-status.json", std::ios::trunc);
            file << JSON{{"navigationReady", navigationReady},
                         {"bridgeReady", bridgeReady},
                         {"processId", GetCurrentProcessId()}}
                        .dump();
        } catch (...) {
        }
    }
    void startWorker() {
        worker = std::thread([this] {
            for (;;) {
                Request request;
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [this] {
                        return closing || !requests.empty();
                    });
                    if (closing)
                        return;
                    request = std::move(requests.front());
                    requests.pop_front();
                }
                auto reply = std::make_unique<Reply>();
                reply->id = request.id;
                try {
                    if (request.message.value("operation", "") == "driver-sync") {
                        const auto selections = [](const JSON& state) {
                            MicrophoneSelections result;
                            if (!state.contains("snapshot") ||
                                !state.at("snapshot").contains("runtime"))
                                return result;
                            const auto& runtime = state.at("snapshot").at("runtime");
                            if (!runtime.contains("devices"))
                                return result;
                            for (const auto& device : runtime.at("devices"))
                                if (device.contains("microphone") &&
                                    device.at("microphone").contains("uid"))
                                    result.emplace(
                                        device.at("id").get<std::string>(),
                                        device.at("microphone").at("uid").get<std::string>());
                            return result;
                        };
                        MicrophoneSelections before;
                        try {
                            before =
                                selections(engineRequest({{"version", 1}, {"operation", "state"}}));
                        } catch (...) {
                            // An unavailable engine must not prevent repair.
                        }
                        std::array<wchar_t, 32768> executable{};
                        const auto length = GetModuleFileNameW(
                            nullptr, executable.data(), static_cast<DWORD>(executable.size()));
                        if (!length || length == executable.size())
                            throw std::runtime_error("Cannot locate microphone installer");
                        const auto helper = std::filesystem::path(executable.data()).parent_path() /
                                            L"loopz-driver-setup.exe";
                        const auto parameters = L"--install-service";
                        SHELLEXECUTEINFOW launch{sizeof(launch)};
                        launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
                        launch.hwnd = window;
                        launch.lpVerb = L"runas";
                        launch.lpFile = helper.c_str();
                        launch.lpParameters = parameters;
                        launch.nShow = SW_HIDE;
                        if (!ShellExecuteExW(&launch))
                            throw std::runtime_error(
                                GetLastError() == ERROR_CANCELLED
                                    ? "Microphone setup was cancelled. Your mixes are saved."
                                    : "Cannot open microphone setup. Install the complete LoopZ "
                                      "package.");
                        const auto waited = WaitForSingleObject(launch.hProcess, 60000);
                        DWORD exitCode{};
                        const bool completed = waited == WAIT_OBJECT_0 &&
                                               GetExitCodeProcess(launch.hProcess, &exitCode);
                        CloseHandle(launch.hProcess);
                        if (!completed)
                            throw std::runtime_error("Microphone setup is still running. Retry "
                                                     "audio after it finishes.");
                        if (exitCode != 0 && exitCode != 3010)
                            throw std::runtime_error(
                                "Microphone setup failed. A signed LoopZ driver package "
                                "is required. See Windows SetupAPI logs.");
                        reply->result =
                            engineRequest({{"version", 1}, {"operation", "retry-session"}});
                        if (exitCode == 0 && reply->result.value("ok", false)) {
                            reply->result["microphoneSetupStarted"] = true;
                            try {
                                const auto after = selections(
                                    engineRequest({{"version", 1}, {"operation", "state"}}));
                                reply->result["microphoneSelectionsChanged"] =
                                    microphoneSelectionsChanged(before, after);
                            } catch (...) {
                                // Setup succeeded; a missing snapshot proves no change.
                            }
                        }
                        if (exitCode == 3010)
                            throw std::runtime_error(
                                "Microphones installed. Restart Windows to finish setup.");
                    } else {
                        reply->result = engineRequest(request.message);
                    }
                    reply->closeWindow = request.message.value("operation", "") == "shutdown" &&
                                         reply->result.value("ok", false);
                } catch (const std::exception& e) {
                    reply->error = e.what();
                }
                if (PostMessageW(window, kReply, 0, reinterpret_cast<LPARAM>(reply.get())))
                    (void)reply.release();
            }
        });
    }
    void stopWorker() {
        {
            std::lock_guard lock(mutex);
            closing = true;
        }
        changed.notify_all();
        if (worker.joinable())
            worker.join();
    }
    void resize() {
        if (controller) {
            RECT rect{};
            GetClientRect(window, &rect);
            controller->put_Bounds(rect);
        }
    }
    void fail(const wchar_t* message) {
        MessageBoxW(window, message, L"LoopZ", MB_OK | MB_ICONERROR);
    }
    void initialize() {
        writeStatus();
        const auto data = userDirectory() / L"WebView2";
        const auto hr = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, data.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                    if (FAILED(result) || !env) {
                        fail(L"Install Microsoft Edge WebView2 Runtime, then open LoopZ again.");
                        return S_OK;
                    }
                    return env->CreateCoreWebView2Controller(
                        window,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [this](HRESULT created, ICoreWebView2Controller* value) -> HRESULT {
                                if (FAILED(created) || !value) {
                                    fail(L"Cannot create the LoopZ window.");
                                    return S_OK;
                                }
                                controller = value;
                                controller->get_CoreWebView2(&view);
                                resize();
                                ComPtr<ICoreWebView2Settings> settings;
                                view->get_Settings(&settings);
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                ComPtr<ICoreWebView2_3> mapping;
                                if (FAILED(view.As(&mapping))) {
                                    fail(L"Update Microsoft Edge WebView2 Runtime.");
                                    return S_OK;
                                }
                                wchar_t module[32768]{};
                                GetModuleFileNameW(nullptr, module, 32768);
                                const auto folder =
                                    std::filesystem::path(module).parent_path() / L"interface";
                                if (!std::filesystem::exists(folder / L"index.html")) {
                                    fail(L"LoopZ interface files are missing. Extract the entire "
                                         L"download before opening LoopZ.");
                                    return S_OK;
                                }
                                if (FAILED(mapping->SetVirtualHostNameToFolderMapping(
                                        L"loopz.local", folder.c_str(),
                                        COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY))) {
                                    fail(L"Cannot load the LoopZ interface.");
                                    return S_OK;
                                }
                                EventRegistrationToken token;
                                view->add_NavigationCompleted(
                                    Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                        [this](ICoreWebView2*,
                                               ICoreWebView2NavigationCompletedEventArgs* args)
                                            -> HRESULT {
                                            BOOL success = FALSE;
                                            args->get_IsSuccess(&success);
                                            navigationReady = success != FALSE;
                                            writeStatus();
                                            return S_OK;
                                        })
                                        .Get(),
                                    &token);
                                view->add_NavigationStarting(
                                    Callback<ICoreWebView2NavigationStartingEventHandler>(
                                        [](ICoreWebView2*,
                                           ICoreWebView2NavigationStartingEventArgs* args)
                                            -> HRESULT {
                                            LPWSTR uri = nullptr;
                                            args->get_Uri(&uri);
                                            const bool trusted =
                                                uri && std::wstring_view(uri).starts_with(kOrigin);
                                            CoTaskMemFree(uri);
                                            if (!trusted)
                                                args->put_Cancel(TRUE);
                                            return S_OK;
                                        })
                                        .Get(),
                                    &token);
                                view->add_NewWindowRequested(
                                    Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                        [](ICoreWebView2*,
                                           ICoreWebView2NewWindowRequestedEventArgs* args)
                                            -> HRESULT {
                                            args->put_Handled(TRUE);
                                            return S_OK;
                                        })
                                        .Get(),
                                    &token);
                                view->add_PermissionRequested(
                                    Callback<ICoreWebView2PermissionRequestedEventHandler>(
                                        [](ICoreWebView2*,
                                           ICoreWebView2PermissionRequestedEventArgs* args)
                                            -> HRESULT {
                                            args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                                            return S_OK;
                                        })
                                        .Get(),
                                    &token);
                                view->add_WebMessageReceived(
                                    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                        [this](ICoreWebView2*,
                                               ICoreWebView2WebMessageReceivedEventArgs* args)
                                            -> HRESULT {
                                            LPWSTR origin = nullptr;
                                            args->get_Source(&origin);
                                            const bool trusted =
                                                origin &&
                                                std::wstring_view(origin).starts_with(kOrigin);
                                            CoTaskMemFree(origin);
                                            if (!trusted)
                                                return S_OK;
                                            LPWSTR raw = nullptr;
                                            args->get_WebMessageAsJson(&raw);
                                            try {
                                                const auto json =
                                                    parseControlJSON(utf8(raw ? raw : L""));
                                                const auto id = json.at("id").get<uint64_t>();
                                                std::lock_guard lock(mutex);
                                                if (requests.size() < 32) {
                                                    requests.push_back({id, json.at("message")});
                                                    changed.notify_one();
                                                }
                                            } catch (...) {
                                            }
                                            CoTaskMemFree(raw);
                                            return S_OK;
                                        })
                                        .Get(),
                                    &token);
                                return view->Navigate(L"https://loopz.local/index.html");
                            })
                            .Get());
                })
                .Get());
        if (FAILED(hr))
            fail(L"Microsoft Edge WebView2 Runtime is required to open LoopZ.");
    }
};
LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* desktop = reinterpret_cast<Desktop*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        desktop = static_cast<Desktop*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(desktop));
        desktop->window = hwnd;
    }
    if (desktop) {
        if (message == WM_DPICHANGED) {
            const auto* bounds = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd, nullptr, bounds->left, bounds->top, bounds->right - bounds->left,
                         bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
            desktop->resize();
            return 0;
        }
        if (message == WM_SIZE) {
            desktop->resize();
            return 0;
        }
        if (message == kReply) {
            const std::unique_ptr<Reply> reply(reinterpret_cast<Reply*>(lparam));
            JSON json{{"id", reply->id}, {"result", reply->result}, {"error", reply->error}};
            if (desktop->view) {
                desktop->view->PostWebMessageAsJson(wide(json.dump()).c_str());
                if (!desktop->bridgeReady && reply->result.contains("snapshot")) {
                    desktop->bridgeReady = true;
                    desktop->writeStatus();
                }
            }
            if (reply->closeWindow)
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (message == WM_CLOSE) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const bool background = argc == 2 && std::wstring_view(argv[1]) == L"--background";
    const bool engine = background || (argc == 2 && std::wstring_view(argv[1]) == L"--engine");
    if (argv)
        LocalFree(argv);
    const HRESULT initialized =
        CoInitializeEx(nullptr, engine ? COINIT_MULTITHREADED : COINIT_APARTMENTTHREADED);
    if (FAILED(initialized))
        return 1;
    int result = 0;
    try {
        if (background)
            result = runBackground();
        else if (engine)
            result = runEngine();
        else {
            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            startEngine();
            Desktop desktop;
            WNDCLASSW klass{};
            klass.lpfnWndProc = windowProc;
            klass.hInstance = instance;
            klass.lpszClassName = L"LoopZ.Windows";
            klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            klass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
            RegisterClassW(&klass);
            POINT cursor{};
            GetCursorPos(&cursor);
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
            const auto dpi = GetDpiForSystem();
            const int margin = MulDiv(24, static_cast<int>(dpi), 96);
            const int width =
                std::min(MulDiv(1280, static_cast<int>(dpi), 96),
                         static_cast<int>(monitor.rcWork.right - monitor.rcWork.left) - margin * 2);
            const int height =
                std::min(MulDiv(820, static_cast<int>(dpi), 96),
                         static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top) - margin * 2);
            const int left =
                monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2;
            const int top =
                monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2;
            HWND window =
                CreateWindowExW(0, klass.lpszClassName, L"LoopZ", WS_OVERLAPPEDWINDOW, left, top,
                                width, height, nullptr, nullptr, instance, &desktop);
            if (!window)
                throw std::runtime_error("Cannot create LoopZ window");
            desktop.startWorker();
            desktop.initialize();
            ShowWindow(window, show);
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            desktop.stopWorker();
            if (desktop.controller)
                desktop.controller->Close();
            while (PeekMessageW(&message, nullptr, kReply, kReply, PM_REMOVE))
                delete reinterpret_cast<Reply*>(message.lParam);
        }
    } catch (const std::exception& e) {
        if (!engine)
            MessageBoxW(nullptr, wide(e.what()).c_str(), L"LoopZ", MB_OK | MB_ICONERROR);
        result = 1;
    }
    CoUninitialize();
    return result;
}
