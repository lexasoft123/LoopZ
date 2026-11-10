#pragma once
#include <loopz/document_json.hpp>
#include <loopz/edit_history.hpp>
#include <loopz/runtime/audio_runtime.hpp>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <memory>
namespace loopz::runtime {
// Borrowed control-owner view. Never read from an audio callback or retain the
// document across a request/poll that can mutate Control, or its destruction.
struct ControlRouteView {
    const RouteDocument* desired{nullptr};
    uint64_t desiredGeneration{0}, activeGeneration{0}, documentRevision{0};
};
namespace detail {
// Owned POSIX descriptor; movable so Control keeps its implicit move operations.
class OwnedDescriptor {
  public:
    OwnedDescriptor() = default;
    explicit OwnedDescriptor(int value) noexcept : value_(value) {}
    OwnedDescriptor(OwnedDescriptor&& other) noexcept : value_(other.release()) {}
    OwnedDescriptor& operator=(OwnedDescriptor&& other) noexcept;
    OwnedDescriptor(const OwnedDescriptor&) = delete;
    OwnedDescriptor& operator=(const OwnedDescriptor&) = delete;
    ~OwnedDescriptor();
    int get() const noexcept {
        return value_;
    }
    int release() noexcept {
        auto value = value_;
        value_ = -1;
        return value;
    }

  private:
    int value_{-1};
};
} // namespace detail
class Control {
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    // Minimum spacing of automatic poll() activation retries after a failure.
    // Explicit edits and retry requests still activate immediately.
    static constexpr std::chrono::seconds kActivationRetryInterval{2};
    explicit Control(std::filesystem::path state);
    // Takes exclusive runtime ownership. Bind native sources before construction.
    // Holds an advisory lock on "<state>.lock" for its lifetime; while another
    // owner holds it, Control fails closed as storage-unavailable.
    Control(std::filesystem::path state, std::unique_ptr<AudioRuntime> runtime);
    // Same, with an injected clock for poll() retry throttling.
    Control(std::filesystem::path state, std::unique_ptr<AudioRuntime> runtime, Clock clock);
    JSON request(const JSON& request);
    void poll();
    ControlRouteView routeView() const noexcept;
    // True while this Control holds the inter-process saved-routing lock.
    bool ownsState() const noexcept {
        return stateLock_.get() >= 0;
    }

  private:
    JSON snapshot();
    void activate();
    bool lockState();
    void load();
    void deferActivation(std::string error);
    // Declared first so the lock is released only after everything else.
    detail::OwnedDescriptor stateLock_;
    std::unique_ptr<AudioRuntime> audio_;
    std::filesystem::path state_;
    Clock clock_;
    std::string epoch_, activationError_, storageError_, inventoryError_;
    singz::AudioHostInventory inventory_;
    std::optional<RouteDocument> desired_;
    uint64_t desiredGeneration_{0}, activeGeneration_{0};
    uint64_t documentRevision_{0};
    // The desired document awaits a successful activation. Tracked separately
    // from generation equality so a hand-edited generation "0" still retries.
    bool activationPending_{false};
    std::chrono::steady_clock::time_point nextActivationRetry_{};
    EditHistory history_;
};
} // namespace loopz::runtime
