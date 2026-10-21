#include "wire.hpp"

#include <algorithm>
#include <utility>

namespace loopz::macos {
struct EditorConnection::Impl {
    EditorServiceClient client;
    uint64_t nextAttempt{0}, delay{100};
    bool ready{false}, previewRequested{false};
    std::string snapshot, error;
    explicit Impl(EditorServiceEndpoint endpoint) : client(std::move(endpoint)) {}
    void failed(const std::exception& failure) {
        ready = false;
        error = failure.what();
        if (!client.connected())
            client.disconnect();
        nextAttempt = editor::milliseconds() + delay;
        delay = std::min<uint64_t>(delay * 2, 5000);
    }
    void invalidateSnapshot() {
        ready = false;
        nextAttempt = 0;
    }
};
EditorConnection::EditorConnection(EditorServiceEndpoint endpoint)
    : impl_(std::make_unique<Impl>(std::move(endpoint))) {}
EditorConnection::~EditorConnection() = default;
void EditorConnection::poll() {
    auto& self = *impl_;
    if (!self.client.connected()) {
        self.ready = false;
        self.previewRequested = false;
    }
    if (editor::milliseconds() < self.nextAttempt)
        return;
    try {
        if (!self.client.connected())
            self.client.connect();
        if (!self.previewRequested) {
            self.client.preview();
            self.previewRequested = true;
        }
        auto snapshot = self.client.snapshot();
        self.snapshot.swap(snapshot);
        self.ready = true;
        self.error.clear();
        self.delay = 100;
        self.nextAttempt = editor::milliseconds() + 100;
    } catch (const std::exception& error) {
        self.failed(error);
    }
}
bool EditorConnection::ready() const noexcept {
    return impl_->ready && impl_->client.connected();
}
bool EditorConnection::connected() const noexcept {
    return impl_->client.connected();
}
const std::string& EditorConnection::snapshot() const noexcept {
    return impl_->snapshot;
}
const std::string& EditorConnection::lastError() const noexcept {
    return impl_->error;
}
std::string EditorConnection::edit(std::string_view request) {
    auto& self = *impl_;
    if (!ready())
        throw EditorServiceError(EditorFailure::Unavailable,
                                 "Refresh routing before making another edit");
    self.invalidateSnapshot();
    try {
        return self.client.edit(request);
    } catch (const std::exception& error) {
        self.failed(error);
        throw;
    }
}
void EditorConnection::retry() {
    auto& self = *impl_;
    if (!ready())
        throw EditorServiceError(EditorFailure::Unavailable,
                                 "Refresh routing before requesting recovery");
    self.invalidateSnapshot();
    try {
        self.client.retry();
    } catch (const std::exception& error) {
        self.failed(error);
        throw;
    }
}
std::string EditorConnection::initialize(std::string_view request) {
    auto& self = *impl_;
    if (!ready())
        throw EditorServiceError(EditorFailure::Unavailable,
                                 "Refresh routing before creating a document");
    self.invalidateSnapshot();
    try {
        return self.client.initialize(request);
    } catch (const std::exception& error) {
        self.failed(error);
        throw;
    }
}
std::string EditorConnection::hardwareBuffer(std::string_view request) {
    auto& self = *impl_;
    if (!ready())
        throw EditorServiceError(EditorFailure::Unavailable,
                                 "Refresh routing before changing the Engine buffer");
    self.invalidateSnapshot();
    try {
        return self.client.hardwareBuffer(request);
    } catch (const std::exception& error) {
        self.failed(error);
        throw;
    }
}
} // namespace loopz::macos
