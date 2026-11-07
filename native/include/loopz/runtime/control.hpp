#pragma once
#include <loopz/document_json.hpp>
#include <loopz/edit_history.hpp>
#include <loopz/runtime/audio_runtime.hpp>
#include <filesystem>
#include <optional>
namespace loopz::runtime {
class Control {
  public:
    explicit Control(std::filesystem::path state);
    JSON request(const JSON& request);
    void poll();

  private:
    JSON snapshot();
    void activate();
    AudioRuntime audio_;
    std::filesystem::path state_;
    std::string epoch_, activationError_, storageError_, inventoryError_;
    singz::AudioHostInventory inventory_;
    std::optional<RouteDocument> desired_;
    uint64_t desiredGeneration_{0}, activeGeneration_{0};
    EditHistory history_;
};
} // namespace loopz::runtime
