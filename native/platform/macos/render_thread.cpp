#include <loopz/macos/render_thread.hpp>
#include <loopz/macos/capture.hpp>
#include <loopz/timeline.hpp>
#include <AudioToolbox/AudioWorkInterval.h>
#include <mach/mach_time.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>

#include <algorithm>
#include <thread>

namespace loopz::macos {
namespace {
// The graph has its own cadence rather than being a worker of one hardware
// callback. Apple requires its realtime thread to own an audio work interval.
// Creation/release belongs to control; joining/leaving belongs to this thread.
class WorkgroupMembership {
  public:
    explicit WorkgroupMembership(os_workgroup_interval_t group) : group_(group) {
        if (group_)
            error = os_workgroup_join(group_, &token_);
        joined = group_ && !error;
    }
    ~WorkgroupMembership() {
        if (joined)
            os_workgroup_leave(group_, &token_);
    }
    bool joined{false};
    int error{0};

  private:
    os_workgroup_interval_t group_;
    os_workgroup_join_token_s token_{};
};
} // namespace
struct RenderThread::Impl {
    SceneRenderer& renderer;
    const RenderScheduleTiming timing;
    std::jthread thread;
    std::atomic<bool> running{false}, realtime{false};
    std::atomic<int32_t> error{0};
    std::atomic<uint64_t> blocks{0}, frames{0}, late{0}, skipped{0}, reanchors{0};
    std::atomic<uint64_t> graphFailures{0}, sinkFailures{0}, execution{0}, lastBlock{0};
    std::atomic<uint64_t> publishedFrame{0};
    os_workgroup_interval_t workgroup{nullptr};
    std::atomic<bool> workgroupJoined{false};
    std::atomic<int32_t> workgroupError{0};
    std::atomic<uint64_t> deadlineMisses{0}, maximumLateness{0};
    Impl(SceneRenderer& graph, RenderScheduleTiming clock) : renderer(graph), timing(clock) {
        (void)RenderSchedule(clock);
        workgroup =
            AudioWorkIntervalCreate("LoopZ graph renderer", OS_CLOCK_MACH_ABSOLUTE_TIME, nullptr);
    }
    ~Impl() {
        if (workgroup)
            os_release(workgroup);
    }
    void run(std::stop_token stop) noexcept {
        pthread_setname_np("LoopZ graph renderer");
        const auto periodNs = frameNs(timing.blockFrames, timing.rate);
        thread_time_constraint_policy_data_t policy{
            static_cast<uint32_t>(nsToTicks(periodNs)),
            static_cast<uint32_t>(nsToTicks(std::min<uint64_t>(200000, periodNs / 4))),
            static_cast<uint32_t>(nsToTicks(periodNs * 3 / 4)), true};
        realtime.store(thread_policy_set(pthread_mach_thread_np(pthread_self()),
                                         THREAD_TIME_CONSTRAINT_POLICY,
                                         reinterpret_cast<thread_policy_t>(&policy),
                                         THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS);
        WorkgroupMembership membership(workgroup);
        workgroupJoined.store(membership.joined);
        workgroupError.store(membership.error);
        RenderSchedule schedule(timing);
        running.store(true);
        while (!stop.stop_requested()) {
            const auto now = hostNowNs();
            const auto tick = schedule.step(now);
            if (!tick.valid) {
                error.store(-1);
                break;
            }
            late.fetch_add(tick.lateFrames);
            skipped.fetch_add(tick.skippedFrames);
            if (tick.reanchored)
                reanchors.fetch_add(1);
            for (uint32_t block = 0; block < tick.blocks; ++block) {
                if (stop.stop_requested())
                    break;
                const auto begun = hostNowNs();
                const auto first =
                    tick.firstFrame + static_cast<uint64_t>(block) * timing.blockFrames;
                const auto deadlineNs = frameNs(first, timing.rate);
                bool interval = false;
                if (membership.joined && deadlineNs > begun) {
                    const auto status = os_workgroup_interval_start(workgroup, nsToTicks(begun),
                                                                    nsToTicks(deadlineNs), nullptr);
                    interval = !status;
                    if (status)
                        workgroupError.store(status);
                }
                const auto result = renderer.render(first, timing.blockFrames, begun);
                const auto finished = hostNowNs();
                if (interval) {
                    const auto status = os_workgroup_interval_finish(workgroup, nullptr);
                    if (status)
                        workgroupError.store(status);
                }
                // An empty publication is normal while waiting for the first
                // authenticated native commit. It must not reset that session.
                if (result.validBlock) {
                    blocks.fetch_add(1);
                    frames.fetch_add(timing.blockFrames);
                    graphFailures.fetch_add(result.failedGraphs);
                    sinkFailures.fetch_add(result.failedSinks);
                    lastBlock.store(begun);
                    publishedFrame.store(first + timing.blockFrames);
                    execution.store(std::max(execution.load(), finished - begun));
                    if (finished > deadlineNs) {
                        deadlineMisses.fetch_add(1);
                        maximumLateness.store(
                            std::max(maximumLateness.load(), finished - deadlineNs));
                    }
                }
            }
            const auto deadline = nsToTicks(tick.nextWakeNs);
            while (!stop.stop_requested() && mach_absolute_time() < deadline) {
                const auto waited = mach_wait_until(deadline);
                if (waited != KERN_SUCCESS && waited != KERN_ABORTED) {
                    error.store(static_cast<int32_t>(waited));
                    running.store(false);
                    return;
                }
            }
        }
        running.store(false);
    }
};
RenderThread::RenderThread(SceneRenderer& renderer, RenderScheduleTiming timing)
    : impl_(std::make_unique<Impl>(renderer, timing)) {}
RenderThread::~RenderThread() {
    stop();
}
void RenderThread::start() {
    stop();
    impl_->error.store(0);
    impl_->realtime.store(false);
    impl_->workgroupJoined.store(false);
    impl_->workgroupError.store(0);
    impl_->thread = std::jthread([state = impl_.get()](std::stop_token stop) {
        state->run(stop);
    });
}
void RenderThread::stop() noexcept {
    if (impl_->thread.joinable()) {
        impl_->thread.request_stop();
        impl_->thread.join();
    }
}
bool RenderThread::active() const noexcept {
    return impl_->thread.joinable() && !impl_->error.load();
}
RenderThreadStatus RenderThread::status() const noexcept {
    const auto& self = *impl_;
    return {self.running.load(),        self.realtime.load(),        self.error.load(),
            self.blocks.load(),         self.frames.load(),          self.late.load(),
            self.skipped.load(),        self.reanchors.load(),       self.graphFailures.load(),
            self.sinkFailures.load(),   self.execution.load(),       self.lastBlock.load(),
            self.publishedFrame.load(), self.workgroupJoined.load(), self.workgroupError.load(),
            self.deadlineMisses.load(), self.maximumLateness.load()};
}
} // namespace loopz::macos
