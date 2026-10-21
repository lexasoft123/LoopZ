#pragma once

#include <loopz/macos/scene_renderer.hpp>
#include <loopz/render_schedule.hpp>

namespace loopz::macos {
struct RenderThreadStatus {
    bool running{false}, realtimePolicy{false};
    int32_t error{0};
    uint64_t blocks{0}, frames{0}, lateFrames{0}, skippedFrames{0}, reanchors{0};
    uint64_t graphFailures{0}, sinkFailures{0}, maximumExecutionNs{0}, lastBlockHostNs{0};
    uint64_t publishedFrame{0};
    bool workgroupJoined{false};
    int32_t workgroupError{0};
    uint64_t publicationDeadlineMisses{0}, maximumPublicationLatenessNs{0};
};

// Only the control owner starts/stops/joins this thread. The thread renders the
// published scene at one common clock and never runs discovery, XPC, file IO,
// provider acquisition or scene preparation. A failed wait can be restarted
// without replacing the driver's session or healthy source/monitor owners.
class RenderThread {
  public:
    RenderThread(SceneRenderer& renderer, RenderScheduleTiming timing);
    ~RenderThread();
    RenderThread(const RenderThread&) = delete;
    RenderThread& operator=(const RenderThread&) = delete;
    void start();
    void stop() noexcept;
    bool active() const noexcept;
    RenderThreadStatus status() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz::macos
