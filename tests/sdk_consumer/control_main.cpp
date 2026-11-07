#include <loopz/runtime/control.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
class Song final : public loopz::AudioSource {
  public:
    mutable std::atomic<unsigned> reads{0};
    uint32_t channels() const noexcept override {
        return 2;
    }
    loopz::AudioReadResult read(uint64_t, double, loopz::RoutingOutput output, uint32_t frames,
                                loopz::AudioReadCursor&) const noexcept override {
        ++reads;
        for (auto* plane : output.channels)
            if (plane)
                std::fill_n(plane, std::min(frames, output.capacityFrames), 0.f);
        return {0, true};
    }
};
class Host final : public singz::AudioHostBackend {
    std::shared_ptr<unsigned> enumerations_;

  public:
    explicit Host(std::shared_ptr<unsigned> enumerations)
        : enumerations_(std::move(enumerations)) {}
    singz::AudioHostInventory enumerate() const override {
        ++*enumerations_;
        return {};
    }
    singz::AudioHostResult open(const singz::AudioHostConfig&, singz::AudioHostRender,
                                void*) override {
        throw std::runtime_error("Unexpected physical output");
    }
    singz::AudioHostResult start() override {
        throw std::runtime_error("Unexpected physical start");
    }
    void stop() noexcept override {}
    singz::AudioHostStatus status() const noexcept override {
        return {};
    }
};
loopz::JSON state(loopz::runtime::Control& control) {
    return control.request({{"version", 1}, {"operation", "state"}}).at("snapshot");
}
} // namespace
int run(int argc, char** argv) {
    require(argc == 2 || (argc == 3 && std::string_view(argv[2]) == "--unbound-negative-control"),
            "Pass a scratch state path and optional negative control");
    const std::filesystem::path path(argv[1]);
    std::filesystem::remove(path);
    try {
        loopz::runtime::Control invalid(path, nullptr);
        throw std::runtime_error("Null runtime accepted");
    } catch (const std::invalid_argument&) {
    }
    const std::array<loopz::edit::Command, 2> commands{
        loopz::edit::CreateDevice{"mix", "SDK consumer", 2, 48000, false},
        loopz::edit::AddSource{"mix", "song", loopz::SourceKind::Application, "native:singz"}};
    auto document = loopz::edit::applyCommands({}, commands);
    {
        std::ofstream saved(path);
        saved << loopz::JSON{
            {"version", 1}, {"generation", "7"}, {"document", loopz::encodeDocument(document)}};
    }
    auto enumerations = std::make_shared<unsigned>(0);
    auto song = std::make_shared<Song>();
    auto runtime = [&] {
        auto audio = std::make_unique<loopz::runtime::AudioRuntime>(
            [=] {
                return std::make_unique<Host>(enumerations);
            },
            []() -> std::unique_ptr<singz::AudioInputBackend> {
                throw std::runtime_error("Unexpected physical input");
            });
        if (argc == 2)
            audio->bindNativeSource("native:singz", song);
        return audio;
    };
    {
        loopz::runtime::Control control(path, runtime());
        auto snapshot = state(control);
        require(snapshot["restoration"]["activeGeneration"] == "7", "Saved graph not activated");
        for (unsigned attempt = 0; attempt < 100 && !song->reads.load(); ++attempt) {
            control.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        require(song->reads.load() > 0, "Restored graph did not read prebound native source");
        auto result = control.request(
            {{"version", 1},
             {"operation", "edit"},
             {"request",
              {{"version", 1},
               {"expected", snapshot["revision"]},
               {"commands", loopz::JSON::array({{{"type", "renameDevice"},
                                                 {"device", "mix"},
                                                 {"name", "Saved by Control"}}})}}}});
        require(result["ok"] == true && result["revision"]["generation"] == "8",
                "Edit not acknowledged");
    }
    {
        loopz::runtime::Control control(path, runtime());
        control.poll();
        auto snapshot = state(control);
        require(snapshot["revision"]["generation"] == "8", "Control revision not restored");
        require(snapshot["restoration"]["activeGeneration"] == "8", "Edited graph not reactivated");
        require(snapshot["document"]["devices"][0]["name"] == "Saved by Control",
                "Edit not persisted");
    }
    require(*enumerations > 0, "Control bypassed injected inventory factory");
    std::filesystem::remove(path);
    std::puts(
        "PASS library-only Control owns injected runtime and restores native sources and edits");
    return 0;
}
int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
