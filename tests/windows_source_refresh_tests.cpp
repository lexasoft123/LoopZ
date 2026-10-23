#include "../native/platform/windows/source_refresh.hpp"
#include "../native/platform/windows/worker_recovery.hpp"
#include "../native/platform/windows/microphone_selection.hpp"
#include <cassert>
#include <iostream>

int main() {
    using namespace loopz;
    using namespace loopz::windows;
    WorkerRecovery firstWorker, secondWorker;
    assert(!firstWorker.attempt(0, true));
    assert(firstWorker.attempt(100, false));
    assert(!firstWorker.attempt(101, false));
    assert(secondWorker.attempt(101, false));
    assert(!firstWorker.attempt(3000000099, false));
    assert(firstWorker.attempt(3000000100, false));
    assert(!firstWorker.attempt(3000000101, true));
    assert(firstWorker.attempt(3000000102, false));
    WorkerRecovery overflow;
    assert(overflow.attempt(UINT64_MAX - 2, false));
    assert(!overflow.attempt(UINT64_MAX - 1, false));

    const MicrophoneSelections oldSelections{{"mix", "old"}, {"other", "keep"}};
    assert(!microphoneSelectionsChanged(oldSelections, oldSelections));
    assert(microphoneSelectionsChanged(oldSelections, {{"mix", "new"}, {"other", "keep"}}));
    assert(!microphoneSelectionsChanged(oldSelections, {{"other", "keep"}, {"added", "new"}}));
    assert(!microphoneSelectionsChanged(oldSelections, {{"mix", ""}, {"other", "keep"}}));
    assert(!microphoneSelectionsChanged({}, oldSelections));
    assert(!microphoneSelectionsChanged(oldSelections, {}));
    assert(!microphoneSelectionsChanged({{"mix", ""}}, {{"mix", "new"}}));
    struct App {
        std::string selector;
        uint32_t pid;
    };
    RouteDocument document;
    VirtualDeviceDefinition device;
    device.id = "mix";
    device.bindings = {{"source", SourceKind::Application, "saved-app"}};
    device.routing.sources = {{"source", 2, 1, true, false, {}}};
    document.devices.push_back(device);
    const std::map<std::string, std::string> missing{{"saved-app", "No active session"}};
    const std::vector<App> absent, unrelated{{"another-app", 42}};
    const std::vector<App> present{{"saved-app", 123}}, restarted{{"saved-app", 456}};
    assert(!sourceRefreshRequired(document, absent, absent, missing));
    assert(!sourceRefreshRequired(document, absent, unrelated, missing));
    assert(!sourceRefreshRequired(document, unrelated, absent, missing));
    assert(sourceRefreshRequired(document, absent, present, missing));
    assert(sourceRefreshRequired(document, present, absent, {}));
    assert(sourceRefreshRequired(document, present, restarted, {}));
    assert(sourceRefreshRequired(document, present, present, missing));
    const std::vector<App> reordered{{"another-app", 42}, {"saved-app", 123}};
    assert(!sourceRefreshRequired(document, present, reordered, {}));
    struct Instance {
        std::string selector;
        uint32_t pid;
        uint64_t created100ns;
        std::string application;
    };
    const std::vector<Instance> one{{"saved-app", 123, 10, "canonical"}};
    const std::vector<Instance> two{{"saved-app", 123, 10, "canonical"},
                                    {"saved-app", 456, 20, "canonical"}};
    const std::vector<Instance> reused{{"saved-app", 123, 11, "canonical"}};
    assert(sourceRefreshRequired(document, one, two, {}));
    assert(sourceRefreshRequired(document, two, one, {}));
    assert(sourceRefreshRequired(document, one, reused, {}));
    auto reversed = two;
    std::reverse(reversed.begin(), reversed.end());
    assert(!sourceRefreshRequired(document, two, reversed, {}));
    document.devices[0].bindings[0].selector = "canonical";
    assert(sourceRefreshRequired(document, one, two, {}));
    document.devices[0].bindings[0].selector = "saved-app";
    document.devices[0].routing.sources[0].muted = true;
    assert(sourceRefreshRequired(document, absent, present, missing));
    assert(sourceRefreshRequired(document, one, reused, {}));
    document.devices[0].bindings[0].muteOriginal = false;
    assert(!sourceRefreshRequired(document, absent, present, missing));
    document.devices[0].bindings[0].muteOriginal = true;
    document.devices[0].routing.sources[0].muted = false;
    document.devices[0].routing.sources[0].enabled = false;
    assert(!sourceRefreshRequired(document, absent, present, missing));
    document.devices[0].routing.sources[0].enabled = true;
    document.devices[0].routing.enabled = false;
    assert(!sourceRefreshRequired(document, absent, present, missing));
    document.devices[0].routing.enabled = true;
    document.devices[0].bindings[0].kind = SourceKind::InputDevice;
    assert(sourceRefreshRequired(document, absent, absent, missing));
    assert(!sourceRefreshRequired(document, absent, unrelated, {}));
    std::cout << "Bound source changes and stable unavailable applications passed\n";
}
