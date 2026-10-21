#include <loopz/macos/public_timing_observer.hpp>

#include <atomic>
#include <array>
#include <cassert>
#include <chrono>
#include <semaphore>
#include <thread>
#include <stdexcept>

namespace {
using namespace loopz::macos;
const std::string a = "com.lexasoft.loopz.device.a", b = "com.lexasoft.loopz.device.b";
struct Gate {
    std::binary_semaphore entered{0}, release{0}, returned{0};
    std::atomic<unsigned> reads{0};
    std::atomic<uint32_t> period{64};
    std::atomic<bool> fail{false};
};
PublicTimingCatalog::Operations operations(const std::shared_ptr<Gate>& gate) {
    return {[](const auto& uid) {
                return uid == a ? 1U : 2U;
            },
            [](auto) {
                return true;
            },
            [gate](auto, bool) {
                const auto count = gate->reads.fetch_add(1);
                const auto period = gate->period.load();
                if (!count) {
                    gate->entered.release();
                    gate->release.acquire();
                }
                if (gate->fail.load())
                    throw std::runtime_error("Read failed");
                if (!count)
                    gate->returned.release();
                return PublicDeviceTiming{period, true, 1, true};
            }};
}
std::shared_ptr<const PublicTimingObservationBatch> complete(PublicTimingObserver& observer,
                                                             std::span<const std::string> uids,
                                                             uint64_t now, uint64_t revision) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        auto batch = observer.snapshot(uids, now);
        if (batch && batch->revision >= revision)
            return batch;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("Fake timing batch did not complete");
}
void blockedReadsAndGenerationIsolation() {
    const auto gate = std::make_shared<Gate>();
    PublicTimingObserver observer(operations(gate));
    const std::array uids{a};
    assert(!observer.snapshot(uids, 0));
    assert(gate->entered.try_acquire_for(std::chrono::seconds(2)));
    for (unsigned attempt = 0; attempt < 128; ++attempt)
        assert(!observer.snapshot(uids, 0));
    assert(gate->reads == 1);
    observer.invalidate();
    gate->period = 512;
    gate->release.release();
    const auto first = complete(observer, uids, 0, 1);
    assert(gate->reads == 2 && first->devices.at(a).value->bufferFrames == 512);
    assert(observer.snapshot(uids, 99999999) == first && gate->reads == 2);
    gate->fail = true;
    (void)observer.snapshot(uids, 100000000);
    const auto failed = complete(observer, uids, 100000000, 2);
    assert(!failed->devices.at(a).value && failed->devices.at(a).error == "Read failed");
    gate->fail = false;
    gate->period = 256;
    (void)observer.snapshot(uids, 200000000);
    const auto recovered = complete(observer, uids, 200000000, 3);
    assert(recovered->devices.at(a).value->bufferFrames == 256);
    assert(first->devices.at(a).value->bufferFrames == 512); // Published batches stay immutable.
}
void changingTargetsAndSafeShutdown() {
    const auto gate = std::make_shared<Gate>();
    PublicTimingObserver observer(operations(gate));
    const std::array one{a};
    const std::array two{a, b};
    assert(!observer.snapshot(one, 0));
    assert(gate->entered.try_acquire_for(std::chrono::seconds(2)));
    assert(!observer.snapshot(two, 0) && gate->reads == 1);
    gate->release.release();
    const auto batch = complete(observer, two, 0, 1);
    assert(batch->devices.size() == 2 && gate->reads == 3);
    assert(!observer.snapshot({}, 0));

    const auto closing = std::make_shared<Gate>();
    {
        PublicTimingObserver transient(operations(closing));
        (void)transient.snapshot(one, 0);
        assert(closing->entered.try_acquire_for(std::chrono::seconds(2)));
    } // A blocked native call must not retain or join the destroyed observer.
    closing->release.release();
    assert(closing->returned.try_acquire_for(std::chrono::seconds(2)));
}
void invalidScopeNeverStartsNativeReads() {
    const auto gate = std::make_shared<Gate>();
    PublicTimingObserver observer(operations(gate));
    const auto rejects = [&](const auto& uids) {
        bool failed = false;
        try {
            (void)observer.snapshot(uids, 0);
        } catch (const std::invalid_argument&) {
            failed = true;
        }
        assert(failed && gate->reads == 0);
    };
    rejects(std::array{a, std::string("BuiltInSpeakerDevice")});
    std::vector<std::string> excessive;
    for (unsigned index = 0; index < 513; ++index)
        excessive.push_back(a + std::to_string(index));
    rejects(excessive);
}
} // namespace
int main() {
    blockedReadsAndGenerationIsolation();
    changingTargetsAndSafeShutdown();
    invalidScopeNeverStartsNativeReads();
}
