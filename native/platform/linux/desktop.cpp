#include <loopz/runtime/control_socket.hpp>
#include <loopz/document_json.hpp>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#ifndef LOOPZ_INTERFACE_PATH
#define LOOPZ_INTERFACE_PATH "/usr/local/share/loopz/interface/index.html"
#endif

namespace {
constexpr size_t maximumMessageBytes = 1024 * 1024;
constexpr unsigned maximumPendingRequests = 8;
std::atomic<unsigned> pendingRequests{0};
struct Host {
    WebKitWebView* view{};
    std::string indexURI;
    std::filesystem::path socket, smokeDirectory;
    loopz::JSON smokeReport;
    int smokeResult{1};
    bool smokeStarted{false};
    bool mapped{false}, hiddenWindowState{false}, pageLoaded{false};
    int publishedVisibility{-1};
};
// GTK reports mapping/minimization independently of keyboard focus. Standard
// GTK/Wayland does not expose reliable full-window occlusion for this purpose.
void publishVisibility(Host& host) {
    const bool visible = host.mapped && !host.hiddenWindowState;
    if (!host.pageLoaded || host.publishedVisibility == int(visible))
        return;
    host.publishedVisibility = int(visible);
    const char* script =
        visible
            ? "if (globalThis.__loopzNativeVisible !== true) { globalThis.__loopzNativeVisible = "
              "true; globalThis.dispatchEvent(new Event('loopz-visibilitychange')); }"
            : "if (globalThis.__loopzNativeVisible !== false) { globalThis.__loopzNativeVisible = "
              "false; globalThis.dispatchEvent(new Event('loopz-visibilitychange')); }";
    webkit_web_view_evaluate_javascript(host.view, script, -1, nullptr, nullptr, nullptr, nullptr,
                                        nullptr);
}
gboolean windowMapped(GtkWidget* widget, GdkEventAny*, gpointer raw) {
    auto& host = *static_cast<Host*>(raw);
    host.mapped = true;
    if (auto* window = gtk_widget_get_window(widget))
        host.hiddenWindowState = (gdk_window_get_state(window) &
                                  (GDK_WINDOW_STATE_ICONIFIED | GDK_WINDOW_STATE_WITHDRAWN)) != 0;
    publishVisibility(host);
    return FALSE;
}
gboolean windowUnmapped(GtkWidget*, GdkEventAny*, gpointer raw) {
    auto& host = *static_cast<Host*>(raw);
    host.mapped = false;
    publishVisibility(host);
    return FALSE;
}
gboolean windowStateChanged(GtkWidget*, GdkEventWindowState* event, gpointer raw) {
    auto& host = *static_cast<Host*>(raw);
    host.hiddenWindowState =
        (event->new_window_state & (GDK_WINDOW_STATE_ICONIFIED | GDK_WINDOW_STATE_WITHDRAWN)) != 0;
    publishVisibility(host);
    return FALSE;
}
void visibilityLoaded(WebKitWebView*, WebKitLoadEvent event, gpointer raw) {
    auto& host = *static_cast<Host*>(raw);
    if (event == WEBKIT_LOAD_STARTED) {
        host.pageLoaded = false;
        host.publishedVisibility = -1;
    } else if (event == WEBKIT_LOAD_FINISHED) {
        host.pageLoaded = true;
        publishVisibility(host);
    }
}
struct Request {
    std::filesystem::path socket;
    std::string bytes, response, error;
    JSCContext* context;
    WebKitScriptMessageReply* reply;
    ~Request() {
        webkit_script_message_reply_unref(reply);
        g_object_unref(context);
        pendingRequests.fetch_sub(1);
    }
};

void execute(GTask* task, gpointer, gpointer raw, GCancellable*) {
    auto& request = *static_cast<Request*>(raw);
    try {
        request.response = loopz::runtime::requestSocket(request.socket, request.bytes);
        if (request.response.empty() ||
            request.response.size() > loopz::runtime::kMaximumControlResponseBytes)
            throw std::runtime_error("The audio service returned an invalid response size");
    } catch (const std::exception& error) {
        request.error = error.what();
    }
    g_task_return_boolean(task, TRUE);
}
void complete(GObject*, GAsyncResult* result, gpointer) {
    auto& request = *static_cast<Request*>(g_task_get_task_data(G_TASK(result)));
    if (!request.error.empty()) {
        webkit_script_message_reply_return_error_message(request.reply, request.error.c_str());
        return;
    }
    JSCValue* value = jsc_value_new_from_json(request.context, request.response.c_str());
    if (!value || jsc_context_get_exception(request.context)) {
        jsc_context_clear_exception(request.context);
        webkit_script_message_reply_return_error_message(request.reply,
                                                         "The audio service returned invalid JSON");
    } else {
        webkit_script_message_reply_return_value(request.reply, value);
    }
    if (value)
        g_object_unref(value);
}
gboolean message(WebKitUserContentManager*, JSCValue* value, WebKitScriptMessageReply* reply,
                 gpointer raw) {
    const auto& host = *static_cast<Host*>(raw);
    const auto* uri = webkit_web_view_get_uri(host.view);
    if (!uri || host.indexURI != uri || !jsc_value_is_object(value)) {
        webkit_script_message_reply_return_error_message(reply, "Untrusted desktop request");
        return TRUE;
    }
    char* json = jsc_value_to_json(value, 0);
    if (!json || std::strlen(json) > maximumMessageBytes) {
        g_free(json);
        webkit_script_message_reply_return_error_message(reply,
                                                         "Desktop request exceeds its limit");
        return TRUE;
    }
    if (pendingRequests.fetch_add(1) >= maximumPendingRequests) {
        pendingRequests.fetch_sub(1);
        g_free(json);
        webkit_script_message_reply_return_error_message(reply, "Audio service requests are busy");
        return TRUE;
    }
    auto* request = new Request{host.socket,
                                json,
                                {},
                                {},
                                JSC_CONTEXT(g_object_ref(jsc_value_get_context(value))),
                                webkit_script_message_reply_ref(reply)};
    g_free(json);
    GTask* task = g_task_new(nullptr, nullptr, complete, nullptr);
    g_task_set_task_data(task, request, [](gpointer item) {
        delete static_cast<Request*>(item);
    });
    g_task_run_in_thread(task, execute);
    g_object_unref(task);
    return TRUE;
}
gboolean policy(WebKitWebView*, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type,
                gpointer raw) {
    const auto& host = *static_cast<Host*>(raw);
    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION ||
        type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        auto* action = webkit_navigation_policy_decision_get_navigation_action(
            WEBKIT_NAVIGATION_POLICY_DECISION(decision));
        const char* uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
        if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION || !uri || host.indexURI != uri)
            webkit_policy_decision_ignore(decision);
        else
            webkit_policy_decision_use(decision);
        return TRUE;
    }
    return FALSE;
}

void finishSmoke(Host& host, bool success, const std::string& detail) {
    host.smokeResult = success ? 0 : 1;
    host.smokeReport["ok"] = success;
    host.smokeReport["detail"] = detail;
    try {
        std::ofstream report(host.smokeDirectory / "report.json");
        report << host.smokeReport.dump(2) << '\n';
        if (!report)
            host.smokeResult = 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Smoke report: %s\n", error.what());
        host.smokeResult = 1;
    }
    if (!success)
        std::fprintf(stderr, "Linux interface smoke: %s\n", detail.c_str());
    gtk_main_quit();
}
void snapshotReady(GObject* object, GAsyncResult* result, gpointer raw) {
    auto& host = *static_cast<Host*>(raw);
    GError* error = nullptr;
    auto* image = webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(object), result, &error);
    if (!image) {
        const std::string detail = error ? error->message : "No rendered snapshot";
        g_clear_error(&error);
        finishSmoke(host, false, detail);
        return;
    }
    const auto status =
        cairo_surface_write_to_png(image, (host.smokeDirectory / "interface.png").c_str());
    cairo_surface_destroy(image);
    finishSmoke(host, status == CAIRO_STATUS_SUCCESS,
                status == CAIRO_STATUS_SUCCESS ? "Native bridge and visible routing verified"
                                               : cairo_status_to_string(status));
}
void smokeEvaluated(GObject* object, GAsyncResult* result, gpointer raw) {
    auto& host = *static_cast<Host*>(raw);
    GError* error = nullptr;
    JSCValue* value = webkit_web_view_call_async_javascript_function_finish(WEBKIT_WEB_VIEW(object),
                                                                            result, &error);
    if (!value) {
        const std::string detail = error ? error->message : "No JavaScript acceptance result";
        g_clear_error(&error);
        finishSmoke(host, false, detail);
        return;
    }
    char* json = jsc_value_to_json(value, 0);
    g_object_unref(value);
    try {
        if (!json)
            throw std::runtime_error("Acceptance result could not be serialized");
        host.smokeReport =
            loopz::parseControlJSON(json, loopz::runtime::kMaximumControlResponseBytes);
        g_free(json);
        json = nullptr;
        if (!host.smokeReport.value("valid", false)) {
            finishSmoke(host, false,
                        "Canonical state or visible routing did not match the fixture");
            return;
        }
        webkit_web_view_get_snapshot(host.view, WEBKIT_SNAPSHOT_REGION_VISIBLE,
                                     WEBKIT_SNAPSHOT_OPTIONS_NONE, nullptr, snapshotReady, &host);
    } catch (const std::exception& failure) {
        g_free(json);
        finishSmoke(host, false, failure.what());
    }
}
void smokeLoaded(WebKitWebView* view, WebKitLoadEvent event, gpointer raw) {
    auto& host = *static_cast<Host*>(raw);
    if (event != WEBKIT_LOAD_FINISHED || host.smokeStarted)
        return;
    host.smokeStarted = true;
    // This calls the production native bridge, then inspects the actual React
    // DOM. No synthetic state, PCM or browser network transport is introduced.
    constexpr auto script = R"JS(
        let state, visibleText = '', heading = null, outputSockets = 0;
        const deadline = performance.now() + 14000;
        do {
            state = await window.webkit.messageHandlers.loopz.postMessage({version:1, operation:'state'});
            heading = [...document.querySelectorAll('h1')].find(el => {
                const box = el.getBoundingClientRect(), style = getComputedStyle(el);
                return el.textContent.trim() === 'Linux smoke mix' && box.width > 0 && box.height > 0 &&
                    box.left < innerWidth && box.top < innerHeight && box.right > 0 && box.bottom > 0 &&
                    style.visibility === 'visible' && style.display !== 'none' && Number(style.opacity) > 0;
            });
            outputSockets = document.querySelectorAll('[data-od-id="outputs"] [data-od-id^="socket-"]').length;
            visibleText = document.body.innerText;
            if (heading && outputSockets >= 2 && state.connection?.ready &&
                state.snapshot?.revision?.generation === '1' &&
                state.snapshot?.document?.devices?.[0]?.name === 'Linux smoke mix') break;
            await new Promise(resolve => setTimeout(resolve, 100));
        } while (performance.now() < deadline);
        return { valid: !!heading && outputSockets >= 2 && state.connection?.ready === true &&
                 state.snapshot?.revision?.generation === '1' &&
                 state.snapshot?.document?.devices?.[0]?.name === 'Linux smoke mix',
                 state, visibleText, outputSockets, viewport:{width:innerWidth,height:innerHeight},
                 heading:heading ? {text:heading.textContent, width:heading.getBoundingClientRect().width,
                                   height:heading.getBoundingClientRect().height} : null };
    )JS";
    webkit_web_view_call_async_javascript_function(view, script, -1, nullptr, nullptr, nullptr,
                                                   nullptr, smokeEvaluated, &host);
}
} // namespace

int main(int argc, char** argv) {
    try {
        std::filesystem::path index = LOOPZ_INTERFACE_PATH;
        std::filesystem::path smoke;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if ((option == "--interface" || option == "--smoke") && i + 1 < argc)
                (option == "--interface" ? index : smoke) = argv[++i];
            else
                throw std::invalid_argument(
                    "Usage: loopz-desktop [--interface PATH] [--smoke DIR]");
        }
        index = std::filesystem::canonical(index);
        if (!std::filesystem::is_regular_file(index))
            throw std::invalid_argument("The interface must be a regular HTML file");
        const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        if (!runtime || !std::filesystem::path(runtime).is_absolute())
            throw std::runtime_error("XDG_RUNTIME_DIR must identify the user runtime directory");
        Host host;
        if (!smoke.empty()) {
            host.smokeDirectory = std::filesystem::absolute(smoke);
            std::filesystem::create_directories(host.smokeDirectory);
        }
        host.socket = std::filesystem::path(runtime) / "loopz/control.sock";
        char* uri = g_filename_to_uri(index.c_str(), nullptr, nullptr);
        if (!uri)
            throw std::runtime_error("The interface path cannot be represented as a file URI");
        host.indexURI = uri;
        g_free(uri);
        if (!gtk_init_check(nullptr, nullptr))
            throw std::runtime_error("A graphical session is required for the LoopZ interface");
        auto* manager = webkit_user_content_manager_new();
        g_signal_connect(manager, "script-message-with-reply-received::loopz", G_CALLBACK(message),
                         &host);
        if (!webkit_user_content_manager_register_script_message_handler_with_reply(
                manager, "loopz", nullptr))
            throw std::runtime_error("Could not register the LoopZ desktop bridge");
        auto* view = webkit_web_view_new_with_user_content_manager(manager);
        host.view = WEBKIT_WEB_VIEW(view);
        g_object_unref(manager);
        g_signal_connect(view, "decide-policy", G_CALLBACK(policy), &host);
        g_signal_connect(view, "load-changed", G_CALLBACK(visibilityLoaded), &host);
        g_signal_connect(
            view, "permission-request",
            G_CALLBACK(+[](WebKitWebView*, WebKitPermissionRequest* request, gpointer) {
                webkit_permission_request_deny(request);
                return TRUE;
            }),
            nullptr);
        auto* settings = webkit_web_view_get_settings(host.view);
        webkit_settings_set_enable_write_console_messages_to_stdout(settings, FALSE);
        webkit_settings_set_allow_file_access_from_file_urls(settings, FALSE);
        webkit_settings_set_allow_universal_access_from_file_urls(settings, FALSE);
        auto* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_widget_add_events(window, GDK_STRUCTURE_MASK);
        g_signal_connect(window, "map-event", G_CALLBACK(windowMapped), &host);
        g_signal_connect(window, "unmap-event", G_CALLBACK(windowUnmapped), &host);
        g_signal_connect(window, "window-state-event", G_CALLBACK(windowStateChanged), &host);
        gtk_window_set_title(GTK_WINDOW(window), "LoopZ");
        gtk_window_set_default_size(GTK_WINDOW(window), 1280, 800);
        gtk_container_add(GTK_CONTAINER(window), view);
        g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
        if (!host.smokeDirectory.empty()) {
            g_signal_connect(view, "load-changed", G_CALLBACK(smokeLoaded), &host);
            g_timeout_add_seconds(
                20,
                +[](gpointer raw) -> gboolean {
                    finishSmoke(*static_cast<Host*>(raw), false,
                                "20-second acceptance deadline exceeded");
                    return G_SOURCE_REMOVE;
                },
                &host);
        }
        webkit_web_view_load_uri(host.view, host.indexURI.c_str());
        gtk_widget_show_all(window);
        gtk_main();
        return host.smokeDirectory.empty() ? 0 : host.smokeResult;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "loopz-desktop: %s\n", error.what());
        return 1;
    }
}
