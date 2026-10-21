#include <loopz/realtime_publication.hpp>
#include <array>
#include <cassert>
#include <cstdio>
#include <optional>
#include <thread>

namespace {
struct Tracked {
    unsigned id;
    unsigned& destroyed;
    std::thread::id owner{std::this_thread::get_id()};
    std::array<unsigned, 32> payload;
    Tracked(unsigned value, unsigned& count) : id(value), destroyed(count) {
        payload.fill(id);
    }
    ~Tracked() {
        assert(std::this_thread::get_id() == owner);
        ++destroyed;
    }
};
void continuouslyOverlappingReadersCanRetireOldVersions() {
    unsigned destroyed = 0;
    loopz::RealtimePublication<Tracked> publication(1);
    publication.stage(std::make_unique<Tracked>(0, destroyed))->commit();
    std::optional<loopz::RealtimePublication<Tracked>::Borrow> held(publication.borrow());
    // There is always at least one live borrower. A current-version borrower
    // must not prevent an unreferenced older version from being reclaimed.
    for (unsigned id = 1; id <= 500; ++id) {
        publication.stage(std::make_unique<Tracked>(id, destroyed))->commit();
        auto next = publication.borrow();
        assert(next && next->id == id && (*held)->id == id - 1);
        held.reset();
        publication.collect();
        assert(destroyed == id);
        held.emplace(std::move(next));
    }
}
void actuallyPinnedVersionsStillApplyBoundedBackpressure() {
    unsigned destroyed = 0;
    loopz::RealtimePublication<Tracked> publication(1);
    publication.stage(std::make_unique<Tracked>(1, destroyed))->commit();
    {
        auto old = publication.borrow();
        publication.stage(std::make_unique<Tracked>(2, destroyed))->commit();
        auto current = publication.borrow();
        bool blocked = false;
        try {
            publication.checkStageCapacity();
        } catch (const loopz::RealtimePublicationBusy&) {
            blocked = true;
        }
        assert(blocked && old->id == 1 && current->id == 2 && destroyed == 0);
    }
    publication.collect();
    assert(destroyed == 1);
    publication.stage(nullptr)->commit();
    assert(!publication.borrow());
    publication.collect();
    assert(destroyed == 2);
}
void concurrentReadersKeepCoherentLiveNodes() {
    unsigned destroyed = 0;
    loopz::RealtimePublication<Tracked> publication(2);
    publication.stage(std::make_unique<Tracked>(0, destroyed))->commit();
    std::atomic<bool> stop{false};
    std::atomic<unsigned> started{0};
    const auto read = [&] {
        started.fetch_add(1);
        while (!stop.load()) {
            auto value = publication.borrow();
            assert(value);
            for (const auto sample : value->payload)
                assert(sample == value->id);
        }
    };
    std::thread first(read), second(read);
    while (started.load() != 2)
        std::this_thread::yield();
    for (unsigned id = 1; id <= 1000; ++id) {
        for (;;) {
            try {
                publication.checkStageCapacity();
                publication.stage(std::make_unique<Tracked>(id, destroyed))->commit();
                break;
            } catch (const loopz::RealtimePublicationBusy&) {
                std::this_thread::yield();
            }
        }
    }
    stop.store(true);
    first.join();
    second.join();
    publication.collect();
    assert(destroyed == 1000 && publication.borrow()->id == 1000);
}
} // namespace
int main() {
    try {
        continuouslyOverlappingReadersCanRetireOldVersions();
        actuallyPinnedVersionsStillApplyBoundedBackpressure();
        concurrentReadersKeepCoherentLiveNodes();
        std::puts("Overlapping readers reclaim only unpinned versions; bounded backpressure and "
                  "control-only destruction pass");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Publication regression: %s\n", error.what());
        return 1;
    }
}
