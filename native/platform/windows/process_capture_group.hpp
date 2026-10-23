#pragma once
#include "app_routing_process.hpp"
#include "process_capture.hpp"
#include <loopz/application_processes.hpp>
#include <vector>
namespace loopz::windows {
// Control-thread ancestry evidence. Retained process handles keep ancestor IDs
// from being reused after exit; a later plan can authenticate surviving children.
class ProcessCapturePlan final {
  public:
    explicit ProcessCapturePlan(const std::vector<std::shared_ptr<AppRoutingProcess>>&,
                                const ProcessCapturePlan* previous = nullptr);
    ~ProcessCapturePlan();
    const std::vector<ProcessInstance>& roots() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Read-only control-thread ancestry plan, also usable before opening capture.
std::vector<ProcessInstance>
processCaptureRoots(const std::vector<std::shared_ptr<AppRoutingProcess>>&);
// Control-owned authenticated membership; one process-tree capture per disjoint
// root. Readers own only histories and independent, prepared read state.
class ProcessCaptureGroup final {
  public:
    explicit ProcessCaptureGroup(std::vector<std::shared_ptr<AppRoutingProcess>>,
                                 const ProcessCaptureGroup* previous = nullptr);
    const std::string& application() const noexcept {
        return members_.front()->identity().application;
    }
    bool matches(const std::vector<std::shared_ptr<AppRoutingProcess>>&) const;
    std::string error() const;
    bool readyToRoute() const;
    std::shared_ptr<const AudioSource> reader() const;
    const std::vector<ProcessInstance>& roots() const noexcept {
        return plan_.roots();
    }

  private:
    struct Root {
        ProcessInstance identity;
        std::shared_ptr<ProcessCapture> capture;
    };
    std::vector<std::shared_ptr<AppRoutingProcess>> members_;
    std::vector<ProcessInstance> membership_;
    ProcessCapturePlan plan_;
    std::vector<Root> roots_;
};
} // namespace loopz::windows
