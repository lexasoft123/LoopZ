#include <loopz/runtime/control.hpp>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>
#include <unistd.h>

namespace {
thread_local bool watchHeap = false;
thread_local unsigned heapEvents = 0;
} // namespace
#if defined(__GNUC__)
#define HEAP_HOOK __attribute__((noinline))
#else
#define HEAP_HOOK
#endif
HEAP_HOOK void* operator new(size_t size) {
    if (watchHeap)
        ++heapEvents;
    if (auto* result = std::malloc(size ? size : 1))
        return result;
    throw std::bad_alloc();
}
HEAP_HOOK void* operator new[](size_t size) {
    return ::operator new(size);
}
HEAP_HOOK void operator delete(void* value) noexcept {
    if (watchHeap && value)
        ++heapEvents;
    std::free(value);
}
HEAP_HOOK void operator delete[](void* value) noexcept {
    ::operator delete(value);
}
HEAP_HOOK void operator delete(void* value, size_t) noexcept {
    ::operator delete(value);
}
HEAP_HOOK void operator delete[](void* value, size_t) noexcept {
    ::operator delete(value);
}
#undef HEAP_HOOK

namespace {
using namespace loopz;
class UnavailableHost final : public singz::AudioHostBackend {
  public:
    singz::AudioHostInventory enumerate() const override {
        singz::AudioHostDeviceInfo device;
        device.uid = "unavailable";
        device.outputChannels = 2;
        device.nominalSampleRate = 48000;
        device.direction = singz::AudioHostEndpointDirection::Output;
        return {{device}, {}, device.uid};
    }
    singz::AudioHostResult open(const singz::AudioHostConfig&, singz::AudioHostRender,
                                void*) override {
        throw std::runtime_error("Fixture output is unavailable");
    }
    singz::AudioHostResult start() override {
        throw std::runtime_error("No physical start");
    }
    void stop() noexcept override {}
    singz::AudioHostStatus status() const noexcept override {
        return {};
    }
};
std::unique_ptr<runtime::AudioRuntime> audio() {
    return std::make_unique<runtime::AudioRuntime>([] {
        return std::make_unique<UnavailableHost>();
    });
}
JSON snapshot(runtime::Control& control) {
    return control.request({{"version", 1}, {"operation", "state"}}).at("snapshot");
}
JSON applyEdit(runtime::Control& control, JSON command, bool initialize = false) {
    return control.request({{"version", 1},
                            {"operation", initialize ? "initialize" : "edit"},
                            {"request",
                             {{"version", 1},
                              {"expected", snapshot(control).at("revision")},
                              {"commands", JSON::array({std::move(command)})}}}});
}
void allocationFree(const runtime::Control& control) {
    const auto expected = control.routeView();
    heapEvents = 0;
    watchHeap = true;
    for (unsigned index = 0; index < 100; ++index) {
        const auto view = control.routeView();
        assert(view.desired == expected.desired &&
               view.documentRevision == expected.documentRevision &&
               view.desiredGeneration == expected.desiredGeneration &&
               view.activeGeneration == expected.activeGeneration);
    }
    watchHeap = false;
    assert(heapEvents == 0);
}
} // namespace
int main() {
    char pattern[] = "/tmp/loopz-control-view-XXXXXX";
    const auto* directory = ::mkdtemp(pattern);
    assert(directory);
    const auto path = std::filesystem::path(directory) / "state.json";
    {
        runtime::Control control(path, audio());
        const auto empty = control.routeView();
        assert(!empty.desired && empty.desiredGeneration == 0 && empty.activeGeneration == 0);
        allocationFree(control);
        assert(applyEdit(control,
                         {{"type", "createDevice"},
                          {"id", "mix"},
                          {"name", "First"},
                          {"channels", 2},
                          {"sampleRate", 48000},
                          {"passThrough", false}},
                         true)
                   .at("ok") == true);
        const auto first = control.routeView();
        assert(first.desired && first.desiredGeneration == 1 && first.activeGeneration == 1);
        const auto initialRevision = first.documentRevision;
        assert(first.desired->devices.front().name == "First");
        allocationFree(control);
        assert(applyEdit(control, {{"type", "renameDevice"}, {"device", "mix"}, {"name", "Second"}})
                   .at("ok") == true);
        const auto second = control.routeView();
        assert(second.desiredGeneration == 2 && second.activeGeneration == 2 &&
               second.documentRevision != initialRevision &&
               second.desired->devices.front().name == "Second");
        assert(control
                   .request({{"version", 1},
                             {"operation", "edit"},
                             {"request",
                              {{"version", 1},
                               {"expected", snapshot(control).at("revision")},
                               {"history", "undo"}}}})
                   .at("ok") == true);
        const auto undone = control.routeView();
        assert(undone.desiredGeneration == 3 && undone.activeGeneration == 3 &&
               undone.documentRevision != second.documentRevision &&
               undone.desired->devices.front().name == "First");
        assert(applyEdit(control, {{"type", "addMonitor"},
                                   {"device", "mix"},
                                   {"id", "missing"},
                                   {"selector", "unavailable"}})
                   .at("ok") == true);
        const auto failed = control.routeView();
        assert(failed.desiredGeneration == 4 && failed.activeGeneration == 3 &&
               failed.documentRevision != undone.documentRevision &&
               failed.desired->devices.front().monitors.size() == 1);
        allocationFree(control);
        control.poll();
        assert(control.routeView().activeGeneration == 3 &&
               control.routeView().documentRevision == failed.documentRevision);
        // Typed observation must not change the public canonical document.
        assert(snapshot(control).at("document") == encodeDocument(*control.routeView().desired));
    }
    {
        runtime::Control restored(path, audio());
        const auto view = restored.routeView();
        const auto canonical = snapshot(restored);
        // With no predecessor, AudioRuntime commits a partial initial scene
        // when an output cannot open. Editing an existing scene above instead
        // retains its predecessor. Active generation describes the committed
        // scene, not a claim that all its endpoints are healthy.
        assert(view.desired && view.desiredGeneration == 4 && view.activeGeneration == 4 &&
               view.documentRevision != 0 && view.desired->devices.front().monitors.size() == 1);
        assert(canonical.at("restoration").at("desiredGeneration") ==
               std::to_string(view.desiredGeneration));
        assert(canonical.at("restoration").at("activeGeneration") ==
               std::to_string(view.activeGeneration));
        assert(canonical.at("restoration").at("phase") == "partial");
        assert(canonical.at("document") == encodeDocument(*view.desired));
        allocationFree(restored);
    }
    std::filesystem::remove_all(directory);
    std::puts("Control borrowed route view generations and allocation regression passed");
}
