#pragma once

#include <loopz/macos/editor_protocol.hpp>
#include <xpc/xpc.h>

#include <functional>

namespace loopz::macos {

inline constexpr char kEditorServiceName[] = "com.lexasoft.loopz.editor.control";
inline constexpr size_t kMaximumEditorPeers = 8;
inline constexpr size_t kMaximumEditorOutstanding = 16;
inline constexpr size_t kMaximumEditorPeerOutstanding = 4;
inline constexpr size_t kMaximumEditorReplyBytes = 20 * 1024 * 1024;

struct EditorServicePolicy {
    std::string clientRequirement;
    uint32_t clientUID{UINT32_MAX};
};
struct EditorServiceEndpoint {
    std::string serviceName{kEditorServiceName};
    std::string serverRequirement;
    uint32_t serverUID{UINT32_MAX};
    xpc_endpoint_t endpoint{nullptr};
};
// Fixed production identities; anonymous fixtures supply their own exact code
// requirements. No production unsigned or same-UID-only fallback exists.
EditorServicePolicy productionEditorPolicy();
EditorServiceEndpoint productionEditorEndpoint();

struct EditorOperations {
    std::function<std::string()> snapshot;
    std::function<std::string(std::string_view)> edit;
    std::function<void()> retry;
    std::function<std::string(std::string_view)> initialize{};
    std::function<void(bool)> preview{};
    std::function<std::string(std::string_view)> hardwareBuffer{};
    // Optional thread-safe scheduler notification. XPC callbacks may invoke it
    // after queueing work or losing a peer; it must not access runtime state.
    std::function<void()> wake{};
};
class EngineRuntime;
EditorOperations runtimeEditorOperations(EngineRuntime& runtime);
struct EditorServiceStatus {
    bool listening;
    size_t peers, outstanding, inFlight;
    uint64_t completed, rejected, expired;
};

// XPC callbacks authenticate and enqueue only. The engine's single control
// owner calls poll() to execute at most one operation, independently of render
// callbacks. Queued work expires after two seconds; disconnected peers lose
// unstarted work. stop/destruction belong to that same control owner.
class EditorService {
  public:
    static std::unique_ptr<EditorService> anonymous(EditorServicePolicy policy,
                                                    EditorOperations operations);
    static std::unique_ptr<EditorService> machService(EditorServicePolicy policy,
                                                      EditorOperations operations,
                                                      std::string name = kEditorServiceName);
    ~EditorService();
    XPC_RETURNS_RETAINED xpc_endpoint_t endpoint() const;
    void poll();
    EditorServiceStatus status() const;
    void stop() noexcept;

  private:
    struct Impl;
    explicit EditorService(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

enum class EditorFailure { Busy, Expired, Conflict, Invalid, Unavailable, Failed };
class EditorServiceError : public std::runtime_error {
  public:
    EditorServiceError(EditorFailure code, std::string detail)
        : std::runtime_error(std::move(detail)), code(code) {}
    const EditorFailure code;
};
class UncertainEditorOutcome : public std::runtime_error {
  public:
    UncertainEditorOutcome()
        : std::runtime_error(
              "Editor reply was lost; refresh routing before deciding whether to retry") {}
};

// A background/UI transport caller owns this object serially. connect() performs
// a non-mutating handshake and verifies the server's pinned code and UID before
// any editor operation is sent. Calls wait at most three seconds. A timeout
// disconnects and never automatically resubmits an edit with an uncertain result.
class EditorServiceClient {
  public:
    explicit EditorServiceClient(EditorServiceEndpoint endpoint);
    ~EditorServiceClient();
    void connect();
    void disconnect() noexcept;
    bool connected() const noexcept;
    std::string snapshot();
    void preview();
    std::string edit(std::string_view request);
    std::string initialize(std::string_view request);
    std::string hardwareBuffer(std::string_view request);
    void retry();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The desktop's serial background transport owner calls poll(). Reconnect and
// read retries back off from 100 ms to 5 seconds. Cached state remains available
// for display, but ready() stays false until a fresh snapshot succeeds. Edits
// and recovery commands are sent once and are never replayed by poll().
class EditorConnection {
  public:
    explicit EditorConnection(EditorServiceEndpoint endpoint);
    ~EditorConnection();
    void poll();
    bool ready() const noexcept;
    bool connected() const noexcept;
    const std::string& snapshot() const noexcept;
    const std::string& lastError() const noexcept;
    std::string edit(std::string_view request);
    std::string initialize(std::string_view request);
    std::string hardwareBuffer(std::string_view request);
    void retry();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace loopz::macos
