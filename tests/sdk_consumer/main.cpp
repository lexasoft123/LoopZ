#include <loopz/runtime/audio_runtime.hpp>
#include <loopz/route_edits.hpp>
#include <algorithm>
#include <cstdio>
#include <stdexcept>

class Song final : public loopz::AudioSource {
  public:
    uint32_t channels() const noexcept override {
        return 2;
    }
    loopz::AudioReadResult read(uint64_t, double, loopz::RoutingOutput output, uint32_t frames,
                                loopz::AudioReadCursor&) const noexcept override {
        for (auto* plane : output.channels)
            if (plane)
                std::fill_n(plane, std::min(frames, output.capacityFrames), 0.f);
        return {0, true};
    }
};
int main() {
    // Neither factory is permitted to open physical audio in this consumer test.
    loopz::runtime::AudioRuntime runtime(
        []() -> std::unique_ptr<singz::AudioHostBackend> {
            throw std::runtime_error("Unexpected hardware output");
        },
        []() -> std::unique_ptr<singz::AudioInputBackend> {
            throw std::runtime_error("Unexpected hardware input");
        });
    runtime.bindNativeSource("native:song", std::make_shared<Song>());
    const std::array<loopz::edit::Command, 2> commands{
        loopz::edit::CreateDevice{"mix", "SDK consumer", 2, 48000, false},
        loopz::edit::AddSource{"mix", "song", loopz::SourceKind::Application, "native:song"}};
    auto document = loopz::edit::applyCommands({}, commands);
    runtime.commit(runtime.prepare(document));
    if (!runtime.status().active || !runtime.unbindNativeSource("native:song"))
        return 1;
    runtime.commit(runtime.prepare(document));
    runtime.poll();
    std::puts("PASS pinned shared-zCore LoopZ Runtime native source consumer");
}
