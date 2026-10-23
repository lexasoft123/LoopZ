#pragma once
#include <loopz/route_document.hpp>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace loopz::windows {
// Control-thread inventory refresh. An unavailable application is a stable
// state, not a reason to stop every healthy mix whenever the UI polls again.
// Retry real capture failures when the bound application is still present.
template <class Application, class Matches>
bool sourceRefreshRequired(const RouteDocument& document, const std::vector<Application>& before,
                           const std::vector<Application>& after,
                           const std::map<std::string, std::string>& failures, Matches matches) {
    const auto membership = [&](const auto& applications, const std::string& selector) {
        std::vector<std::pair<uint32_t, uint64_t>> result;
        for (const auto& app : applications)
            if (matches(app, selector)) {
                uint64_t created = 0;
                if constexpr (requires { &Application::created100ns; })
                    created = app.created100ns;
                result.emplace_back(app.pid, created);
            }
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    };
    for (const auto& device : document.devices) {
        if (!device.routing.enabled)
            continue;
        for (const auto& binding : device.bindings) {
            const auto route = std::find_if(device.routing.sources.begin(),
                                            device.routing.sources.end(), [&](const auto& source) {
                                                return source.id == binding.id;
                                            });
            const bool needsPlaybackCapture =
                binding.kind == SourceKind::Application && binding.muteOriginal;
            if (route == device.routing.sources.end() || !route->enabled ||
                (route->muted && !needsPlaybackCapture))
                continue;
            if (binding.kind == SourceKind::Application) {
                const auto current = membership(after, binding.selector);
                if (current != membership(before, binding.selector) ||
                    (!current.empty() && failures.contains(binding.selector)))
                    return true;
            } else if (failures.contains(binding.selector))
                return true;
        }
    }
    return false;
}
template <class Application>
bool sourceRefreshRequired(const RouteDocument& document, const std::vector<Application>& before,
                           const std::vector<Application>& after,
                           const std::map<std::string, std::string>& failures) {
    return sourceRefreshRequired(document, before, after, failures,
                                 [](const auto& app, const auto& selector) {
                                     if (app.selector == selector)
                                         return true;
                                     if constexpr (requires { &Application::application; })
                                         return app.application == selector;
                                     return false;
                                 });
}
} // namespace loopz::windows
