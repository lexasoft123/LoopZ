// Control owner robustness: poll activation retry throttling, invalid UTF-8 in
// host strings, stale replacement sweep and inter-process state locking.
#include <loopz/runtime/control.hpp>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
using namespace loopz;
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
struct Fixture {
    std::atomic<unsigned> opens{0};
    std::string label{"Output"};
    std::vector<std::string> channels{"Left", "Right"};
    std::string failure{"Fixture output is unavailable"};
};
class Host final : public singz::AudioHostBackend {
    std::shared_ptr<Fixture> fixture_;

  public:
    explicit Host(std::shared_ptr<Fixture> fixture) : fixture_(std::move(fixture)) {}
    singz::AudioHostInventory enumerate() const override {
        singz::AudioHostDeviceInfo device;
        device.uid = "unavailable";
        device.label = fixture_->label;
        device.outputChannels = 2;
        device.outputChannelLabels = fixture_->channels;
        device.nominalSampleRate = 48000;
        device.direction = singz::AudioHostEndpointDirection::Output;
        return {{device}, {}, device.uid};
    }
    singz::AudioHostResult open(const singz::AudioHostConfig&, singz::AudioHostRender,
                                void*) override {
        ++fixture_->opens;
        throw std::runtime_error(fixture_->failure);
    }
    singz::AudioHostResult start() override {
        throw std::runtime_error("No physical start");
    }
    void stop() noexcept override {}
    singz::AudioHostStatus status() const noexcept override {
        return {};
    }
};
std::unique_ptr<runtime::AudioRuntime> audio(std::shared_ptr<Fixture> fixture) {
    return std::make_unique<runtime::AudioRuntime>([fixture] {
        return std::make_unique<Host>(fixture);
    });
}
JSON state(runtime::Control& control) {
    return control.request({{"version", 1}, {"operation", "state"}});
}
JSON applyEdit(runtime::Control& control, JSON command, bool initialize = false) {
    return control.request({{"version", 1},
                            {"operation", initialize ? "initialize" : "edit"},
                            {"request",
                             {{"version", 1},
                              {"expected", state(control).at("snapshot").at("revision")},
                              {"commands", JSON::array({std::move(command)})}}}});
}
JSON createBus(runtime::Control& control) {
    return applyEdit(control,
                     {{"type", "createDevice"},
                      {"id", "mix"},
                      {"name", "Mix"},
                      {"channels", 2},
                      {"sampleRate", 48000},
                      {"passThrough", false}},
                     true);
}
JSON addUnavailableMonitor(runtime::Control& control) {
    return applyEdit(control, {{"type", "addMonitor"},
                               {"device", "mix"},
                               {"id", "speakers"},
                               {"selector", "unavailable"}});
}
std::filesystem::path scratch(const char* name) {
    std::string pattern = std::string("/tmp/loopz-control-") + name + "-XXXXXX";
    require(::mkdtemp(pattern.data()) != nullptr, "mkdtemp failed");
    return pattern;
}
std::string contents(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
// The negative-control build compiles against the pre-fix API.
bool owns(runtime::Control& control) {
#ifdef LOOPZ_NEGATIVE_CONTROL_OLD_API
    return state(control).at("connection").at("ready") == true;
#else
    return control.ownsState();
#endif
}
void touch(const std::filesystem::path& path) {
    std::ofstream(path) << "partial";
}

void throttlesAutomaticRetries() {
    const auto directory = scratch("throttle");
    const auto path = directory / "state.json";
    auto fixture = std::make_shared<Fixture>();
    {
        // Real clock: many fast polls after a failed activation reopen nothing.
        runtime::Control control(path, audio(fixture));
        require(createBus(control).at("ok") == true, "initial bus not created");
        require(addUnavailableMonitor(control).at("ok") == true, "pending edit not accepted");
        const auto view = control.routeView();
        require(view.desiredGeneration == 2 && view.activeGeneration == 1,
                "unavailable monitor unexpectedly activated");
        const auto afterEdit = fixture->opens.load();
        require(afterEdit >= 1, "edit did not attempt to open the output");
        for (int poll = 0; poll < 40; ++poll)
            control.poll();
        require(fixture->opens.load() == afterEdit,
                "poll retried activation on every call instead of throttling");
        // Explicit retry is never throttled.
        require(control.request({{"version", 1}, {"operation", "retry"}}).at("ok") == true,
                "retry rejected");
        require(fixture->opens.load() > afterEdit, "explicit retry was throttled");
    }
#ifndef LOOPZ_NEGATIVE_CONTROL_OLD_API
    {
        // Injected clock: retries happen exactly once per interval.
        std::filesystem::remove(path);
        auto now = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
        runtime::Control control(path, audio(fixture), [&now] {
            return now;
        });
        require(createBus(control).at("ok") == true, "initial bus not created");
        require(addUnavailableMonitor(control).at("ok") == true, "pending edit not accepted");
        auto attempts = fixture->opens.load();
        for (int second = 0; second < 10; ++second) {
            // 25 ms polling up to 1975 ms after the last attempt: no retry.
            for (int poll = 0; poll < 80; ++poll) {
                control.poll();
                now += std::chrono::milliseconds(25);
            }
            require(fixture->opens.load() == attempts, "retried before the 2 s interval");
            control.poll(); // exactly 2 s after the last attempt
            require(fixture->opens.load() > attempts, "did not retry after the 2 s interval");
            attempts = fixture->opens.load();
        }
        require(control.routeView().activeGeneration == 1, "unavailable output activated");
    }
#endif
    std::filesystem::remove_all(directory);
}

void sanitizesHostStrings() {
    const auto directory = scratch("utf8");
    const auto path = directory / "state.json";
    auto fixture = std::make_shared<Fixture>();
    fixture->label = "Bad \xff\xfe label \xc3\x28";
    fixture->channels = {"\xed\xa0\x80", "ok \xe2\x82"};
    fixture->failure = "Device \xf5 failed";
    runtime::Control control(path, audio(fixture));
    require(createBus(control).at("ok") == true, "initial bus not created");
    auto edit = addUnavailableMonitor(control);
    auto snapshot = state(control);
    std::string bytes;
    try {
        bytes = snapshot.dump();
        bytes += edit.dump();
    } catch (const JSON::type_error& error) {
        std::fprintf(stderr, "FAIL: snapshot not dumpable: %s\n", error.what());
        std::exit(1);
    }
    const auto& device = snapshot.at("inventory").at("devices").at(0);
    require(device.at("name") == "Bad \xef\xbf\xbd\xef\xbf\xbd label \xef\xbf\xbd(",
            "label not sanitized to U+FFFD");
    require(device.at("outputChannelNames").at(0) == "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd",
            "surrogate encoding not rejected");
    require(device.at("outputChannelNames").at(1) == "ok \xef\xbf\xbd\xef\xbf\xbd",
            "truncated sequence not replaced");
    require(bytes.find("Device \xef\xbf\xbd failed") != std::string::npos,
            "activation error not sanitized");
    std::filesystem::remove_all(directory);
}

void sweepsStaleReplacements() {
    const auto directory = scratch("sweep");
    const auto path = directory / "state.json";
    const auto removedLegacy = directory / ".loopz-state-a1B2c3";
    const auto removedNamed = directory / ".loopz-state-state.json.Xy12Ab";
    const auto keptOther = directory / ".loopz-state-other.json.Ab12Cd";
    const auto keptLong = directory / ".loopz-state-abcdefg";
    const auto keptUnrelated = directory / "unrelated.txt";
    const auto keptDirectory = directory / ".loopz-state-dir123";
    const auto target = directory / "target";
    const auto keptLink = directory / ".loopz-state-sym123";
    for (const auto& file :
         {removedLegacy, removedNamed, keptOther, keptLong, keptUnrelated, target})
        touch(file);
    std::filesystem::create_directory(keptDirectory);
    std::filesystem::create_symlink(target, keptLink);
    {
        auto fixture = std::make_shared<Fixture>();
        runtime::Control control(path, audio(fixture));
        require(owns(control), "state lock not acquired");
        require(!std::filesystem::exists(removedLegacy), "legacy stale replacement remains");
        require(!std::filesystem::exists(removedNamed), "stale replacement remains");
        for (const auto& kept : {keptOther, keptLong, keptUnrelated, keptDirectory, target})
            require(std::filesystem::exists(kept), "sweep removed an unrelated entry");
        require(std::filesystem::is_symlink(keptLink), "sweep removed or followed a symlink");
        require(createBus(control).at("ok") == true, "edit after sweep failed");
        // A second owner must not sweep while the first holds the lock.
        const auto inFlight = directory / ".loopz-state-state.json.InFlit";
        touch(inFlight);
        runtime::Control second(path, audio(fixture));
        require(!owns(second), "second owner acquired the held lock");
        require(std::filesystem::exists(inFlight), "unlocked owner swept in-flight replacement");
        std::filesystem::remove(inFlight);
    }
    std::filesystem::remove_all(directory);
}

void locksSavedState() {
    const auto directory = scratch("lock");
    const auto path = directory / "state.json";
    auto fixture = std::make_shared<Fixture>();
    auto first = std::make_unique<runtime::Control>(path, audio(fixture));
    require(createBus(*first).at("ok") == true, "first owner cannot edit");
    const auto saved = contents(path);
    require(!saved.empty(), "first owner did not persist");
    {
        runtime::Control second(path, audio(fixture));
        auto snapshot = state(second);
        require(snapshot.at("connection").at("ready") == false, "second owner reports ready");
        require(snapshot.at("snapshot").at("document").is_null(),
                "second owner read the locked state");
        require(snapshot.at("snapshot").at("canCreateDocument") == false,
                "second owner offers document creation");
        require(snapshot.at("snapshot")
                        .at("restoration")
                        .at("error")
                        .get<std::string>()
                        .find("in use") != std::string::npos,
                "lock contention error is unclear");
        auto edit = createBus(second);
        require(edit.at("ok") == false && edit.at("code") == "storage-unavailable",
                "second owner edit not rejected as storage-unavailable");
        auto retry = second.request({{"version", 1}, {"operation", "retry"}});
        require(retry.at("ok") == false && retry.at("code") == "storage-unavailable",
                "retry succeeded while the lock is held");
        require(contents(path) == saved, "second owner touched the state file");
        first.reset();
        retry = second.request({{"version", 1}, {"operation", "retry"}});
        require(retry.at("ok") == true, "retry after release did not acquire the lock");
        snapshot = state(second);
        require(snapshot.at("connection").at("ready") == true, "not ready after acquiring lock");
        require(snapshot.at("snapshot").at("document").at("devices").at(0).at("id") == "mix",
                "saved routing not loaded after acquiring lock");
        require(applyEdit(second, {{"type", "renameDevice"}, {"device", "mix"}, {"name", "Two"}})
                        .at("ok") == true,
                "edit after acquiring lock failed");
    }
    {
        runtime::Control third(path, audio(fixture));
        require(owns(third), "lock not released on destruction");
        require(state(third).at("snapshot").at("document").at("devices").at(0).at("name") == "Two",
                "edit by late owner not persisted");
    }
    std::filesystem::remove_all(directory);
}
} // namespace

int main(int argc, char** argv) {
    // An optional case name runs one case (used for negative controls).
    const std::string only = argc > 1 ? argv[1] : "";
    if (only.empty() || only == "throttle")
        throttlesAutomaticRetries();
    if (only.empty() || only == "utf8")
        sanitizesHostStrings();
    if (only.empty() || only == "sweep")
        sweepsStaleReplacements();
    if (only.empty() || only == "lock")
        locksSavedState();
    std::puts("Control retry throttling, UTF-8, temporary sweep and state lock passed");
}
