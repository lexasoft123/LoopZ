#pragma once
#include <loopz/document_json.hpp>
#include <loopz/edit_history.hpp>
#include <loopz/runtime/audio_runtime.hpp>
#include <filesystem>
#include <optional>
#include <memory>
namespace loopz::runtime {
// Borrowed control-owner view. Never read from an audio callback or retain the
// document across a request/poll that can mutate Control, or its destruction.
struct ControlRouteView {
    const RouteDocument* desired{nullptr};
    uint64_t desiredGeneration{0}, activeGeneration{0}, documentRevision{0};
};
class Control {
  public:
    explicit Control(std::filesystem::path state);
    // Takes exclusive runtime ownership. Bind native sources before construction.
    Control(std::filesystem::path state, std::unique_ptr<AudioRuntime> runtime);
    JSON request(const JSON& request);
    void poll();
    ControlRouteView routeView() const noexcept;

  private:
    JSON snapshot();
    void activate();
    std::unique_ptr<AudioRuntime> audio_;
    std::filesystem::path state_;
    std::string epoch_, activationError_, storageError_, inventoryError_;
    singz::AudioHostInventory inventory_;
    std::optional<RouteDocument> desired_;
    uint64_t desiredGeneration_{0}, activeGeneration_{0};
    uint64_t documentRevision_{0};
    EditHistory history_;
};
} // namespace loopz::runtime
