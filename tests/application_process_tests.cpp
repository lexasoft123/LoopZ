#include <loopz/application_processes.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <functional>
#include <stdexcept>

namespace {
using namespace loopz;
void rejects(const std::function<void()>& operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}
void captureTreeRoots() {
    std::vector<ProcessInstance> selected{{40, 300}, {30, 200}, {20, 100}, {50, 250}};
    std::vector<ProcessTreeNode> nodes{{{20, 100}, 0},  {{25, 150}, 20}, {{30, 200}, 25},
                                       {{40, 300}, 30}, {{50, 250}, 8},  {{8, 50}, 999}};
    assert((selectProcessCaptureRoots(selected, nodes) ==
            std::vector<ProcessInstance>{{20, 100}, {50, 250}}));
    std::reverse(selected.begin(), selected.end());
    assert((selectProcessCaptureRoots(selected, nodes) ==
            std::vector<ProcessInstance>{{20, 100}, {50, 250}}));
    const auto fails = [](const auto& wanted, const auto& ancestry) {
        bool failed = false;
        try {
            (void)selectProcessCaptureRoots(wanted, ancestry);
        } catch (const std::exception&) {
            failed = true;
        }
        assert(failed);
    };
    auto missing = nodes;
    missing.erase(missing.begin() + 1);
    fails(selected, missing);
    auto reused = nodes;
    reused[1].process.generation = 201;
    fails(selected, reused);
    auto duplicate = selected;
    duplicate.push_back(selected[0]);
    fails(duplicate, nodes);
    const std::vector<ProcessInstance> cycle{{20, 100}, {30, 100}};
    const std::vector<ProcessTreeNode> cyclic{{{20, 100}, 30}, {{30, 100}, 20}};
    fails(cycle, cyclic);
    auto stale = selected;
    stale[0].generation += 1;
    fails(stale, nodes);
    assert(selectProcessCaptureRoots({}, nodes).empty());
}
void groupingAndIsolation() {
    const std::vector<ApplicationProcessRoot> roots{{{20, 100}, "browser"}, {{21, 110}, "other"}};
    std::vector<AudioProcessAttribution> audio{
        {8, {20, 100}, "browser", {}, {}, true},
        {3, {30, 120}, "browser.helper", "browser", {}, false},
        {9, {31, 121}, "shared.gpu", {}, ProcessInstance{20, 100}, false},
        {5, {32, 122}, "shared.gpu", {}, ProcessInstance{21, 110}, false},
        {4, {33, 123}, "browser.helper.evil", {}, {}, false},
        {6, {34, 124}, "different-app", {}, ProcessInstance{20, 100}, true},
    };
    assert((selectApplicationProcesses("browser", roots, audio) == std::vector<uint64_t>{3, 8, 9}));
    assert((selectApplicationProcesses("other", roots, audio) == std::vector<uint64_t>{5}));
    auto otherRestarted = roots;
    otherRestarted[1].process.generation = 111;
    assert((selectApplicationProcesses("browser", otherRestarted, audio) ==
            std::vector<uint64_t>{3, 8, 9}));
    // Sharing a helper's name never joins its other owner's capture.
    assert((selectApplicationProcesses("shared.gpu", roots, audio) == std::vector<uint64_t>{5, 9}));
    std::reverse(audio.begin(), audio.end());
    assert((selectApplicationProcesses("browser", roots, audio) == std::vector<uint64_t>{3, 8, 9}));
    audio.erase(std::remove_if(audio.begin(), audio.end(),
                               [](const auto& p) {
                                   return p.object == 9;
                               }),
                audio.end());
    assert((selectApplicationProcesses("browser", roots, audio) == std::vector<uint64_t>{3, 8}));
}
void instancesAndUnavailableOwnership() {
    const ApplicationProcessRoot root{{20, 100}, "browser"};
    AudioProcessAttribution helper{7, {30, 120}, "shared.gpu", {}, ProcessInstance{20, 99}, false};
    assert(selectApplicationProcesses("browser", {&root, 1}, {&helper, 1}).empty());
    helper.responsible = ProcessInstance{20, 100};
    assert(selectApplicationProcesses("browser", {&root, 1}, {&helper, 1}).size() == 1);
    auto restarted = root;
    restarted.process.generation = 101;
    assert(selectApplicationProcesses("browser", {&restarted, 1}, {&helper, 1}).empty());
    helper.responsible.reset(); // Optional platform ownership is unavailable.
    assert(selectApplicationProcesses("browser", {&root, 1}, {&helper, 1}).empty());
    helper.containingApplication = "browser";
    assert(selectApplicationProcesses("browser", {&root, 1}, {&helper, 1}).size() == 1);
    helper.containingApplication = "browser-beta";
    assert(selectApplicationProcesses("browser", {&root, 1}, {&helper, 1}).empty());
    helper.application = "browser";
    assert(selectApplicationProcesses("browser", {}, {&helper, 1}).size() == 1);
}
void malformedSnapshots() {
    const ApplicationProcessRoot root{{20, 100}, "browser"};
    const std::array<ApplicationProcessRoot, 2> duplicate{root, root};
    rejects([&] {
        (void)selectApplicationProcesses("browser", duplicate, {});
    });
    AudioProcessAttribution helper{7, {30, 120}, "browser", {}, {}, false};
    const std::array<AudioProcessAttribution, 2> repeated{helper, helper};
    rejects([&] {
        (void)selectApplicationProcesses("browser", {}, repeated);
    });
    helper.process.generation = 0;
    rejects([&] {
        (void)selectApplicationProcesses("browser", {}, {&helper, 1});
    });
    rejects([&] {
        (void)selectApplicationProcesses(std::string("browser\0other", 13), {}, {});
    });
    rejects([&] {
        (void)selectApplicationProcesses(std::string(4097, 'x'), {}, {});
    });
    const std::vector<AudioProcessAttribution> excessive(4097);
    rejects([&] {
        (void)selectApplicationProcesses("browser", {}, excessive);
    });
}
} // namespace
int main() {
    captureTreeRoots();
    groupingAndIsolation();
    instancesAndUnavailableOwnership();
    malformedSnapshots();
}
