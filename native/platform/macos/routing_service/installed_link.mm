#include <loopz/macos/routing_service.hpp>

#include <stdexcept>

namespace loopz::macos {
namespace {
bool same(const RoutingServiceEndpoint& a, const RoutingServiceEndpoint& b) {
    return a.serviceName == b.serviceName && a.serverRequirement == b.serverRequirement &&
           a.serverUID == b.serverUID && a.privileged == b.privileged && a.endpoint == b.endpoint;
}
uint64_t after(uint64_t value, uint64_t delay) {
    return value > UINT64_MAX - delay ? UINT64_MAX : value + delay;
}
} // namespace
struct InstalledDriverLink::Impl {
    const EndpointLoader loader;
    RoutingServiceEndpoint endpoint;
    std::unique_ptr<RoutingDriverLink> peer;
    GraphRecovery discovery;
    uint64_t nextRead{0}, innerSession{0}, publishedSession{0}, nextSession{0};
    std::string error;
    explicit Impl(EndpointLoader read) : loader(std::move(read)) {
        if (!loader)
            throw std::invalid_argument("Installed service discovery needs a loader");
        discovery.setDesired({{"pin", {}}}, 0);
    }
    void read() {
        auto candidate = loader();
        if (!peer || !same(candidate, endpoint)) {
            innerSession = publishedSession = 0;
            peer.reset();
            endpoint = std::move(candidate);
            peer = std::make_unique<RoutingDriverLink>(endpoint);
        }
        error.clear();
    }
    void drop() noexcept {
        innerSession = publishedSession = 0;
        peer.reset();
    }
};
InstalledDriverLink::InstalledDriverLink(EndpointLoader loader)
    : impl_(std::make_unique<Impl>(std::move(loader))) {}
InstalledDriverLink::~InstalledDriverLink() = default;
void InstalledDriverLink::poll(uint64_t now) {
    auto& self = *impl_;
    if (self.discovery.ready("pin") && now >= self.nextRead) {
        try {
            self.read();
            self.nextRead = after(now, 1000);
        } catch (const std::exception& error) {
            self.error = error.what();
            self.drop();
            self.discovery.lost("pin", now);
        }
    }
    for (const auto& attempt : self.discovery.attempts(now)) {
        bool loaded = false;
        try {
            self.read();
            loaded = true;
            self.nextRead = after(now, 1000);
        } catch (const std::exception& error) {
            self.error = error.what();
            self.drop();
        }
        self.discovery.complete(attempt, loaded, now);
    }
    if (!self.peer)
        return;
    self.peer->poll(now);
    const auto inner = self.peer->session();
    if (!inner) {
        self.innerSession = self.publishedSession = 0;
        return;
    }
    if (inner != self.innerSession) {
        if (self.nextSession == UINT64_MAX)
            throw std::overflow_error("Installed service session exhausted");
        self.innerSession = inner;
        self.publishedSession = ++self.nextSession;
    }
}
uint64_t InstalledDriverLink::session() const noexcept {
    const auto& self = *impl_;
    return self.peer && self.innerSession && self.peer->session() == self.innerSession
               ? self.publishedSession
               : 0;
}
int32_t InstalledDriverLink::submit(std::span<const DeviceTransport> devices, uint64_t revision) {
    if (!session())
        throw std::runtime_error("Installed routing service is unavailable");
    return impl_->peer->submit(devices, revision);
}
uint32_t InstalledDriverLink::sampleRateMask() const noexcept {
    return session() ? impl_->peer->sampleRateMask() : kLegacyVirtualSampleRates;
}
DriverConfigurationResult InstalledDriverLink::status(uint64_t revision) {
    if (!session())
        throw std::runtime_error("Installed routing service is unavailable");
    return impl_->peer->status(revision);
}
int32_t InstalledDriverLink::cancel(uint64_t revision) {
    if (!session())
        throw std::runtime_error("Installed routing service is unavailable");
    return impl_->peer->cancel(revision);
}
void InstalledDriverLink::invalidate() noexcept {
    impl_->innerSession = impl_->publishedSession = 0;
    if (impl_->peer)
        impl_->peer->invalidate();
}
std::vector<DriverRateRequest> InstalledDriverLink::rateRequests() {
    if (!session())
        throw std::runtime_error("Installed routing service is unavailable");
    return impl_->peer->rateRequests();
}
void InstalledDriverLink::acknowledgeRates(std::span<const DriverRateRequest> requests) {
    if (!session())
        throw std::runtime_error("Installed routing service is unavailable");
    impl_->peer->acknowledgeRates(requests);
}
std::string InstalledDriverLink::lastError() const {
    if (!impl_->error.empty())
        return impl_->error;
    return impl_->peer ? impl_->peer->lastError() : "Installed routing service is unavailable";
}
} // namespace loopz::macos
