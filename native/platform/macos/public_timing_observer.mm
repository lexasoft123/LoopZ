#include <loopz/macos/public_timing_observer.hpp>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace loopz::macos {
struct PublicTimingObserver::Impl {
    struct Completion {
        uint64_t generation;
        std::shared_ptr<PublicTimingObservationBatch> batch;
        std::exception_ptr error;
    };
    struct State {
        std::mutex mutex;
        uint64_t generation{0};
        bool running{false};
        std::optional<Completion> completed;
        // Only the single worker accesses this catalog or its generation.
        PublicTimingCatalog catalog;
        uint64_t catalogGeneration{UINT64_MAX};
        explicit State(PublicTimingCatalog::Operations operations)
            : catalog(std::move(operations)) {}
    };
    const std::shared_ptr<State> state;
    std::vector<std::string> targets;
    std::shared_ptr<const PublicTimingObservationBatch> cached;
    uint64_t nextRefresh{0}, revision{0};
    explicit Impl(PublicTimingCatalog::Operations operations)
        : state(std::make_shared<State>(std::move(operations))) {}
};

PublicTimingObserver::PublicTimingObserver(PublicTimingCatalog::Operations operations)
    : impl_(std::make_unique<Impl>(std::move(operations))) {}
PublicTimingObserver::~PublicTimingObserver() = default;

std::shared_ptr<const PublicTimingObservationBatch>
PublicTimingObserver::snapshot(std::span<const std::string> uids, uint64_t nowNs) {
    auto& self = *impl_;
    std::vector<std::string> targets(uids.begin(), uids.end());
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    if (targets.size() > 512 || std::any_of(targets.begin(), targets.end(), [](const auto& uid) {
            return !uid.starts_with("com.lexasoft.loopz.device.") || uid.size() > 4096 ||
                   uid.find('\0') != std::string::npos;
        }))
        throw std::invalid_argument("Public timing observation requires bounded LoopZ identities");
    std::optional<Impl::Completion> completed;
    uint64_t generation = 0;
    bool start = false;
    {
        std::lock_guard lock(self.state->mutex);
        if (targets != self.targets) {
            self.targets = std::move(targets);
            ++self.state->generation;
            self.state->completed.reset();
            self.nextRefresh = 0;
        }
        if (self.state->completed) {
            if (self.state->completed->generation == self.state->generation)
                completed = std::move(self.state->completed);
            self.state->completed.reset();
        }
        if (!completed && !self.targets.empty() && nowNs >= self.nextRefresh &&
            !self.state->running) {
            self.state->running = start = true;
            generation = self.state->generation;
        }
    }
    if (completed) {
        self.nextRefresh = nowNs > UINT64_MAX - 100000000 ? UINT64_MAX : nowNs + 100000000;
        if (completed->error)
            std::rethrow_exception(completed->error);
        if (self.revision == UINT64_MAX)
            throw std::overflow_error("Public timing observation revision exhausted");
        completed->batch->revision = ++self.revision;
        self.cached = std::move(completed->batch);
    }
    if (self.targets.empty()) {
        self.cached.reset();
        return {};
    }
    if (start) {
        try {
            std::thread([state = self.state, generation, targets = self.targets] {
                @autoreleasepool {
                    Impl::Completion result{generation, {}, {}};
                    try {
                        if (state->catalogGeneration != generation) {
                            state->catalog.invalidate();
                            state->catalogGeneration = generation;
                        }
                        result.batch = std::make_shared<PublicTimingObservationBatch>();
                        for (const auto& uid : targets) {
                            PublicTimingObservation observation;
                            try {
                                observation.value = state->catalog.probe(uid);
                            } catch (const std::exception& error) {
                                observation.error = std::string(error.what()).substr(0, 512);
                            } catch (...) {
                                observation.error = "Public timing observation failed";
                            }
                            result.batch->devices.emplace(uid, std::move(observation));
                        }
                    } catch (...) {
                        result.error = std::current_exception();
                    }
                    std::lock_guard lock(state->mutex);
                    state->completed = std::move(result);
                    state->running = false;
                }
            }).detach();
        } catch (...) {
            std::lock_guard lock(self.state->mutex);
            self.state->running = false;
            self.state->completed = Impl::Completion{generation, {}, std::current_exception()};
        }
    }
    return self.cached;
}
std::shared_ptr<const PublicTimingObservationBatch> PublicTimingObserver::latest() const noexcept {
    return impl_->cached;
}
void PublicTimingObserver::invalidate() noexcept {
    auto& self = *impl_;
    std::lock_guard lock(self.state->mutex);
    ++self.state->generation;
    self.state->completed.reset();
    self.cached.reset();
    self.nextRefresh = 0;
}
} // namespace loopz::macos
